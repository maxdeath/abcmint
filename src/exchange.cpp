#include "exchange.h"
#include "init.h"
#include "util.h"
#include "main.h"
#include "abcmintrpc.h"
#include "script.h"

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
static std::map<unsigned int, CKeyID> depositKeyIdMap;

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
        //CScript s;
        //s<<keyID;
        //printf("exchange, depositKeyIdMap add userId:%d, row[1]:%s, address:%s, keyId:%s\n", userId, row[1], address.ToString().c_str(), HexStr(s).c_str());
        //depositKeyIdMap[userId] = HexStr(s);
        depositKeyIdMap[userId] = keyID;
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
    sprintf(query, "select * from `balance_history_%u`  WHERE `user_id` = %u and `detail` like '%%%s%%'",
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
        //printf("exchange, GetBalanceHistory row[7] is null:%s\n", NULL == row[7]?"true":"false");
        json_t *detail = json_loads(row[7], 0, NULL);
        if (!detail) {
            printf("exchange, GetBalanceHistory invalid balance history record : %s\n", row[7]);
            return false;
        }

        //if find disconnect block, ignore the id
        json_t * disconnect_business_id = json_object_get(detail,"disconnect");
        if (json_is_integer(disconnect_business_id)) {
            int64 disconnectBusinessId = json_integer_value(disconnect_business_id);
            vDisconnect.push_back(disconnectBusinessId);
            json_decref(detail);
            continue;
        }

        //no disconnect detail, means connect block
        json_t * connect_business_id = json_object_get(detail, "id");
        if (json_is_integer(connect_business_id)) {
            int64 connectBusinessId = json_integer_value(connect_business_id);
            vConnect.push_back(connectBusinessId);
        }

        json_decref(detail);
    }

    unsigned int connectCount = vConnect.size();
    unsigned int disconnectCount = vDisconnect.size();
    for ( auto itr=vConnect.begin(); itr!=vConnect.end(); ) {
        bool find = false;
        for ( auto itr_dis=vDisconnect.begin(); itr_dis!=vDisconnect.end(); ) {
            if ( *itr == *itr_dis ) {
                itr = vConnect.erase(itr);
                itr_dis = vDisconnect.erase(itr_dis);
                find = true;
                break;
            }
            else
            {
                ++itr_dis;
            }
        }
        if (!find) ++itr;
    }

    if ((vConnect.size() != 0 && vConnect.size() != 1) || vDisconnect.size() != 0) {
        printf("exchange, mysql GetBalanceHistory， balance_history_%u in error state, connect times should be equal "
        "or one more time than disconnect, connectCount:%u, disconnectCount:%u, userid:%u\n",
        userId % HISTORY_TABLE_COUNT, connectCount, disconnectCount, userId);
        return false;
    }
    chargeBusinessId = vConnect.size() == 0 ? 0:vConnect[0];
    return true;
}

bool SendUpdateBalance(const unsigned int& userId, const std::string& txId, const int64& valueIn,
                            bool connect, const int64& chargeBusinessId)
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
    int64 value = (valueIn > 0 ? valueIn : -valueIn);
    int64 quotient = value/COIN;
    int64 remainder = value % COIN;

    char value_str[64];
    sprintf(value_str, "%s%lld%s%08lld", (((connect && valueIn > 0) ||(!connect && valueIn < 0)) ? "+" : "-"), quotient, ".", remainder);
    json_array_append_new(params, json_string(value_str));                // change

    //detail
    json_t * detail = json_object();
    if(!connect) json_object_set_new(detail, "disconnect", json_integer(chargeBusinessId));
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

