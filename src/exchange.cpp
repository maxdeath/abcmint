#include "exchange.h"
#include "init.h"
#include "util.h"
#include "main.h"

#include <string>
#include <unordered_map>


static MYSQL mysql;
static std::unordered_map<int,std::string> depositAddressMap;

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


MYSQL *ConnectMysql()
{
    MYSQL *conn = mysql_init(&mysql);
    if (conn == NULL)
        return NULL;

    my_bool reconnect = 1;
    if (mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect) != 0) {
        mysql_close(conn);
        return NULL;
    }
    if (mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8") != 0) {
        mysql_close(conn);
        return NULL;
    }

    const char* host =   GetArg("-host", "host").c_str();
    const char* user =   GetArg("-user", "user").c_str();
    const char* passwd = GetArg("-passwd", "passwd").c_str();
    const char* dbname = GetArg("-dbname", "dbname").c_str();
    int64 dbport = GetArg("-dbport", 3306);
    if (mysql_real_connect(conn, host, user, passwd, dbname, dbport, NULL, 0) == NULL) {
        mysql_close(conn);
        return NULL;
    }

    return conn;
}

bool LoadDepositAddress()
{
    char query[64];
    sprintf(query, "select * from coin_abc");
    int ret = mysql_query(&mysql, query);
    if(ret != 0) {
        printf("Query failed (%s)\n",mysql_error(&mysql));
        return false;
    }

    MYSQL_RES *res;
    if (!(res=mysql_store_result(&mysql)))
    {
        printf("Couldn't get result from %s\n", mysql_error(&mysql));
        return false;
    }

    size_t num_rows = mysql_num_rows(res);
    for (size_t i = 0; i < num_rows; ++i) {
        MYSQL_ROW row = mysql_fetch_row(res);
        int userId = atoi(row[0]);
        depositAddressMap[userId] = row[1];
    }

    mysql_free_result(res);
    return true;
}
