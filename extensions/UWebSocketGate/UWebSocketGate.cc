/*
 * Copyright (c) 2017 Pavel Vainerman.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, version 2.1.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Lesser Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
// --------------------------------------------------------------------------
/*! \file
 *  \author Pavel Vainerman
*/
// --------------------------------------------------------------------------
#include <sstream>
#include <iomanip>
#include <unistd.h>

#include "unisetstd.h"
#include <Poco/Net/NetException.h>
#include <Poco/Net/WebSocket.h>
#include "ujson.h"
#include "UWebSocketGate.h"
#include "Configuration.h"
#include "Exceptions.h"
#include "UHelpers.h"
#include "Debug.h"
#include "UniXML.h"
#include "ORepHelpers.h"
#include "UWebSocketGateSugar.h"
#include "SMonitor.h"
// --------------------------------------------------------------------------
using namespace uniset;
using namespace std;
// --------------------------------------------------------------------------
UWebSocketGate::UWebSocketGate( uniset::ObjectId id, xmlNode* cnode, const string& prefix ):
    UniSetObject(id)
{
    offThread(); // отключаем поток обработки, потому-что будем обрабатывать сами

    auto conf = uniset_conf();

    mylog = make_shared<DebugStream>();
    mylog->setLogName(myname);
    {
        ostringstream s;
        s << prefix << "log";
        conf->initLogStream(mylog, s.str());
    }

    UniXML::iterator it(cnode);

    maxwsocks = conf->getArgPInt("--" + prefix + "ws-max", it.getProp("wsMax"), maxwsocks);

    wscmd = make_shared<ev::async>();
    wsactivate.set<UWebSocketGate, &UWebSocketGate::onActivate>(this);
    wscmd->set<UWebSocketGate, &UWebSocketGate::onCommand>(this);
    sigTERM.set<UWebSocketGate, &UWebSocketGate::onTerminate>(this);
    sigQUIT.set<UWebSocketGate, &UWebSocketGate::onTerminate>(this);
    sigINT.set<UWebSocketGate, &UWebSocketGate::onTerminate>(this);
    iocheck.set<UWebSocketGate, &UWebSocketGate::checkMessages>(this);

#ifndef DISABLE_REST_API
    wsHeartbeatTime_sec = (float)conf->getArgPInt("--" + prefix + "ws-heartbeat-time", it.getProp("wsPingTime"), wsHeartbeatTime_sec) / 1000.0;
    wsSendTime_sec = (float)conf->getArgPInt("--" + prefix + "ws-send-time", it.getProp("wsSendTime"), wsSendTime_sec) / 1000.0;
    wsMaxSend = conf->getArgPInt("--" + prefix + "ws-max-send", it.getProp("wsMaxSend"), wsMaxSend);

    httpHost = conf->getArgParam("--" + prefix + "httpserver-host", "localhost");
    httpPort = conf->getArgPInt("--" + prefix + "httpserver-port", 8080);
    httpCORS_allow = conf->getArgParam("--" + prefix + "httpserver-cors-allow", "*");

    mylog1 << myname << "(init): http server parameters " << httpHost << ":" << httpPort << endl;
    Poco::Net::SocketAddress sa(httpHost, httpPort);

    try
    {
        Poco::Net::HTTPServerParams* httpParams = new Poco::Net::HTTPServerParams;

        int maxQ = conf->getArgPInt("--" + prefix + "httpserver-max-queued", it.getProp("httpMaxQueued"), 100);
        int maxT = conf->getArgPInt("--" + prefix + "httpserver-max-threads", it.getProp("httpMaxThreads"), 3);

        httpParams->setMaxQueued(maxQ);
        httpParams->setMaxThreads(maxT);
        httpserv = std::make_shared<Poco::Net::HTTPServer>(new UWebSocketGateRequestHandlerFactory(this), Poco::Net::ServerSocket(sa), httpParams );
    }
    catch( std::exception& ex )
    {
        std::stringstream err;
        err << myname << "(init): " << httpHost << ":" << httpPort << " ERROR: " << ex.what();
        throw uniset::SystemError(err.str());
    }

#endif
}
//--------------------------------------------------------------------------------------------
UWebSocketGate::~UWebSocketGate()
{
    if( evIsActive() )
        evstop();

    if( httpserv )
        httpserv->stop();
}
//--------------------------------------------------------------------------------------------
void UWebSocketGate::onTerminate( ev::sig& evsig, int revents )
{
    if( EV_ERROR & revents )
    {
        mycrit << myname << "(onTerminate): invalid event" << endl;
        return;
    }

    myinfo << myname << "(onTerminate): terminate..." << endl;

    evsig.stop();

    //evsig.loop.break_loop();
    try
    {
        httpserv->stop();
    }
    catch( std::exception& ex )
    {
        myinfo << myname << "(onTerminate): "  << ex.what() << endl;
    }

    try
    {
        evstop();
    }
    catch( std::exception& ex )
    {
        myinfo << myname << "(onTerminate): "  << ex.what() << endl;

    }
}
//--------------------------------------------------------------------------------------------
void UWebSocketGate::checkMessages( ev::timer& t, int revents )
{
    if( EV_ERROR & revents )
        return;

    auto m = receiveMessage();

    if( m )
        processingMessage(m.get());
}
//--------------------------------------------------------------------------------------------
void UWebSocketGate::sensorInfo( const SensorMessage* sm )
{
    uniset_rwmutex_wrlock lock(wsocksMutex);

    for( auto&& s : wsocks )
        s->sensorInfo(sm);
}
//--------------------------------------------------------------------------------------------
UWebSocketGate::RespondFormat UWebSocketGate::from_string(const string& str)
{
    if( str == "json" )
        return RespondFormat::JSON;

    if( str == "txt" )
        return RespondFormat::TXT;

    if( str == "raw" )
        return RespondFormat::RAW;

    return RespondFormat::UNKNOWN;
}
//--------------------------------------------------------------------------------------------
UTCPCore::Buffer* UWebSocketGate::format( const SensorMessage* sm, const std::string& err, const RespondFormat fmt )
{
    if( fmt == RespondFormat::JSON )
        return to_json(sm, err);

    if( fmt == RespondFormat::TXT )
        return to_txt(sm, err);

    if( fmt == RespondFormat::RAW )
        return to_json(sm, err);

    return to_json(sm, err);
}
//--------------------------------------------------------------------------------------------
UTCPCore::Buffer* UWebSocketGate::to_json( const SensorMessage* sm, const std::string& err )
{
    Poco::JSON::Object::Ptr json = new Poco::JSON::Object();

    json->set("error", err);
    json->set("id", sm->id);
    json->set("value", sm->value);
    json->set("name", uniset::ORepHelpers::getShortName(uniset_conf()->oind->getMapName(sm->id)));
    json->set("sm_tv_sec", sm->sm_tv.tv_sec);
    json->set("sm_tv_nsec", sm->sm_tv.tv_nsec);
    json->set("type", uniset::iotype2str(sm->sensor_type));
    json->set("undefined", sm->undefined );
    json->set("supplier", sm->supplier );
    json->set("tv_sec", sm->tm.tv_sec);
    json->set("tv_nsec", sm->tm.tv_nsec);
    json->set("node", sm->node);

    Poco::JSON::Object::Ptr calibr = uniset::json::make_child(json, "calibration");
    calibr->set("cmin", sm->ci.minCal);
    calibr->set("cmax", sm->ci.maxCal);
    calibr->set("rmin", sm->ci.minRaw);
    calibr->set("rmax", sm->ci.maxRaw);
    calibr->set("precision", sm->ci.precision);

    ostringstream out;
    json->stringify(out);
    return new UTCPCore::Buffer(out.str());
}
//--------------------------------------------------------------------------------------------
UTCPCore::Buffer* UWebSocketGate::to_txt( const SensorMessage* sm, const std::string& err )
{
    ostringstream out;

    if( err.empty() )
        out << SMonitor::printEvent(sm) << endl;
    else
    {
        out << uniset::timeToString(sm->sm_tv.tv_sec)
            << "(" << setw(9) << sm->sm_tv.tv_nsec << ")"
            << " id=" << sm->id
            << " error=" << err
            << endl;
    }

    return new UTCPCore::Buffer(out.str());
}
//--------------------------------------------------------------------------------------------
UTCPCore::Buffer* UWebSocketGate::to_raw( const SensorMessage* sm, const std::string& err )
{
    return new UTCPCore::Buffer( (const unsigned char*)(sm), sizeof(*sm) );
}
//--------------------------------------------------------------------------------------------
std::shared_ptr<UWebSocketGate> UWebSocketGate::init_wsgate( int argc, const char* const* argv, const std::string& prefix )
{
    string name = uniset::getArgParam("--" + prefix + "name", argc, argv, "UWebSocketGate");

    if( name.empty() )
    {
        cerr << "(UWebSocketGate): Unknown name. Use --" << prefix << "name" << endl;
        return nullptr;
    }

    return uniset::make_object<UWebSocketGate>(name, "UWebSocketGate", prefix);
}
// -----------------------------------------------------------------------------
void UWebSocketGate::help_print()
{
    cout << "Default: prefix='ws'" << endl;
    cout << "--prefix-name name                   - Имя. Для поиска настроечной секции в configure.xml" << endl;

    cout << "websockets: " << endl;
    cout << "--prefix-ws-max num                  - Максимальное количество websocket-ов" << endl;
    cout << "--prefix-ws-heartbeat-time msec      - Период сердцебиения в соединении. По умолчанию: 3000 мсек" << endl;
    cout << "--prefix-ws-send-time msec           - Период посылки сообщений. По умолчанию: 500 мсек" << endl;
    cout << "--prefix-ws-max num                  - Максимальное число сообщений посылаемых за один раз. По умолчанию: 200" << endl;

    cout << "http: " << endl;
    cout << "--prefix-httpserver-host ip             - IP на котором слушает http сервер. По умолчанию: localhost" << endl;
    cout << "--prefix-httpserver-port num            - Порт на котором принимать запросы. По умолчанию: 8080" << endl;
    cout << "--prefix-httpserver-max-queued num      - Размер очереди запросов к http серверу. По умолчанию: 100" << endl;
    cout << "--prefix-httpserver-max-threads num     - Разрешённое количество потоков для http-сервера. По умолчанию: 3" << endl;
    cout << "--prefix-httpserver-cors-allow addr     - (CORS): Access-Control-Allow-Origin. Default: *" << endl;
}
// -----------------------------------------------------------------------------
void UWebSocketGate::run( bool async )
{
    if( httpserv )
        httpserv->start();

    if( async )
        async_evrun();
    else
        evrun();
}
// -----------------------------------------------------------------------------
void UWebSocketGate::evfinish()
{
    wsactivate.stop();
    iocheck.stop();
    wscmd->stop();
}
// -----------------------------------------------------------------------------
void UWebSocketGate::evprepare()
{
    wsactivate.set(loop);
    wsactivate.start();

    wscmd->set(loop);
    wscmd->start();

    sigTERM.set(loop);
    sigTERM.start(SIGTERM);
    sigQUIT.set(loop);
    sigQUIT.start(SIGQUIT);
    sigINT.set(loop);
    sigINT.start(SIGINT);

    iocheck.set(loop);
    iocheck.start(0, check_sec);
}
// -----------------------------------------------------------------------------
void UWebSocketGate::onActivate( ev::async& watcher, int revents )
{
    if (EV_ERROR & revents)
    {
        mycrit << myname << "(UWebSocketGate::onActivate): invalid event" << endl;
        return;
    }

    uniset_rwmutex_rlock lk(wsocksMutex);

    for( const auto& s : wsocks )
    {
        if( !s->isActive() )
        {
            s->doCommand(ui);
            s->set(loop, wscmd);
        }
    }
}
// -----------------------------------------------------------------------------
void UWebSocketGate::onCommand( ev::async& watcher, int revents )
{
    if (EV_ERROR & revents)
    {
        mycrit << myname << "(UWebSocketGate::onCommand): invalid event" << endl;
        return;
    }

    uniset_rwmutex_rlock lk(wsocksMutex);

    for( const auto& s : wsocks )
        s->doCommand(ui);
}
// -----------------------------------------------------------------------------
#ifndef DISABLE_REST_API
// -----------------------------------------------------------------------------
class UWebSocketGateRequestHandler:
    public Poco::Net::HTTPRequestHandler
{
    public:

        UWebSocketGateRequestHandler( UWebSocketGate* l ): wsgate(l) {}

        virtual void handleRequest( Poco::Net::HTTPServerRequest& request,
                                    Poco::Net::HTTPServerResponse& response ) override
        {
            wsgate->handleRequest(request, response);
        }

    private:
        UWebSocketGate* wsgate;
};
// -----------------------------------------------------------------------------
class UWebSocketGateWebSocketRequestHandler:
    public Poco::Net::HTTPRequestHandler
{
    public:

        UWebSocketGateWebSocketRequestHandler( UWebSocketGate* l ): wsgate(l) {}

        virtual void handleRequest( Poco::Net::HTTPServerRequest& request,
                                    Poco::Net::HTTPServerResponse& response ) override
        {
            wsgate->onWebSocketSession(request, response);
        }

    private:
        UWebSocketGate* wsgate;
};
// -----------------------------------------------------------------------------
Poco::Net::HTTPRequestHandler* UWebSocketGate::UWebSocketGateRequestHandlerFactory::createRequestHandler( const Poco::Net::HTTPServerRequest& req )
{
    if( req.find("Upgrade") != req.end() && Poco::icompare(req["Upgrade"], "websocket") == 0 )
        return new UWebSocketGateWebSocketRequestHandler(wsgate);

    return new UWebSocketGateRequestHandler(wsgate);
}
// -----------------------------------------------------------------------------
void UWebSocketGate::makeResponseAccessHeader( Poco::Net::HTTPServerResponse& resp )
{
    resp.set("Access-Control-Allow-Methods", "GET");
    resp.set("Access-Control-Allow-Request-Method", "*");
    resp.set("Access-Control-Allow-Origin", httpCORS_allow /* req.get("Origin") */);

    //  header('Access-Control-Allow-Credentials: true');
    //  header('Access-Control-Allow-Headers: Origin, X-Requested-With, Content-Type, Accept, Authorization');
}
// -----------------------------------------------------------------------------
void UWebSocketGate::handleRequest( Poco::Net::HTTPServerRequest& req, Poco::Net::HTTPServerResponse& resp )
{
    using Poco::Net::HTTPResponse;

    std::ostream& out = resp.send();

    makeResponseAccessHeader(resp);

    // В этой версии API поддерживается только GET
    if( req.getMethod() != "GET" )
    {
        auto jdata = respError(resp, HTTPResponse::HTTP_BAD_REQUEST, "method must be 'GET'");
        jdata->stringify(out);
        out.flush();
        return;
    }

    resp.setContentType("text/json");

    Poco::URI uri(req.getURI());

    mylog3 << req.getHost() << ": query: " << uri.getQuery() << endl;

    std::vector<std::string> seg;
    uri.getPathSegments(seg);

    // проверка подключения к страничке со списком websocket-ов
    if( !seg.empty() && seg[0] == "wsgate" )
    {
        if( seg.size() > 2 )
        {
            if( seg[1] == "json" || seg[1] == "txt" || seg[1] == "raw" )
            {
                ostringstream params;
                params << seg[2] << "&format=" << seg[1];
                httpWebSocketConnectPage(out, req, resp, params.str());
            }
            else
            {
                auto jdata = respError(resp, HTTPResponse::HTTP_BAD_REQUEST, "Unknown format. Must be [json,txt,raw]");
                jdata->stringify(out);
                out.flush();
            }

            return;
        }

        if( seg.size() > 1 )
        {
            if( seg[1] == "json" || seg[1] == "txt" || seg[1] == "raw" )
            {
                ostringstream params;
                auto qp = uri.getQueryParameters();

                for( const auto& p : qp )
                {
                    params << p.first;

                    if( !p.second.empty() )
                        params << "=" << p.second;

                    params << "&";
                }

                params << "format=" << seg[1];
                httpWebSocketConnectPage(out, req, resp, params.str());
            }
            else
                httpWebSocketConnectPage(out, req, resp, seg[1]);

            return;
        }
    }

    // default page
    httpWebSocketPage(out, req, resp);

    out.flush();
}
// -----------------------------------------------------------------------------
Poco::JSON::Object::Ptr UWebSocketGate::respError( Poco::Net::HTTPServerResponse& resp,
        Poco::Net::HTTPResponse::HTTPStatus estatus,
        const string& message )
{
    makeResponseAccessHeader(resp);
    resp.setStatus(estatus);
    resp.setContentType("text/json");
    Poco::JSON::Object::Ptr jdata = new Poco::JSON::Object();
    jdata->set("error", resp.getReasonForStatus(resp.getStatus()));
    jdata->set("ecode", (int)resp.getStatus());
    jdata->set("message", message);
    return jdata;
}
// -----------------------------------------------------------------------------
void UWebSocketGate::onWebSocketSession(Poco::Net::HTTPServerRequest& req, Poco::Net::HTTPServerResponse& resp)
{
    using Poco::Net::WebSocket;
    using Poco::Net::WebSocketException;
    using Poco::Net::HTTPResponse;
    using Poco::Net::HTTPServerRequest;

    std::vector<std::string> seg;

    makeResponseAccessHeader(resp);

    Poco::URI uri(req.getURI());

    uri.getPathSegments(seg);

    mylog3 << req.getHost() << ": WSOCKET: " << uri.getQuery() << endl;

    // example: ws://host:port/wsgate/?s1,s2,s3,s4&format=[json,txt,raw]
    if( seg.empty() || seg[0] != "wsgate" )
    {
        resp.setStatus(HTTPResponse::HTTP_BAD_REQUEST);
        resp.setContentType("text/html");
        resp.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
        resp.setContentLength(0);
        std::ostream& err = resp.send();
        err << "Bad request. Must be:  ws://host:port/wsgate/?s1,s2,s3,s4&format=[json,txt,raw]";
        err.flush();
        return;
    }

    {
        uniset_rwmutex_rlock lk(wsocksMutex);

        if( wsocks.size() >= maxwsocks )
        {
            resp.setStatus(HTTPResponse::HTTP_SERVICE_UNAVAILABLE);
            resp.setContentType("text/html");
            resp.setStatusAndReason(HTTPResponse::HTTP_SERVICE_UNAVAILABLE);
            resp.setContentLength(0);
            std::ostream& err = resp.send();
            err << "Error: exceeding the maximum number of open connections (" << maxwsocks << ")";
            err.flush();
            return;
        }
    }

    auto qp = uri.getQueryParameters();
    auto ws = newWebSocket(&req, &resp, qp);

    if( !ws )
    {
        mywarn << myname << "(onWebSocketSession): failed create socket.." << endl;
        return;
    }

    UWebSocketGuard lk(ws, this);

    mylog3 << myname << "(onWebSocketSession): start session for " << req.clientAddress().toString() << endl;

    // т.к. вся работа происходит в eventloop
    // то здесь просто ждём..
    ws->waitCompletion();

    mylog3 << myname << "(onWebSocketSession): finish session for " << req.clientAddress().toString() << endl;
}
// -----------------------------------------------------------------------------
bool UWebSocketGate::activateObject()
{
    bool ret = UniSetObject::activateObject();

    if( ret )
        run(true);

    return ret;
}
// -----------------------------------------------------------------------------
std::shared_ptr<UWebSocketGate::UWebSocket> UWebSocketGate::newWebSocket( Poco::Net::HTTPServerRequest* req,
        Poco::Net::HTTPServerResponse* resp, const Poco::URI::QueryParameters& qp )
{
    using Poco::Net::WebSocket;
    using Poco::Net::WebSocketException;
    using Poco::Net::HTTPResponse;
    using Poco::Net::HTTPServerRequest;

    std::shared_ptr<UWebSocket> ws;

    RespondFormat fmt = RespondFormat::JSON;

    std::string slist("");

    for( const auto& p : qp )
    {
        // обрабатываем только первый встреченный параметр
        if( p.first == "format" )
            fmt = from_string(p.second);
        else if( p.second.empty() && !p.first.empty() )
            slist += ("," + p.first);
    }

    if( qp.size() == 1 && qp[0].first.empty() )
        slist = qp[0].first;

	auto idlist = uniset::explode(slist);
    if( idlist.empty() )
    {
        resp->setStatus(HTTPResponse::HTTP_BAD_REQUEST);
        resp->setContentType("text/html");
        resp->setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
        resp->setContentLength(0);
        std::ostream& err = resp->send();
        err << "Error: no list of sensors for '" << slist << "'. Use:  http://host:port/wsgate/?s1,s2,s3";
        err.flush();

        mywarn << myname << "(newWebSocket): error: no list of sensors for '" << slist << "'" << endl;

        return nullptr;
    }

    {
        uniset_rwmutex_wrlock lock(wsocksMutex);
        ws = make_shared<UWebSocket>(req, resp);
        ws->setHearbeatTime(wsHeartbeatTime_sec);
        ws->setSendPeriod(wsSendTime_sec);
        ws->setMaxSendCount(wsMaxSend);
        ws->mylog = mylog;

        ws->setRespondFormat(fmt);

        for( const auto& i : idlist.getList() )
        {
            mylog3 << myname << ": add " << i << endl;
            UWebSocket::sinfo si;
            si.id = i;
            si.cmd = "ask";
            ws->add(si);
        }

        wsocks.emplace_back(ws);
    }

    // wsocksMutex надо отпустить, прежде чем посылать сигнал
    // т.к. в обработчике происходит его захват
    wsactivate.send();
    return ws;
}
// -----------------------------------------------------------------------------
void UWebSocketGate::delWebSocket(std::shared_ptr<UWebSocket>& ws )
{
    uniset_rwmutex_wrlock lock(wsocksMutex);

    for( auto it = wsocks.begin(); it != wsocks.end(); it++ )
    {
        if( (*it).get() == ws.get() )
        {
            mylog3 << myname << ": delete websocket " << endl;
            wsocks.erase(it);
            return;
        }
    }
}
// -----------------------------------------------------------------------------
const std::string UWebSocketGate::UWebSocket::ping_str = { "." };

