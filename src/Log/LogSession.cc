#include <iostream>
#include <string>
#include <sstream>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <cc++/socket.h>
#include "LogSession.h"
#include "UniSetTypes.h"
#include "LogServerTypes.h"
#include "LogAgregator.h"
// -------------------------------------------------------------------------
using namespace std;
using namespace UniSetTypes;
// -------------------------------------------------------------------------
LogSession::~LogSession()
{
    cancelled = true;
    if( isRunning() )
    {
        disconnect();
        ost::Thread::join();
    }
}
// -------------------------------------------------------------------------
LogSession::LogSession( ost::TCPSocket &server, DebugStream* _log, timeout_t _sessTimeout, timeout_t _cmdTimeout, timeout_t _outTimeout, timeout_t _delay ):
TCPSession(server),
peername(""),
caddr(""),
log(_log),
sessTimeout(_sessTimeout),
cmdTimeout(_cmdTimeout),
outTimeout(_outTimeout),
delayTime(_delay),
cancelled(false)
{
    //slog.addLevel(Debug::ANY);
    log->signal_stream_event().connect( sigc::mem_fun(this, &LogSession::logOnEvent) );
}
// -------------------------------------------------------------------------
void  LogSession::logOnEvent( const std::string& s )
{
    uniset_rwmutex_wrlock l(mLBuf);
    lbuf.push_back(s);
}
// -------------------------------------------------------------------------
void LogSession::run()
{
    if( cancelled )
        return;

    {
        ost::tpport_t p;
        ost::InetAddress iaddr = getIPV4Peer(&p);

        // resolve..
        caddr = string( iaddr.getHostname() );

        ostringstream s;
        s << iaddr << ":" << p;
        peername = s.str();
    }

    if( slog.debugging(Debug::INFO) )
        slog[Debug::INFO] << peername << "(run): run thread of sessions.." << endl;

    ptSessionTimeout.setTiming(sessTimeout);

    setKeepAlive(true);
//    setTimeout(sessTimeout);

    // Команды могут посылаться только в начале сессии..
    if( isPending(Socket::pendingInput, cmdTimeout) )
    {
        LogServerTypes::lsMessage msg;
        // проверяем канал..(если данных нет, значит "клиент отвалился"...
        if( peek( (void*)(&msg),sizeof(msg)) > 0 )
        {
            ssize_t ret = readData( &msg,sizeof(msg) );

            if( ret!=sizeof(msg) || msg.magic!=LogServerTypes::MAGICNUM )
                slog.warn() << peername << "(run): BAD MESSAGE..." << endl;
            else
            {
                slog.info() << peername << "(run): receive command: '" << msg.cmd << "'" << endl;

                string cmdLogName(msg.logname);
                DebugStream* cmdlog = log;
                string logfile(log->getLogFile());

                if( !cmdLogName.empty () )
                {
                    LogAgregator* lag = dynamic_cast<LogAgregator*>(log);
                    if( lag )
                    {
                        LogAgregator::LogInfo inf = lag->getLogInfo(cmdLogName);
                        if( inf.log )
                        {
                            cmdlog = inf.log;
                            logfile = inf.logfile;
                        }
                        else
                        {
                              // если имя задали, но такого лога не нашлось
                              // то игнорируем команду
                              cmdlog = 0;
                              logfile = "";
                        }
                    }
                    else
                    {
                        // если имя лога задали, а оно не совпадает с текущим
                        // игнорируем команду
                        if( log->getLogFile() != cmdLogName )
                        {
                            cmdlog = 0;
                            logfile = "";
                        }
                    }
                }

                // обрабатываем команды только если нашли log
                if( cmdlog )
                {
                    // Обработка команд..
                    // \warning Работа с логом ведётся без mutex-а, хотя он разделяется отдельными потоками 
                    switch( msg.cmd )
                    {
                        case LogServerTypes::cmdSetLevel:
                            cmdlog->level( (Debug::type)msg.data );
                        break;
                        case LogServerTypes::cmdAddLevel:
                            cmdlog->addLevel( (Debug::type)msg.data );
                        break;
                        case LogServerTypes::cmdDelLevel:
                            cmdlog->delLevel( (Debug::type)msg.data );
                        break;

                        case LogServerTypes::cmdRotate:
                            if( !logfile.empty() )
                                 cmdlog->logFile(logfile,true);
                        break;

                        case LogServerTypes::cmdOffLogFile:
                        {
                            if( !logfile.empty() )
                                cmdlog->logFile("");
                        }
                        break;

                        case LogServerTypes::cmdOnLogFile:
                        {
                            if( !logfile.empty() )
                                cmdlog->logFile(logfile);
                        }
                        break;

                        default:
                            slog.warn() << peername << "(run): Unknown command '" << msg.cmd << "'" << endl;
                        break;
                    }
                }
            }
        }
    }

    cancelled = false;
    while( !cancelled && isConnected() ) // !ptSessionTimeout.checkTime()
    {
        // проверка только ради проверки "целостности" соединения
        if( isPending(Socket::pendingInput, 10) )
        {
            char buf[10];
            // проверяем канал..(если данных нет, значит "клиент отвалился"...
            if( peek(buf,sizeof(buf)) <=0 )
                break;
        }

        if( isPending(Socket::pendingOutput, outTimeout) )
        {
            //slog.info() << peername << "(run): send.." << endl;
      //      ptSessionTimeout.reset();

            // чтобы не застревать на посылке в сеть..
            // делаем через промежуточный буффер (stringstream)
            ostringstream sbuf;
            bool send = false;
            {
                uniset_rwmutex_wrlock l(mLBuf);
                if( !lbuf.empty() )
                {
                    slog.info() << peername << "(run): send messages.." << endl;
                    while( !lbuf.empty() )
                    {
                        sbuf << lbuf.front();
                        lbuf.pop_front();
                    }
                    send = true;
                }
            }

            if( send )
            {
                *tcp() << sbuf.str();
                tcp()->sync();
            }

            // чтобы постоянно не проверять... (надо переделать на condition)
            sleep(delayTime);
        }
    }

    if( slog.debugging(Debug::INFO) )
        slog[Debug::INFO] << peername << "(run): stop thread of sessions..disconnect.." << endl;

    disconnect();

    if( slog.debugging(Debug::INFO) )
        slog[Debug::INFO] << peername << "(run): thread stopping..." << endl;
}
// -------------------------------------------------------------------------
void LogSession::final()
{
    tcp()->sync();
    slFin(this);
    delete this;
}
// -------------------------------------------------------------------------
void LogSession::connectFinalSession( FinalSlot sl )
{
    slFin = sl;
}
// ---------------------------------------------------------------------