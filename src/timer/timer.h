#ifndef __TIMER_H__
#define __TIMER_H__

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
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../log/mylog.h"

class timerClass;
// 连接资源,包括客户端socket地址、文件描述符和定时器
struct clientData {
    sockaddr_in address;

    int sockfd;
    timerClass* timer;
};

// 定时器类
class timerClass {
    // 将连接资源、定时事件和超时时间封装为定时器类
   public:
    timerClass() : prev(nullptr), next(nullptr) {}

   public:
    // 超时时间
    time_t expire;
    // 回调函数
    void (*timer_cb_fun)(clientData*);
    // 连接资源
    clientData* user_data;
    // 前向定时器
    timerClass* prev;
    // 后继定时器
    timerClass* next;
};

// 定时器容器
// 使用带头尾结点的双向链表实现
class timerList {
   public:
    timerList() : head(nullptr), tail(nullptr) {}
    ~timerList();

    // 往链表中添加一个定时器
    void addAtimer(timerClass* timer);
    // 调整定时器,任务发生变化时，调整定时器在链表中的位置
    void adjustAtimer(timerClass* timer);
    // 在链表中删除一个定时器
    void deleteAtimer(timerClass* timer);

    // 定时任务处理函数
    void tick();

   private:
    // 私有成员被共有成员调用
    // 向有序链表中遍历插入新的定时器
    void addAtimer(timerClass* head, timerClass* timer);

   private:
    timerClass* head;
    timerClass* tail;
};

class util {
   public:
    util(){};
    ~util(){};

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setNonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot,int flagET);

    //信号处理函数
    static void sig_alarm_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart=true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timerHandler();

    void showError(int connfd, const char* info);

   public:
    static int* u_pipefd;
    timerList m_timerList;
    static int u_epollfd;
    int m_TIMESLOT;
};

void timer_cb_fun(clientData* clientTimerData);

#endif  // __TIMER_H__