#include "sqlconnpool.h"

sqlConnPool::sqlConnPool() {
    this->currConn = 0;
    this->freeConn = 0;
}

sqlConnPool::~sqlConnPool() {
    destorySqlConnPool();
}

void sqlConnPool::initSqlConnPool(string serverHost, string userName, string passwd, string dbName, int maxConnNum, int closeLog) {
    sqlServerHost = serverHost;
    sqlUserName = userName;
    sqlPasswd = passwd;
    sqlDatabaseName = dbName;
    maxConn = maxConnNum;
    m_closeLog = closeLog;
    // list_lock.lock();
    for (int i = 0; i < maxConn; i++) {
        printf("create the %dth mysql connection\n", i);
        MYSQL* conn = NULL;
        conn = mysql_init(NULL);
        if (conn == NULL) {
            printf("mysql_init failed!\n");
            LOG_ERROR("%s", "mysql_init failed!");
            exit(1);
        }
        conn = mysql_real_connect(conn, serverHost.c_str(), sqlUserName.c_str(), sqlPasswd.c_str(), sqlDatabaseName.c_str(), 0, NULL, 0);

        if (conn == NULL) {
            printf("Connection failed!\n");
            LOG_ERROR("%s", "Connection failed!");
            exit(1);
        }
        freeConn++;
        sqlList.push_back(conn);
    }

    sqlsem = sem(freeConn);
    maxConn = freeConn;
}

MYSQL* sqlConnPool::getAsqlConn() {
    MYSQL* conn = NULL;

    if (sqlList.empty()) {
        return NULL;
    }

    sqlsem.wait();
    listlock.lock();

    conn = sqlList.front();
    sqlList.pop_front();

    listlock.unlock();

    return conn;
}

bool sqlConnPool::releaseAsqlConn(MYSQL* conn) {
    if (conn == NULL) {
        return false;
    }
    listlock.lock();

    sqlList.push_back(conn);

    listlock.unlock();
    sqlsem.post();
    return true;
}

void sqlConnPool::destorySqlConnPool() {
    listlock.lock();

    for (auto it = sqlList.begin(); it != sqlList.end(); it++) {
        MYSQL* conn = *it;
        mysql_close(conn);
    }
    sqlList.clear();
    listlock.unlock();
}

sqlConnPoolRAII::sqlConnPoolRAII(MYSQL** conn, sqlConnPool* pool) {
    *conn = pool->getAsqlConn();

    sqlConn = *conn;
    poolRAII = pool;
}

sqlConnPoolRAII::~sqlConnPoolRAII() {
    poolRAII->releaseAsqlConn(sqlConn);
}