bool UpdateMysqlBalanceConnect(const uint256& hash, value_type& chargeRecord)
{
    /*printf("exchange, find block %s height:%d in chargeMap to call via server\n",
            pBlockIndex->GetBlockHash().ToString().c_str(), pBlockIndex->nHeight);*/
    for (value_type::iterator it = chargeRecord.begin(); it != chargeRecord.end(); ++it) {
        const std::pair<unsigned int, std::string> & key = it->first;
        const unsigned int& userId = key.first;
        const std::string& txId = key.second;

        std::pair<int64, bool>& value = it->second;
        int64& chargeValue = value.first;
        bool& status = value.second;

        if (status) continue;

        int64 chargeBusinessId;
        if (!GetBalanceHistory(userId, txId, chargeBusinessId)) {
            printf("exchange UpdateMysqlBalanceConnect GetBalanceHistory return false, userId:%u, txId:%s, chargeBusinessId:%lld\n",
                userId, txId.c_str(),chargeBusinessId);
            continue;
        }

        if (0 == chargeBusinessId) {
            if (!SendUpdateBalance(userId, txId, chargeValue, true, chargeBusinessId)) {
                printf("exchange UpdateMysqlBalanceConnect exchange, SendUpdateBalance failed, chargeBusinessId:%lld, userId:%u, txId:%s, chargeValue:%lld, add:true\n",
                    chargeBusinessId, userId, txId.c_str(), chargeValue);
                status = false;
                continue;
            } else
                status = true;
        } else {
            /*printf("exchange, GetBalanceHistory chargeBusinessId %lld, userId:%d, txId:%s, chargeValue:%lld, add:%s\n",
                    chargeBusinessId, userId, txId.c_str(), chargeValue, add?"true":"false");*/
            continue;
        }
    }

    //over-write to update the status
    if (!pwalletMain->AddChargeRecordInOneBlock(hash, chargeRecord)) {
        printf("exchange, update status for block %s in berkeley db return false!\n", hash.ToString().c_str());
        return false;
    }

    return true;
}

bool UpdateMysqlBalanceDisConnect(const uint256& hash, value_type& chargeRecord)
{
    for (value_type::iterator it = chargeRecord.begin(); it != chargeRecord.end(); ++it) {
        const std::pair<unsigned int, std::string> & key = it->first;
        const unsigned int& userId = key.first;
        const std::string& txId = key.second;

        std::pair<int64, bool>& value = it->second;
        int64& chargeValue = value.first;

        int64 chargeBusinessId;
        if (!GetBalanceHistory(userId, txId, chargeBusinessId)) return false;
        if (0 != chargeBusinessId) {
            if (!SendUpdateBalance(userId, txId, chargeValue, false, chargeBusinessId)) {
                printf("exchange, UpdateMysqlBalanceDisConnect SendUpdateBalance failed, chargeBusinessId:%lld, userId:%u, txId:%s, chargeValue:%lld, add:true\n",
                    chargeBusinessId, userId, txId.c_str(), chargeValue);
                return false;
            }
        } else {
            /*printf("exchange, GetBalanceHistory chargeBusinessId: %lld, userId:%d, txId:%s, chargeValue:%lld, add:%s\n",
                    chargeBusinessId, userId, txId.c_str(), chargeValue, add?"true":"false");*/
            continue;
        }
    }

    if (!pwalletMain->DeleteChargeRecordInOneBlock(hash)) {
        printf("exchange, disconnect block %s failed, delete charge record in berkeley db return false!\n", hash.ToString().c_str());
        return false;
    }

    return true;
}

