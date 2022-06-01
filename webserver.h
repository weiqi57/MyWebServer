#ifndef __WEBSERVER_H__
#define __WEBSERVER_H__
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "./http/http_conn.h"
#include "./lock/locker.h"
#include "./log/mylog.h"
#include "./mysqlpool/sqlconnpool.h"
#include "./threadpool/threadpool.h"
#include "./timer/timer.h"

#define MAX_FD 65536            // 最大文件描述符
#define MAX_EVENT_NUMBER 10000  // 最大处理的事件数
#define TIMESLOT 5              // 最小超时时间,5 seconds

class webServer {
   public:
    webServer();
    ~webServer();

    void init(string serverIP, int serverPORT, int threadNum, int logMode, int closeLog, string mysqlUser, string mysqlPasswd, string mysqlDBname, int sqlConnNum);
    void writeLog();
    void sqlPool();
    void threadPool();
    void eventListen();
    void eventLoop();

    void timer(int connfd, struct sockaddr_in client_address);
    void adjustTimer(timerClass* timer);
    void deleteTimer(timerClass* timer, int sockfd);

    bool dealNewConn();
    bool dealSignal(bool& timeout, bool& stop_server);
    void dealRead(int sockfd);
    void dealWrite(int sockfd);

   private:
    http_conn* users;
    threadpool<http_conn>* m_pool;

    string m_serverIP;
    int m_serverPORT;
    int m_threadNum;

    int m_listenfd;
    int m_epollfd = 0;
    epoll_event events[MAX_EVENT_NUMBER];

    // 日志信息
    int m_logMode;
    int m_closeLog;

    // 定时器
    int m_pipefd[2];             // 用于定时器的信号处理函数和主循环之间进行通信的管道
    clientData* usersTimerData;  // 用于保存每个连接客户的资源的数组
    util m_util;

    // 数据库信息
    string m_mysqlUser;
    string m_mysqlPasswd;
    string m_mysqlDBname;
    int m_sqlConnNum;
    sqlConnPool* m_sqlPool;

    // m_flagET=1 ET
    // m_flagET=0 LT
    int m_flagET;
};

#endif  // __WEBSERVER_H__