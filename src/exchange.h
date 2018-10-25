// Copyright (c) 2018 The Abcmint developers

#ifndef ABCMINT_EXCHANGE_H
#define ABCMINT_EXCHANGE_H

#include <boost/thread.hpp>
#include <mysql/mysql.h>
#include <mysql/errmsg.h>

static const unsigned int KEY_POOL_SIZE         = 100;

void FillKeyPool(boost::thread_group& threadGroup);

MYSQL *ConnectMysql();
bool LoadDepositAddress();




#endif

