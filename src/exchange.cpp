#include "exchange.h"
#include "init.h"
#include "util.h"
#include "main.h"
#include "abcmintrpc.h"

#include "jansson.h"

#include <string>
#include <map>
#include <vector>
#include <algorithm>


#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>


static const unsigned int CHARGE_MATURITY       = 6;
static const unsigned int HISTORY_TABLE_COUNT   = 100;
static const char*        UPDATE_COMMAND        = "balance.update";
static const char*        BALANCE_ASSET         = "ABC";
static const char*        BALANCE_BUSINESS      = "deposit";

static MYSQL mysql;
static std::map<unsigned int, std::string> depositKeyIdMap;

void KeyPoolFiller()
{
    printf("exchange, KeyPoolFiller started\n");
    RenameThread("KeyPoolFiller");
    SetThreadPriority(THREAD_PRIORITY_NORMAL);

    unsigned int nTargetSize = GetArg("-keypool", KEY_POOL_SIZE);
    try {
        while (true) {
            if (pwalletMain->setKeyPool.size() < nTargetSize/2) {
                if (pwalletMain->IsLocked()) {
                    //getnewaddress will return new address, but maybe timeout
                    //consider how to handle this error
                    printf("error: please unlock the wallet for key pool filling!\n");
                }
                pwalletMain->TopUpKeyPool(true);

            } else
                MilliSleep(10*1000);//sleep a loog while if the key pool still have a lot, 10 minutes
        }
    }
    catch (boost::thread_interrupted)
    {
        printf("exchange, FillKeyPool terminated\n");
        throw;
    }
}

void FillKeyPool(boost::thread_group& threadGroup)
{
    //use boost thread group, so that this thread can exit together with other thread when press ctrl+c
    threadGroup.create_thread(boost::bind(&KeyPoolFiller));
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

    std::string strHost =   GetArg("-host", "host");
    std::string strUser =   GetArg("-user", "user");
    std::string strPasswd = GetArg("-passwd", "passwd");
    std::string strDbname = GetArg("-dbname", "dbname");
    int64 dbport = GetArg("-dbport", 3306);
    if (mysql_real_connect(conn, strHost.c_str(), strUser.c_str(), strPasswd.c_str(), strDbname.c_str(), dbport, NULL, 0) == NULL) {
        mysql_close(conn);
        return NULL;
    }

    printf("exchange, connect to mysql, success\n");

    return conn;
}

bool LoadDepositAddress()
{
    char useStmt[32];
    sprintf(useStmt, "use abcmint");
    int ret = mysql_query(&mysql, useStmt);
    if(ret != 0) {
        printf("exchange, Query failed (%s)\n",mysql_error(&mysql));
        return false;
    }

    char query[64];
    sprintf(query, "select * from `coin_abc`");
    ret = mysql_query(&mysql, query);
    if(ret != 0) {
        printf("exchange, Query failed (%s)\n",mysql_error(&mysql));
        return false;
    }

    MYSQL_RES *res;
    if (!(res=mysql_store_result(&mysql)))
    {
        printf("exchange, Couldn't get result from %s\n", mysql_error(&mysql));
        return false;
    }

    size_t num_rows = mysql_num_rows(res);
    for (size_t i = 0; i < num_rows; ++i) {
        MYSQL_ROW row = mysql_fetch_row(res);
        unsigned int userId = strtoul(row[0], NULL, 10);

        //transaction to keyId
        CAbcmintAddress address;
        address.SetString(row[1]);
        CKeyID keyID;
        address.GetKeyID(keyID);
        CScript s;
        s<<keyID;
        //printf("exchange, depositKeyIdMap add userId:%d, row[1]:%s, address:%s, keyId:%s\n", userId, row[1], address.ToString().c_str(), HexStr(s).c_str());
        depositKeyIdMap[userId] = HexStr(s);
    }

    mysql_free_result(res);
    return true;
}

