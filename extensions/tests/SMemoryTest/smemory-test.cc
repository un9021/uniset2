#include <string>
#include "Debug.h"
#include "UniSetActivator.h"
#include "SharedMemory.h"
#include "Extensions.h"
#include "TestProc.h"
// --------------------------------------------------------------------------
using namespace std;
using namespace UniSetTypes;
using namespace UniSetExtensions;
// --------------------------------------------------------------------------
int main(int argc, const char **argv)
{
    if( argc>1 && ( strcmp(argv[1],"--help")==0 || strcmp(argv[1],"-h")==0 ) )
    {
        cout << "--confile    - Использовать указанный конф. файл. По умолчанию configure.xml" << endl;
        SharedMemory::help_print(argc, argv);
        return 0;
    }

    try
    {
        string confile = UniSetTypes::getArgParam( "--confile", argc, argv, "configure.xml" );
        conf = new Configuration(argc, argv, confile);

        conf->initDebug(dlog,"dlog");
        string logfilename = conf->getArgParam("--logfile", "smemory.log");
        string logname( conf->getLogDir() + logfilename );
        ulog.logFile( logname );
        dlog.logFile( logname );

        SharedMemory* shm = SharedMemory::init_smemory(argc, argv);
        if( !shm )
            return 1;

        UniSetActivator act;

        act.addObject(static_cast<class UniSetObject*>(shm));

        int num = conf->getArgPInt("--numproc",20);

		for( int i=1; i<=num; i++ )
		{
            ostringstream s;
            s << "TestProc" << i;

            cout << "..create " << s.str() << endl;
            TestProc* tp = new TestProc(conf->getObjectID(s.str()));
            tp->init_dlog(dlog);
            act.addObject(static_cast<class UniSetObject*>(tp));
        }

        SystemMessage sm(SystemMessage::StartUp);
        act.broadcast( sm.transport_msg() );
        act.run(false);

        return 0;
    }
    catch( SystemError& err )
    {
        ucrit << "(smemory): " << err << endl;
    }
    catch( Exception& ex )
    {
        ucrit << "(smemory): " << ex << endl;
    }
    catch( std::exception& e )
    {
        ucrit << "(smemory): " << e.what() << endl;
    }
    catch(...)
    {
        ucrit << "(smemory): catch(...)" << endl;
    }

    return 1;
}