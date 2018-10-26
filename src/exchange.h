// Copyright (c) 2018 The Abcmint developers

#ifndef ABCMINT_EXCHANGE_H
#define ABCMINT_EXCHANGE_H

#include <boost/thread.hpp>
#include <mysql/mysql.h>
#include <mysql/errmsg.h>
#include "main.h"

static const unsigned int KEY_POOL_SIZE         = 100;
static const unsigned int CHARGE_MATURITY       = 6;

void FillKeyPool(boost::thread_group& threadGroup);

struct KEY
{
    int          userId;
    std::string  txId;

    KEY(int userIdIn, std::string txIdIn) : userId(userIdIn), txId(txIdIn){}
};

struct HashFunc
{
    std::size_t operator()(const KEY &key) const
    {
        using std::size_t;
        using std::hash;

        return ((hash<int>()(key.userId)) >> 1)
            ^ (hash<std::string>()(key.txId) << 1);
    }
};

struct EqualKey
{
    bool operator () (const KEY &lhs, const KEY &rhs) const
    {
        return lhs.userId  == rhs.userId
            && lhs.txId  == rhs.txId;
    }
};

MYSQL *ConnectMysql();
bool LoadDepositAddress();
bool UpdateMysqlBalance(CBlock *block, bool add);




#endif

