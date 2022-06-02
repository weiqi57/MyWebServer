#include "webserver.h"

int main(int argc, char* argv[]) {
    // 基础信息
    string serverIP = "0.0.0.0";
    int serverPORT = 9000;
    int threadNum = 8;  // 线程池线程数量

    // 日志信息
    int logMode = 0;   // 写日志方式
    int closeLog = 0;  // 是否关闭日志功能(1关闭，0不关闭，用于webbench)

    //数据库信息
    string mysqlUser = "root";
    string mysqlPasswd = "123";
    string mysqlDBname = "webserver_user";
    int sqlConnNum = 8;  // 数据库连接池数量

    webServer server;

    //初始化
    server.init(serverIP, serverPORT, threadNum, logMode, closeLog, mysqlUser, mysqlPasswd, mysqlDBname, sqlConnNum);

    //日志
    server.writeLog();

    //数据库
    server.sqlPool();

    //线程池
    server.threadPool();

    //监听
    server.eventListen();

    //运行
    server.eventLoop();

    return 0;
}