/*1, how to handle repeat message error when calling via, just continue?
  2, any other error number need to be handle?
  3, only one charge in a transaction return failed, return false for the whole block?
  4, backup plan if miss any block?
*/
bool UpdateMysqlBalance(CBlock *block, bool add)
{
    LOCK(cs_main);
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
            //if(tx.IsCoinBase()) continue;

            unsigned int nVoutSize = tx.vout.size();
            for (unsigned int i = 0; i < nVoutSize; i++) {
                const CTxOut &txOut = tx.vout[i];

                std::vector<std::vector<unsigned char> > vSolutions;
                txnouttype whichType;
                if (!Solver(txOut.scriptPubKey, whichType, vSolutions)){
                    printf("exchange AddressScanner Solver return false\n");
                    continue;
                }

                CKeyID keyID;
                if (TX_PUBKEYHASH == whichType) {
                    keyID = CKeyID(uint256(vSolutions[0]));
                } else {
                    continue;
                }


                for (auto& x: depositKeyIdMap) {
                    if (keyID == x.second) {
                        std::pair<unsigned int, std::string> key = std::make_pair(x.first, tx.GetHash().ToString());
                        std::pair<int64, bool>& value = chargeMapOneBlock[key];
                        value.first += txOut.nValue;
                        value.second = false;
                        break; //address is unique, user and address, 1:1
                    }
                }
            }
/*
            CCoinsViewCache view(*pcoinsTip, true);
            unsigned int nVinSize = tx.vin.size();
            for (unsigned int i = 0; i < nVinSize; i++) {
                const CTxIn &txIn = tx.vin[i];

                CCoins coins;
                if (!view.GetCoins(txIn.prevout.hash, coins) || !coins.IsAvailable(txIn.prevout.n))
                {
                    continue;
                }
                const CTxOut&  txOut = coins.vout[txIn.prevout.n];
                const CScript& prevPubKey = txOut.scriptPubKey;

                std::vector<std::vector<unsigned char> > vSolutions;
                txnouttype whichType;
                if (!Solver(prevPubKey, whichType, vSolutions)){
                    printf("exchange AddressScanner Solver return false\n");
                    continue;
                }

                CKeyID keyID;
                if (TX_PUBKEYHASH == whichType) {
                    keyID = CKeyID(uint256(vSolutions[0]));
                } else {
                    continue;
                }


                for (auto& x: depositKeyIdMap) {
                    if (keyID == x.second) {
                        std::pair<unsigned int, std::string> key = std::make_pair(x.first, tx.GetHash().ToString());
                        std::pair<int64, bool>& value = chargeMapOneBlock[key];
                        value.first -= txOut.nValue;
                        value.second = false;
                        break; //address is unique, user and address, 1:1
                    }
                }
            }
*/
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

        const uint256& hash = got->first;
        value_type& chargeRecord = got->second;
        return UpdateMysqlBalanceConnect(hash, chargeRecord);

    } else {
        /*disconnect block, check if the record is already exists in mysql, minus the value*/
        std::map<uint256, value_type>::iterator got = chargeMap.find(block->GetHash());
        if (got == chargeMap.end()) {
            return true;
        }

        const uint256& hash = got->first;
        value_type& chargeRecord = got->second;
        if (UpdateMysqlBalanceDisConnect(hash, chargeRecord)) {
            chargeMap.erase(hash);
            return true;
        } else
            return false;
    }
}


void charge()
{
    printf("exchange, charge thread started\n");
    RenameThread("charge");
    SetThreadPriority(THREAD_PRIORITY_NORMAL);

    try {
        while (true) {
            if (fImporting || fReindex) {
                MilliSleep(60*1000);//1 minutes
                continue;
            }

            LOCK(cs_main);

            for (std::map<uint256, value_type>::iterator it= chargeMap.begin(); it!= chargeMap.end();) {
                const uint256& hash = it->first;
                value_type& chargeRecord = it->second;
                std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
                assert(mi != mapBlockIndex.end());
                CBlockIndex* pBlockIndex = mi->second;

                if(NULL != pBlockIndex->pnext || pindexBest == pBlockIndex){
                    //in main chain, connect
                    if (pindexBest->nHeight - pBlockIndex->nHeight >5) {
                        UpdateMysqlBalanceConnect(hash, chargeRecord);
                    }
                    ++it;
                } else {
                    //not in main chain, disconnect
                    if (UpdateMysqlBalanceDisConnect(hash, chargeRecord)) {
                        chargeMap.erase(hash);
                    } else
                        ++it;
                }
            }

            if (chargeMap.size() == 0) {
                MilliSleep(30*60*1000);//30 minutes
            }
        }
    }
    catch (boost::thread_interrupted)
    {
        printf("exchange, charge thread terminated\n");
        throw;
    }
}


void UpdateBalance(boost::thread_group& threadGroup)
{
    //use boost thread group, so that this thread can exit together with other thread when press ctrl+c
    threadGroup.create_thread(boost::bind(&charge));
}


