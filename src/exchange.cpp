#include "exchange.h"
#include "init.h"
#include "util.h"
#include "main.h"

#include <string>
#include <unordered_map>


static MYSQL mysql;
static std::unordered_map<int, std::string> depositKeyIdMap;
static std::unordered_map<uint256, std::unordered_map<KEY, int64, HashFunc, EqualKey>> chargeMap;

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

        //the transaction use the keyId
        std::set<CKeyID> setAddress;
        pwalletMain->GetKeys(setAddress);
        for (std::set<CKeyID>::iterator it = setAddress.begin(); it != setAddress.end(); ++it) {
            std::string address = CAbcmintAddress(*it).ToString();
            if (0 == strcmp(address.c_str(), row[1])) {
                //CKeyID ToString() will output in revert order, use HexStr(CScript), the same as transaction
                const CKeyID& id = *it;
                CScript s;
                s<<id;
                depositKeyIdMap[userId] = HexStr(s);
                break;
            }
        }
    }

    mysql_free_result(res);
    return true;
}

bool CallExchangeServer(std::string strRequest)
{

    // Connect to localhost
    bool fUseSSL = GetBoolArg("-rpcssl");
    asio::io_service io_service;
    ssl::context context(io_service, ssl::context::sslv23);
    context.set_options(ssl::context::no_sslv2);
    asio::ssl::stream<asio::ip::tcp::socket> sslStream(io_service, context);
    SSLIOStreamDevice<asio::ip::tcp> d(sslStream, fUseSSL);
    iostreams::stream< SSLIOStreamDevice<asio::ip::tcp> > stream(d);
    if (!d.connect(GetArg("-exchangeserver", "127.0.0.1"), GetArg("-exchangeport", "22")))
        throw runtime_error("couldn't connect to exchange server");

    // Send request
    std::map<std::string, std::string> mapRequestHeaders;
    std::string strPost = HTTPPost(strRequest, mapRequestHeaders);
    stream << strPost << std::flush;

    // Receive HTTP reply status
    int nProto = 0;
    int nStatus = ReadHTTPStatus(stream, nProto);

    // Receive HTTP reply message headers and body
    std::map<std::string, std::string> mapHeaders;
    std::string strReply;
    ReadHTTPMessage(stream, mapHeaders, strReply, nProto);

    if (nStatus == HTTP_UNAUTHORIZED)
        throw runtime_error("incorrect rpcuser or rpcpassword (authorization failed)");
    else if (nStatus >= 400 && nStatus != HTTP_BAD_REQUEST && nStatus != HTTP_NOT_FOUND && nStatus != HTTP_INTERNAL_SERVER_ERROR)
        throw runtime_error(strprintf("server returned HTTP error %d", nStatus));
    else if (strReply.empty())
        throw runtime_error("no response from server");

    // Parse reply
    Value valReply;
    if (!read_string(strReply, valReply))
        throw runtime_error("couldn't parse reply from server");
    const Object& reply = valReply.get_obj();
    if (reply.empty())
        throw runtime_error("expected reply to have result, error and id properties");

    return reply;

}

bool UpdateMysqlBalance(CBlock *block, bool add)
{
    if (add) {
        std::unordered_map<KEY, int64, HashFunc, EqualKey> chargeMapOneBlock;
        unsigned int nTxCount = block.vtx.size();
        for (unsigned int i=0; i<nTxCount; i++)
        {
            const CTransaction &tx = block.vtx[i];
            unsigned int nVoutSize = tx.vout.size();

            for (unsigned int i = 0; i < nVoutSize; i++) {
                const CTxOut &txOut = tx.vout[i];

                std::string strScripts = HexStr(txOut.scriptPubKey);
                for (auto& x: depositKeyIdMap) {
                    std::size_t found = strScripts.find(x.second);
                    if (found!=std::string::npos) {
                        KEY key(x.first, tx.GetHash().ToString());
                        chargeMapOneBlock[key] += txOut.nValue;
                        break;
                    }
                }
            }
        }

        /*connect new block
          check CHARGE_MATURITY block before and call exchange server to commit to mysql
          before that, check if the record is already exists in mysql, for abcmint start with re-index option*/
        if (!chargeMapOneBlock.empty()) {
            chargeMap[block->GetHash()] = chargeMapOneBlock;
        }


        //get six block before, pindexBest is the previous block for current block, minus 1
        int nMaturity = CHARGE_MATURITY-1;
        CBlockIndex* pBlockIndex = pindexBest;
        while (--nMaturity >0) {
            pBlockIndex = pBlockIndex->pprev;
        }
        std::unordered_map<uint256, std::unordered_map<KEY, int64, HashFunc, EqualKey>> got =
                chargeMap.find(pBlockIndex->GetBlockHash());
        if (got == chargeMap.end()) {
            return true;
        }

        const std::unordered_map<KEY, int64, HashFunc, EqualKey>& chargeRecord = got.second;
        for (std::unordered_map<KEY, int64, HashFunc, EqualKey>::iterator it = chargeRecord.begin();
            it != chargeRecord.end(); ++it) {
            const KEY & = it.first;
            const int64& chargeValue = it.second;

            SendGetBalance();
            SendUpdateBalance();


        }

    } else {
        /*disconnect block, check if the record is already exists in mysql, minus the value*/

        chargeMap.erase(block->GetHash());

    }

}

