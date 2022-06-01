#ifndef __SQL_CONN_POOL_H__
#define __SQL_CONN_POOL_H__
#include <mysql/mysql.h>
#include <unistd.h>
#include <list>
#include <string>

#include "../lock/locker.h"
#include "../log/mylog.h"

using namespace std;

class sqlConnPool {
   public:
    static sqlConnPool* getInstance() {
        static sqlConnPool instance;
        return &instance;
    }

    sqlConnPool();
    ~sqlConnPool();

    void initSqlConnPool(string serverHost, string userName, string passwd, string dbName, int maxConnNum, int closeLog);

    MYSQL* getAsqlConn();
    bool releaseAsqlConn(MYSQL* conn);

    void destorySqlConnPool();

   private:
    // 最大连接数
    int maxConn;
    // 当前已使用连接数
    int currConn;
    // 空闲连接数
    int freeConn;

   private:
    // 连接池
    list<MYSQL*> sqlList;
    locker listlock;
    sem sqlsem;

   private:
    // 主机地址 "localhost"
    string sqlServerHost;
    // sql用户名 "root"
    string sqlUserName;
    // sql用户对应的密码 "123"
    string sqlPasswd;
    // 连接的数据库名
    string sqlDatabaseName;

    int m_closeLog;  // 是否关闭日志
};

// RAII (Resource Acquisition Is Initialization)
// RAII机制用于管理资源的申请和释放
// 避免只关注资源的申请和使用，而忘了释放，从而导致内存泄漏以及业务逻辑的错误
class sqlConnPoolRAII {
   public:
    sqlConnPoolRAII(MYSQL** conn, sqlConnPool* poolRAII);
    ~sqlConnPoolRAII();

   private:
    MYSQL* sqlConn;
    sqlConnPool* poolRAII;
};

#endif  // __SQL_CONN_POOL_H__