void AddressScanner()
{
    printf("exchange, AddressScanner started\n");
    RenameThread("AddressScanner");
    SetThreadPriority(THREAD_PRIORITY_NORMAL);

    CBlockIndex* pBlockIterator = pindexGenesisBlock;
    try {
        while (true) {
            if (fImporting || fReindex) {
                MilliSleep(60*1000);//1 minutes
                continue;
            }

            if(pBlockIterator ==NULL) {
                MilliSleep(60*1000);//1 minutes
                pBlockIterator = pindexGenesisBlock;
                continue;
            }

            if(pindexBest == pBlockIterator) {
                MilliSleep(10*60*1000);//10 minutes
                continue;
            }

            LOCK(cs_main);
            CBlock block;
            block.ReadFromDisk(pBlockIterator);

            unsigned int nTxCount = block.vtx.size();
            printf("exchange AddressScanner process block %u begin, nTxCount: %u\n", pBlockIterator->nHeight, nTxCount);
            for (unsigned int i=0; i<nTxCount; i++)
            {
                const CTransaction &tx = block.vtx[i];
                //if(tx.IsCoinBase()) continue;

                unsigned int nVoutSize = tx.vout.size();
                for (unsigned int i = 0; i < nVoutSize; i++) {
                    const CTxOut &txOut = tx.vout[i];
                    //-------------------------------------step 1, get user address-------------------------------------
                    std::vector<std::vector<unsigned char> > vSolutions;
                    txnouttype whichType;
                    if (!Solver(txOut.scriptPubKey, whichType, vSolutions)){
                        printf("exchange AddressScanner Solver return false\n");
                        continue;
                    }

                    CKeyID keyID;
                    if (TX_PUBKEYHASH == whichType) {
                        keyID = CKeyID(uint256(vSolutions[0]));
                    } else {
                        continue;
                    }

                    CAbcmintAddress address;
                    address.Set(keyID);

                    printf("exchange AddressScanner process block address %s\n", address.ToString().c_str());

                    //-------------------------------------step 2, check if the address exists-------------------------------------
                    char query[256];
                    sprintf(query, "select * from `abcmint`.`coin_abc` where `address` = '%s'", address.ToString().c_str());
                    int ret = mysql_query(&mysql, query);
                    if(ret != 0) {
                        printf("exchange, Query failed (%s)\n",mysql_error(&mysql));
                        continue;
                    }

                    MYSQL_RES *res;
                    if (!(res=mysql_store_result(&mysql)))
                    {
                        printf("exchange, Couldn't get result from %s\n", mysql_error(&mysql));
                        continue;
                    }

                    size_t num_rows = mysql_num_rows(res);
                    mysql_free_result(res);


                    printf("exchange AddressScanner process block address %s, num_rows: %u\n",
                    address.ToString().c_str(), num_rows);
                    if(num_rows > 0) continue;

                    //-------------------------------------step 3, create a user-------------------------------------
                    char insert_user[512];
                    sprintf(insert_user, "INSERT INTO `abcmint`.`abc_user`(`nickname`, `email`, `phone`, `passwd`, `country`, `balance`, `freeze`, `active`, `permit`, `register_time`)"
                    " VALUES ('true', 'true@qq.com', '123456789', 'false', NULL, NULL, NULL, NULL, NULL, '2018-11-01 17:38:06')");
                    ret = mysql_query(&mysql, insert_user);
                    if(ret != 0) {
                        printf("exchange, insert user failed (%s)\n",mysql_error(&mysql));
                        continue;
                    }

                    //-------------------------------------step 4, get the user just created-------------------------------------
                    char query_user[256];
                    sprintf(query_user, "select max(id) from `abcmint`.`abc_user`");
                    ret = mysql_query(&mysql, query_user);
                    if(ret != 0) {
                        printf("exchange, Query abc_user failed (%s)\n",mysql_error(&mysql));
                        continue;
                    }

                    if (!(res=mysql_store_result(&mysql)))
                    {
                        printf("exchange, Couldn't get result from abc_user %s\n", mysql_error(&mysql));
                        continue;
                    }

                    num_rows = mysql_num_rows(res);
                    if(num_rows < 1) continue;
                    MYSQL_ROW row = mysql_fetch_row(res);
                    printf("exchange AddressScanner max userid %s\n",row[0]);

                    //-------------------------------------step 5, insert the user address-------------------------------------
                    char insert_address[512];
                    sprintf(insert_address, "INSERT INTO `abcmint`.`coin_abc`(`id`, `address`) VALUES (%s, '%s')", row[0], address.ToString().c_str());
                    ret = mysql_query(&mysql, insert_address);
                    if(ret != 0) {
                        printf("exchange, insert address failed (%s)\n",mysql_error(&mysql));
                        continue;
                    }

                    mysql_free_result(res);
                }
            }
            pBlockIterator = pBlockIterator->pnext;
        }
    }
    catch (boost::thread_interrupted)
    {
        printf("exchange, AddressScanner terminated\n");
        throw;
    }
}


void ScanAddress(boost::thread_group& threadGroup)
{
    //use boost thread group, so that this thread can exit together with other thread when press ctrl+c
    threadGroup.create_thread(boost::bind(&AddressScanner));
}