UWebSocketGate::UWebSocket::UWebSocket(Poco::Net::HTTPServerRequest* _req,
                                       Poco::Net::HTTPServerResponse* _resp):
    Poco::Net::WebSocket(*_req, *_resp),
    req(_req),
    resp(_resp)
{
    setBlocking(false);

    cancelled = false;

    // т.к. создание websocket-а происходит в другом потоке
    // то активация и привязка к loop происходит в функции set()
    // вызываемой из eventloop
    ioping.set<UWebSocketGate::UWebSocket, &UWebSocketGate::UWebSocket::ping>(this);
    iosend.set<UWebSocketGate::UWebSocket, &UWebSocketGate::UWebSocket::send>(this);
    iorecv.set<UWebSocketGate::UWebSocket, &UWebSocketGate::UWebSocket::read>(this);

    maxsize = maxsend * 10; // пока так

    setReceiveTimeout( uniset::PassiveTimer::millisecToPoco(recvTimeout));
}
// -----------------------------------------------------------------------------
UWebSocketGate::UWebSocket::~UWebSocket()
{
    if( !cancelled )
        term();

    // удаляем всё что осталось
    while(!wbuf.empty())
    {
        delete wbuf.front();
        wbuf.pop();
    }
}
// -----------------------------------------------------------------------------
bool UWebSocketGate::UWebSocket::isActive()
{
    return iosend.is_active();
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::set(ev::dynamic_loop& loop, std::shared_ptr<ev::async> a )
{
    iosend.set(loop);
    ioping.set(loop);

    iosend.start(0, send_sec);
    ioping.start(ping_sec, ping_sec);

    iorecv.set(loop);
    iorecv.start(sockfd(), ev::READ);

    cmdsignal = a;
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::send( ev::timer& t, int revents )
{
    if( EV_ERROR & revents )
        return;

    for( size_t i = 0; !wbuf.empty() && i < maxsend && !cancelled; i++ )
        write();

    //  read(iorecv,revents);
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::ping( ev::timer& t, int revents )
{
    if( EV_ERROR & revents )
        return;

    if( cancelled )
        return;

    if( !wbuf.empty() )
    {
        ioping.stop();
        return;
    }

    wbuf.emplace(new UTCPCore::Buffer(ping_str));

    if( ioping.is_active() )
        ioping.stop();
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::read( ev::io& io, int revents )
{
    if( EV_ERROR & revents )
        return;

    if( !(revents & EV_READ) )
        return;

    using Poco::Net::WebSocket;
    using Poco::Net::WebSocketException;
    using Poco::Net::HTTPResponse;
    using Poco::Net::HTTPServerRequest;

    int flags = 0; // WebSocket::FRAME_FLAG_FIN | WebSocket::FRAME_OP_TEXT;

    try
    {
        if( available() <= 0 )
            return;

        int n = receiveFrame(rbuf, sizeof(rbuf), flags);
        //      int n = receiveBytes(rbuf, sizeof(rbuf));

        if( n <= 0 )
            return;

        const std::string cmd(rbuf, n);

        if( cmd == ping_str )
            return;

        onCommand(cmd);

        // откладываем ping, т.к. что-то в канале и так было
        ioping.start(ping_sec, ping_sec);
    }
    catch( WebSocketException& exc )
    {
        switch( exc.code() )
        {
            case WebSocket::WS_ERR_HANDSHAKE_UNSUPPORTED_VERSION:
                resp->set("Sec-WebSocket-Version", WebSocket::WEBSOCKET_VERSION);

            case WebSocket::WS_ERR_NO_HANDSHAKE:
            case WebSocket::WS_ERR_HANDSHAKE_NO_VERSION:
            case WebSocket::WS_ERR_HANDSHAKE_NO_KEY:
                resp->setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
                resp->setContentLength(0);
                resp->send();
                break;
        }
    }
    catch( const Poco::Net::NetException& e )
    {
        mylog3 << "(websocket):NetException: "
               << req->clientAddress().toString()
               << " error: " << e.displayText()
               << endl;
    }
    catch( Poco::IOException& ex )
    {
        mylog3 << "(websocket): IOException: "
               << req->clientAddress().toString()
               << " error: " << ex.displayText()
               << endl;
    }
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::add( const sinfo& si )
{
    smap[si.id] = si;
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::del( uniset::ObjectId id )
{
    auto s = smap.find(id);

    if( s != smap.end() )
        s->second.cmd = "del";
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::set( uniset::ObjectId id, long value )
{
    auto s = smap.find(id);

    if( s != smap.end() )
    {
        s->second.value = value;
        s->second.cmd = "set";
    }
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::sensorInfo( const uniset::SensorMessage* sm )
{
    if( cancelled )
        return;

    auto s = smap.find(sm->id);

    if( s == smap.end() )
        return;

    if( wbuf.size() > maxsize )
    {
        mywarn << req->clientAddress().toString() << " lost messages..." << endl;
        return;
    }

    wbuf.emplace(UWebSocketGate::format(sm, s->second.err, fmt));

    if( ioping.is_active() )
        ioping.stop();
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::doCommand( const std::shared_ptr<UInterface>& ui )
{
    for( auto&& io : smap )
    {
        auto& s = io.second;

        try
        {
            if( s.cmd == "" )
                continue;

            if( s.cmd == "ask" )
                ui->askSensor(s.id, UniversalIO::UIONotify);
            else if( s.cmd == "del" )
                ui->askSensor(s.id, UniversalIO::UIODontNotify);
            else if( s.cmd == "set" )
                ui->setValue(s.id, s.value);

            s.err = "";
            s.cmd = "";
        }
        catch( std::exception& ex )
        {
            mycrit << "(UWebSocket::doCommand): " << ex.what() << endl;
            sendError(s, ex.what());
        }
    }
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::sendError( sinfo& si, const std::string& err )
{
    uniset::SensorMessage sm(si.id, 0);
    //  sm.undefined = true;
    si.err = err;
    sensorInfo(&sm);
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::onCommand( const string& cmdtxt )
{
    const string cmd = cmdtxt.substr(0, 3);
    const string params = cmdtxt.substr(4, cmdtxt.size());

    myinfo << "(websocket): " << req->clientAddress().toString()
           << "(" << cmd << "): " << params << endl;

    if( cmd == "set" )
    {
        myinfo << "(websocket): " << req->clientAddress().toString()
               << "(set): " << params << endl;

        auto idlist = uniset::getSInfoList(params, uniset_conf());

        for( const auto& i : idlist )
            set(i.si.id, i.val);

        cmdsignal->send();
    }
    else if( cmd == "ask" )
    {
        myinfo << "(websocket): " << req->clientAddress().toString()
               << "(ask): " << params << endl;

        auto idlist = uniset::explode(params);

        for( const auto& id : idlist.getList() )
        {
            sinfo s;
            s.id = id;
            s.cmd = "ask";
            add(s);
        }

        // даём команду на перезаказ датчиков
        cmdsignal->send();
    }
    else if( cmd == "del" )
    {
        myinfo << "(websocket): " << req->clientAddress().toString()
               << "(del): " << params << endl;

        auto idlist = uniset::explode(params);

        for( const auto& id : idlist.getList() )
            del(id);

        // даём команду на перезаказ датчиков
        cmdsignal->send();
    }
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::write()
{
    UTCPCore::Buffer* msg = 0;

    if( wbuf.empty() )
    {
        if( !ioping.is_active() )
            ioping.start(ping_sec, ping_sec);

        return;
    }

    msg = wbuf.front();

    if( !msg )
        return;

    using Poco::Net::WebSocket;
    using Poco::Net::WebSocketException;
    using Poco::Net::HTTPResponse;
    using Poco::Net::HTTPServerRequest;

    int flags = WebSocket::FRAME_TEXT;

    if( msg->len == 1 ) // это пинг состоящий из "."
        flags = WebSocket::FRAME_FLAG_FIN | WebSocket::FRAME_OP_PING;

    try
    {
        ssize_t ret = sendFrame(msg->dpos(), msg->nbytes(), flags);

        if( ret < 0 )
        {
            mylog3 << "(websocket): " << req->clientAddress().toString()
                   << "  write to socket error(" << errno << "): " << strerror(errno) << endl;

            if( errno == EPIPE || errno == EBADF )
            {
                mylog3 << "(websocket): "
                       << req->clientAddress().toString()
                       << " write error.. terminate session.." << endl;

                term();
            }

            return;
        }

        msg->pos += ret;

        if( msg->nbytes() == 0 )
        {
            wbuf.pop();
            delete msg;
        }

        if( !wbuf.empty() )
        {
            if( ioping.is_active() )
                ioping.stop();
        }
        else
        {
            if( !ioping.is_active() )
                ioping.start(ping_sec, ping_sec);
        }

        return;
    }
    catch( WebSocketException& exc )
    {
        cerr << "(sendFrame): ERROR: " << exc.displayText() << endl;

        switch( exc.code() )
        {
            case WebSocket::WS_ERR_HANDSHAKE_UNSUPPORTED_VERSION:
                resp->set("Sec-WebSocket-Version", WebSocket::WEBSOCKET_VERSION);

            case WebSocket::WS_ERR_NO_HANDSHAKE:
            case WebSocket::WS_ERR_HANDSHAKE_NO_VERSION:
            case WebSocket::WS_ERR_HANDSHAKE_NO_KEY:
                resp->setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
                resp->setContentLength(0);
                resp->send();
                break;
        }
    }
    catch( const Poco::Net::NetException& e )
    {
        mylog3 << "(websocket):NetException: "
               << req->clientAddress().toString()
               << " error: " << e.displayText()
               << endl;
    }
    catch( Poco::IOException& ex )
    {
        mylog3 << "(websocket): IOException: "
               << req->clientAddress().toString()
               << " error: " << ex.displayText()
               << endl;
    }

    term();
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::term()
{
    if( cancelled )
        return;

    cancelled = true;
    ioping.stop();
    iosend.stop();
    iorecv.stop();
    finish.notify_all();
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::waitCompletion()
{
    std::unique_lock<std::mutex> lk(finishmut);

    while( !cancelled )
        finish.wait(lk);
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::setHearbeatTime( const double& sec )
{
    if( sec > 0 )
        ping_sec = sec;
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::setSendPeriod ( const double& sec )
{
    if( sec > 0 )
        send_sec = sec;
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::setMaxSendCount( size_t val )
{
    if( val > 0 )
        maxsend = val;
}
// -----------------------------------------------------------------------------
void UWebSocketGate::UWebSocket::setRespondFormat( UWebSocketGate::RespondFormat f )
{
    fmt = f;
}
// -----------------------------------------------------------------------------
void UWebSocketGate::httpWebSocketPage( std::ostream& ostr, Poco::Net::HTTPServerRequest& req, Poco::Net::HTTPServerResponse& resp )
{
    using Poco::Net::HTTPResponse;

    resp.setChunkedTransferEncoding(true);
    resp.setContentType("text/html");
    //  resp.

    ostr << "<html>" << endl;
    ostr << "<head>" << endl;
    ostr << "<title>" << myname << ": test page</title>" << endl;
    ostr << "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\">" << endl;
    ostr << "</head>" << endl;
    ostr << "<body>" << endl;
    ostr << "<h1>select sensors:</h1>" << endl;
    ostr << "<ul>" << endl;

    ostr << "  <li><a target='_blank' href=\"http://"
         << req.serverAddress().toString()
         << "/wsgate/json\">42,30,1042 [json]</a></li>"
         << endl;

    ostr << "  <li><a target='_blank' href=\"http://"
         << req.serverAddress().toString()
         << "/wsgate/txt\">42,30,1042 [txt]</a></li>"
         << endl;

    ostr << "</ul>" << endl;
    ostr << "</body>" << endl;
}
// -----------------------------------------------------------------------------
void UWebSocketGate::httpWebSocketConnectPage( ostream& ostr,
        Poco::Net::HTTPServerRequest& req,
        Poco::Net::HTTPServerResponse& resp,
        const std::string& params )
{
    resp.setChunkedTransferEncoding(true);
    resp.setContentType("text/html");

    // code base on example from
    // https://github.com/pocoproject/poco/blob/developNet/samples/WebSocketServer/src/WebSocketServer.cpp

    ostr << "<html>" << endl;
    ostr << "<head>" << endl;
    ostr << "<title>" << myname << ": sensors event</title>" << endl;
    ostr << "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\">" << endl;
    ostr << "<script type=\"text/javascript\">" << endl;
    ostr << "logscrollStopped = false;" << endl;
    ostr << "" << endl;
    ostr << "function clickScroll()" << endl;
    ostr << "{" << endl;
    ostr << "	if( logscrollStopped )" << endl;
    ostr << "		logscrollStopped = false;" << endl;
    ostr << "	else" << endl;
    ostr << "		logscrollStopped = true;" << endl;
    ostr << "}" << endl;
    ostr << "function LogAutoScroll()" << endl;
    ostr << "{" << endl;
    ostr << "   if( logscrollStopped == false )" << endl;
    ostr << "   {" << endl;
    ostr << "	   document.getElementById('end').scrollIntoView();" << endl;
    ostr << "   }" << endl;
    ostr << "}" << endl;
    ostr << "" << endl;
    ostr << "function WebSocketCreate()" << endl;
    ostr << "{" << endl;
    ostr << "  if (\"WebSocket\" in window)" << endl;
    ostr << "  {" << endl;
    ostr << "    var ws = new WebSocket(\"ws://" << req.serverAddress().toString() << "/wsgate/\");" << endl;
    ostr << "setInterval(send_cmd, 1000);" << endl;
    ostr << "    var l = document.getElementById('logname');" << endl;
    ostr << "    l.innerHTML = '*'" << endl;
    ostr << "ws.onopen = function() {" << endl;
    //          ostr << "ws.send(\"set:33=44,344=45\")" << endl;
    ostr << "};" << endl;

    ostr << "    ws.onmessage = function(evt)" << endl;
    ostr << "    {" << endl;
    ostr << "    	var p = document.getElementById('logs');" << endl;
    ostr << "    	if( evt.data != '.' ) {" << endl;
    ostr << "    		p.innerHTML = p.innerHTML + \"</br>\"+evt.data" << endl;
    ostr << "    		LogAutoScroll();" << endl;
    ostr << "    	}" << endl;
    ostr << "    };" << endl;
    ostr << "    ws.onclose = function()" << endl;
    ostr << "      { " << endl;
    ostr << "        alert(\"WebSocket closed.\");" << endl;
    ostr << "      };" << endl;
    ostr << "  }" << endl;
    ostr << "  else" << endl;
    ostr << "  {" << endl;
    ostr << "     alert(\"This browser does not support WebSockets.\");" << endl;
    ostr << "  }" << endl;

    ostr << "function send_cmd() {" << endl;
    ostr << "  ws.send( 'set:12,32,34' );" << endl;
    ostr << "}" << endl;

    ostr << "}" << endl;

    ostr << "</script>" << endl;
    ostr << "<style media='all' type='text/css'>" << endl;
    ostr << ".logs {" << endl;
    ostr << "	font-family: 'Liberation Mono', 'DejaVu Sans Mono', 'Courier New', monospace;" << endl;
    ostr << "	padding-top: 30px;" << endl;
    ostr << "}" << endl;
    ostr << "" << endl;
    ostr << ".logtitle {" << endl;
    ostr << "	position: fixed;" << endl;
    ostr << "	top: 0;" << endl;
    ostr << "	left: 0;" << endl;
    ostr << "	padding: 10px;" << endl;
    ostr << "	width: 100%;" << endl;
    ostr << "	height: 25px;" << endl;
    ostr << "	background-color: green;" << endl;
    ostr << "	border-top: 2px solid;" << endl;
    ostr << "	border-bottom: 2px solid;" << endl;
    ostr << "	border-color: white;" << endl;
    ostr << "}" << endl;
    ostr << "</style>" << endl;
    ostr << "</head>" << endl;
    ostr << "<body style='background: #111111; color: #ececec;' onload=\"javascript:WebSocketCreate()\">" << endl;
    ostr << "<h4><div onclick='javascritpt:clickScroll()' id='logname' class='logtitle'></div></h4>" << endl;
    ostr << "<div id='logs' class='logs'></div>" << endl;
    ostr << "<p><div id='end' style='display: hidden;'>&nbsp;</div></p>" << endl;
    ostr << "</body>" << endl;
}
// -----------------------------------------------------------------------------
#endif
// -----------------------------------------------------------------------------