/*the business_id should be use to identify the charge history, because the block can be connect-disconnect-reconnect.
one case below, A is the root block:
step 1, B/C block come and both link to block A， A-B becomes the best chain, B block will be connect and send charge
record to exchange server.
 ___    ___
|_A_|->|_B_|                <-------- the best chain
  |     ___
  |--->|_C_|

step 2, D block comes and links to C block, A-C-D becomes the best chain, B would be disconnect, C and D would be
connect and send charge record to exchange server.
 ___    ___
|_A_|->|_B_|
  |     ___    ___
  |--->|_C_|->|_D_|         <-------- the best chain

step3, E and F block comes and connect to block B, A-B-E-F becomes the best chain. B will be reconnect, but if
business_id is not used, we can't identify the charge history, and the charge record in B won't be send to exchange
server.
 ___    ___    ___    ___
|_A_|->|_B_|->|_E_|->|_F_|  <-------- the best chain
  |     ___    ___
  |--->|_C_|->|_D_|


*/
bool GetBalanceHistory(const unsigned int userId, const std::string& txId, int64& chargeBusinessId)
{
    char useStmt[32];
    sprintf(useStmt, "use trade_history");
    int ret = mysql_query(&mysql, useStmt);
    if(ret != 0) {
        printf("exchange, GetBalanceHistory Query failed (%s)\n",mysql_error(&mysql));
        return false;
    }

    char query[256];
    sprintf(query, "select * from `balance_history_%u` A WHERE `A.user_id` = %u and `A.detail` like '%%%s%%'",
            userId % HISTORY_TABLE_COUNT, userId, txId.c_str());
    ret = mysql_query(&mysql, query);
    if(ret != 0) {
        printf("exchange, GetBalanceHistory Query failed (%s)\n",mysql_error(&mysql));
        return false;
    }

    MYSQL_RES *res;
    if (!(res=mysql_store_result(&mysql)))
    {
        printf("exchange, GetBalanceHistory Couldn't get result from %s\n", mysql_error(&mysql));
        return false;
    }

    std::vector<int64> vConnect;
    std::vector<int64> vDisconnect;
    size_t num_rows = mysql_num_rows(res);
    for (size_t i = 0; i < num_rows; ++i) {
        MYSQL_ROW row = mysql_fetch_row(res);

        json_t *detail = json_loads(row[7], 0, NULL);
        if (!detail) {
            printf("exchange, GetBalanceHistory invalid balance history record : %s\n", row[7]);
            return false;
        }

        //if find disconnect block, ignore the id
        json_t * disconnect_business_id = json_object_get("disconnect");
        if (json_is_integer(disconnect_business_id)) {
            int64 disconnectBusinessId = json_integer_value(disconnect_business_id);
            vDisconnect.push_back(disconnectBusinessId);
            json_decref(detail);
            continue;
        }

        //no disconnect detail, means connect block
        json_t * connect_business_id = json_object_get("id");
        if (json_is_integer(connect_business_id)) {
            int64 connectBusinessId = json_integer_value(connect_business_id);
            vConnect.push_back(connectBusinessId);
        }

        json_decref(detail);
    }

    unsigned int connectCount = vConnect.size();
    unsigned int disconnectCount = vDisconnect.size();
    for ( auto itr=vConnect.begin(); itr!=vConnect.end(); ) {
        if ( find(vDisconnect.begin(),vDisconnect.end(),*itr) != vDisconnect.end() ) {
            itr = vConnect.erase(itr);
        }
        else
        {
            ++itr;
        }
    }

    if (vConnect.size() != 0 || vConnect.size() != 1) {
        printf("exchange, mysql GetBalanceHistory， balance_history_%u in error state, connect times should be equal
        or one more time than disconnect, connectCount:%u, disconnectCount:%u, userid:%u\n",
        userId % HISTORY_TABLE_COUNT, connectCount, disconnectCount, userId);
        return false;
    }
    chargeBusinessId = connectCount.size() == 0 ? 0:vConnect[1];
    return true;
}

