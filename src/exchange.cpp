#include "exchange.h"
#include "init.h"
#include "util.h"


void FillKeyPool(boost::thread_group& threadGroup)
{
    printf("FillKeyPool started\n");
    RenameThread("FillKeyPool");
    SetThreadPriority(THREAD_PRIORITY_NORMAL);

    unsigned int nTargetSize = GetArg("-keypool", KEY_POOL_SIZE);
    try {
        while (true) {
            if (pwalletMain->setKeyPool.size() < nTargetSize/2) {
                if (pwalletMain->IsLocked()) {
                    //getnewaddress will return new address, but will timeout
                    //consider how to handle this error
                    printf("error: please unlock the wallet for key pool filling!\n");
                }
                pwalletMain->TopUpKeyPool(true);

                //MilliSleep(3*1000);//sleep a little while and check if the key pool is used out, 10 seconds
            } else
                MilliSleep(10*1000);//sleep a loog while if the key pool still have a lot, 10 minutes
        }
    }
    catch (boost::thread_interrupted)
    {
        printf("FillKeyPool terminated\n");
        throw;
    }
}