bool SendUpdateBalance(const unsigned int& userId, const std::string& txId, const int64& value,
                            bool add, const int64& chargeBusinessId)
{
    //{"id":5,"method":"account.update",params": [1, "BTC", "deposit", 100, "1.2345"]}
    json_t * request = json_object();
    json_object_set_new(request, "id", json_integer(userId));//json_integer user long long int
    json_object_set_new(request, "method", json_string(UPDATE_COMMAND));

    json_t * params = json_array();
    json_array_append_new(params, json_integer(userId));               // user_id
    json_array_append_new(params, json_string(BALANCE_ASSET));         // asset
    json_array_append_new(params, json_string(BALANCE_BUSINESS));      // business

    int64 business_id = GetTimeMicros();
    json_array_append_new(params, json_integer(business_id));          // business_id, use for repeat checking

    //convert to char*
    int64 quotient = value/COIN;
    int64 remainder = value % COIN;

    char value_str[64];
    sprintf(value_str, "%s%lld%s%08lld", (add ? "+" : "-"), quotient, ".", remainder);
    json_array_append_new(params, json_string(value_str));                // change

    // detail
    json_t * detail = json_object();
    json_object_set_new(detail, "disconnect", json_integer(chargeBusinessId));
    json_object_set_new(detail, "txId", json_string(txId.c_str()));
    json_array_append_new(params, detail);
    json_object_set_new(request, "params", params);

    char* request_str = json_dumps(request, 0);
    std::string strRequest(request_str);

    bool result = CallExchangeServer(strRequest);
    free(request_str);
    json_decref(request);
    return result;

}
/*1, how to handle repeat message error when calling via, just continue?
  2, any other error number need to be handle?
  3, only one charge in a transaction return failed, return false for the whole block?
  4, backup plan if miss any block?
*/
bool UpdateMysqlBalance(CBlock *block, bool add)
{
    //load the address each time when update balance, it would be better the front end notify to add or load address.
    if (!LoadDepositAddress()){
        printf("exchange, connect to query table coin_abc! please check mysql abcmint database.\n");
        return false;
    }

    if (add) {
        value_type chargeMapOneBlock;
        unsigned int nTxCount = block->vtx.size();
        for (unsigned int i=0; i<nTxCount; i++)
        {
            const CTransaction &tx = block->vtx[i];
            unsigned int nVoutSize = tx.vout.size();

            for (unsigned int i = 0; i < nVoutSize; i++) {
                const CTxOut &txOut = tx.vout[i];

                std::string strScripts = HexStr(txOut.scriptPubKey);
                for (auto& x: depositKeyIdMap) {
                    std::size_t found = strScripts.find(x.second);
                    if (found!=std::string::npos) {
                        printf("exchange, find CTxOut is for key %s\n", x.second.c_str());
                        chargeMapOneBlock[std::make_pair(x.first, tx.GetHash().ToString())] += txOut.nValue;
                        break; //user and address, 1:1
                    }
                }
            }
        }

        /*connect new block
          check CHARGE_MATURITY block before and call exchange server to commit to mysql
          before that, check if the record is already exists in mysql, for abcmint start with re-index option*/
        if (!chargeMapOneBlock.empty()) {
            if (!pwalletMain->AddChargeRecordInOneBlock(block->GetHash(), chargeMapOneBlock)) {
                printf("exchange, connect new block %s failed, add charge record to berkeley db return false!\n",
                    block->GetHash().ToString().c_str());
                return false;
            }
            /*printf("exchange, add block %s to chargeMap\n", block->GetHash().ToString().c_str());*/
            chargeMap[block->GetHash()] = chargeMapOneBlock;
        }

        //get six block before, pindexBest is the previous block for current block, minus 1
        int nMaturity = CHARGE_MATURITY-1;
        CBlockIndex* pBlockIndex = pindexBest;
        while (--nMaturity > 0 && pBlockIndex) {
            pBlockIndex = pBlockIndex->pprev;
        }

        //if pBlockIndex is null, return true
        if (!pBlockIndex) return true;

        std::map<uint256, value_type>::iterator got = chargeMap.find(pBlockIndex->GetBlockHash());
        if (got == chargeMap.end()) {
            return true;
        }


        /*printf("exchange, find block %s height:%d in chargeMap to call via server\n",
                pBlockIndex->GetBlockHash().ToString().c_str(), pBlockIndex->nHeight);*/

        const value_type& chargeRecord = got->second;
        for (value_type::const_iterator it = chargeRecord.begin(); it != chargeRecord.end(); ++it) {
            const std::pair<unsigned int, std::string> & pair_value = it->first;
            const unsigned int& userId = pair_value.first;
            const std::string& txId = pair_value.second;
            const int64& chargeValue = it->second;

            int64 chargeBusinessId;
            if (!GetBalanceHistory(userId, txId, chargeBusinessId)) return false;
            if (0 == chargeBusinessId) {
                if (!SendUpdateBalance(userId, txId, chargeValue, add, chargeBusinessId)) {
                    /*printf("exchange, SendUpdateBalance failed, chargeBusinessId:%lld, userId:%u, txId:%s, chargeValue:%lld, add:%s\n",
                        chargeBusinessId, userId, txId.c_str(), chargeValue, add?"true":"false");*/
                    return false;
                }
            } else {
                /*printf("exchange, GetBalanceHistory chargeBusinessId %lld, userId:%d, txId:%s, chargeValue:%lld, add:%s\n",
                        chargeBusinessId, userId, txId.c_str(), chargeValue, add?"true":"false");*/
                continue;
            }
        }

    } else {
        /*disconnect block, check if the record is already exists in mysql, minus the value*/
        std::map<uint256, value_type>::iterator got = chargeMap.find(block->GetHash());
        if (got == chargeMap.end()) {
            return true;
        }

        const value_type& chargeRecord = got->second;
        for (value_type::const_iterator it = chargeRecord.begin(); it != chargeRecord.end(); ++it) {
            const std::pair<unsigned int, std::string>& pair_value  = it->first;
            const unsigned int& userId = pair_value.first;
            const std::string& txId = pair_value.second;
            const int64& chargeValue = it->second;

            int64 chargeBusinessId;
            if (!GetBalanceHistory(userId, txId, chargeBusinessId)) return false;
            if (0 != chargeBusinessId) {
                if (!SendUpdateBalance(userId, txId, chargeValue, add, chargeBusinessId)) {
                    /*printf("exchange, SendUpdateBalance failed, chargeBusinessId:%lld, userId:%u, txId:%s, chargeValue:%lld, add:%s\n",
                        chargeBusinessId, userId, txId.c_str(), chargeValue, add?"true":"false");*/
                    return false;
                }
            } else {
                /*printf("exchange, GetBalanceHistory chargeBusinessId: %lld, userId:%d, txId:%s, chargeValue:%lld, add:%s\n",
                        chargeBusinessId, userId, txId.c_str(), chargeValue, add?"true":"false");*/
                continue;
            }
        }

        if (!pwalletMain->DeleteChargeRecordInOneBlock(block->GetHash())) {
            printf("exchange, disconnect block %s failed, delete charge record in berkeley db return false!\n", block->GetHash().ToString().c_str());
            return false;
        }

        chargeMap.erase(block->GetHash());
    }

    return true;
}

