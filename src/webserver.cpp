#include "webserver.h"

webServer::webServer() {
    // http_conn类对象
    users = new http_conn[MAX_FD];

    // 定时器
    usersTimerData = new clientData[MAX_FD];
}

webServer::~webServer() {
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[0]);
    close(m_pipefd[1]);
    delete[] users;
    delete[] usersTimerData;
    delete m_pool;
}

void webServer::init(string serverIP, int serverPORT, int threadNum, int logMode, int closeLog, string mysqlUser, string mysqlPasswd, string mysqlDBname, int sqlConnNum) {
    m_serverIP = serverIP;
    m_serverPORT = serverPORT;
    m_threadNum = threadNum;

    m_logMode = logMode;
    m_closeLog = closeLog;

    m_mysqlUser = mysqlUser;
    m_mysqlPasswd = mysqlPasswd;
    m_mysqlDBname = mysqlDBname;
    m_sqlConnNum = sqlConnNum;
}

void webServer::writeLog() {
    if (m_closeLog == 0) {
        if (m_logMode == 0) {
            mylog::get_instance()->init("ServerLog", m_closeLog, 0);
        } else {
            mylog::get_instance()->init("ServerLog", m_closeLog, 8);
        }
    }
}

void webServer::sqlPool() {
    m_sqlPool = sqlConnPool::getInstance();
    m_sqlPool->initSqlConnPool("localhost", m_mysqlUser.c_str(), m_mysqlPasswd.c_str(), m_mysqlDBname.c_str(), m_sqlConnNum, m_closeLog);

    users->init_mysql(m_sqlPool);
}

void webServer::threadPool() {
    m_pool = new threadpool<http_conn>(m_threadNum);
}

void webServer::eventListen() {
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    // struct linger{
    //     int l_onoff;
    //     int l_linger;
    // };
    // 当tmp={1,0}时，close系统调用会立即返回，tcp模块会丢弃被关闭socket对应tcp发送缓冲区中残留的数据
    // 设置tmp={1,1},若该socket为阻塞的，close将等待一段l_linger的时间，直到发送完所有残留数据，并得到对方确认
    struct linger tmp = {1, 1};
    setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    // address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_serverPORT);
    inet_pton(AF_INET, m_serverIP.c_str(), &address.sin_addr.s_addr);

    // 端口复用
    int opt = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int ret = 0;
    ret = bind(m_listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 128);
    assert(ret >= 0);

    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    m_util.init(TIMESLOT);

    // 主线程往epoll内核事件表中注册监听socket事件，当listen到新的客户连接时，listenfd变为就绪事件
    m_util.addfd(m_epollfd, m_listenfd, false, 0);
    http_conn::m_epollfd = m_epollfd;

    // 创建管道，管道写端写入信号值，管道读端通过I/O复用系统监测读事件
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    m_util.setNonblocking(m_pipefd[1]);
    m_util.addfd(m_epollfd, m_pipefd[0], false, 1);

    // 传递给主循环的信号值，只关注SIGALRM和SIGTERM
    m_util.addsig(SIGPIPE, SIG_IGN, true);
    m_util.addsig(SIGALRM, m_util.sig_alarm_handler, false);
    m_util.addsig(SIGTERM, m_util.sig_alarm_handler, false);

    alarm(TIMESLOT);

    util::u_pipefd = m_pipefd;
    util::u_epollfd = m_epollfd;
}

void webServer::eventLoop() {
    bool timeout = false;
    bool stop_server = false;
    while (!stop_server) {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR)) {
            LOG_ERROR("%s", "epoll_wait failure");
            break;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            // 处理新用户连接
            if (sockfd == m_listenfd) {
                bool flag = dealNewConn();
                if (false == flag) {
                    continue;
                }
            }
            // 处理错误和异常
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                timerClass* temp_timer = usersTimerData[sockfd].timer;
                deleteTimer(temp_timer, sockfd);
            }
            // 处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) {
                bool flag = dealSignal(timeout, stop_server);
                if (false == flag) {
                    LOG_ERROR("%s", "dealSignal failure");
                }
            }
            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN) {
                dealRead(sockfd);
            }
            // 当这一sockfd上有可写事件时，epoll_wait通知主线程。
            // 主线程往socket上写入服务器处理客户请求的结果
            else if (events[i].events & EPOLLOUT) {
                dealWrite(sockfd);
            }
        }

        // 处理定时器为非必须事件，收到信号并不是立马处理，完成读写事件后，根据设置的标记在此处完成定时处理任务
        // 包括删除定时器到期的任务 并重新定时以继续进行超时检测
        if (timeout) {
            m_util.timerHandler();
            LOG_INFO("%s", "timer tick,process a timer event");
            timeout = false;
        }
    }
}

void webServer::timer(int connfd, struct sockaddr_in client_address) {
    // 初始化客户连接，将connfd注册到内核事件表中
    users[connfd].init(connfd, client_address, m_closeLog, m_sqlPool);

    // 初始化 client_timer_data数据
    // 创建临时定时器，设置回调函数和超时时间，绑定用户数据，并将该定时器添加到定时器链表中
    usersTimerData[connfd].address = client_address;
    usersTimerData[connfd].sockfd = connfd;

    timerClass* temp_timer = new timerClass;
    temp_timer->user_data = &usersTimerData[connfd];
    temp_timer->timer_cb_fun = timer_cb_fun;
    time_t time_now = time(NULL);
    temp_timer->expire = time_now + 3 * TIMESLOT;
    usersTimerData[connfd].timer = temp_timer;

    m_util.m_timerList.addAtimer(temp_timer);
}

void webServer::adjustTimer(timerClass* timer) {
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    // LOG_INFO("due to the EPOLLIN, sockfd(%d)'s timer of client(%s) has been delayed", sockfd, inet_ntoa(users[sockfd].get_address()->sin_addr));
    // 该定时器的超时时间已经增加，需要调整其在升序链表中的位置
    m_util.m_timerList.adjustAtimer(timer);
}

void webServer::deleteTimer(timerClass* timer, int sockfd) {
    timer->timer_cb_fun(&usersTimerData[sockfd]);
    if (timer) {
        m_util.m_timerList.deleteAtimer(timer);
    }
}

bool webServer::dealNewConn() {
    struct sockaddr_in client_address;
    socklen_t client_addr_length = sizeof(client_address);
    // TODO ET/LT

    // LT
    int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addr_length);
    if (connfd < 0) {
        LOG_ERROR("%s,errno is %d", "accpet error", errno);
        return false;
    }
    if (http_conn::m_user_count >= MAX_FD) {
        m_util.showError(connfd, "Internal Server Busy\n");
        LOG_ERROR("%s", "Internal Server Busy");
        return false;
    }
    LOG_INFO("NEW USER %s", inet_ntoa(client_address.sin_addr));

    timer(connfd, client_address);

    return true;
}

bool webServer::dealSignal(bool& timeout, bool& stop_server) {
    char signals[1024];

    // 从管道读端读出信号值
    int ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret <= 0) {
        // ret==0 OR ret==-1
        return false;

    } else {
        for (int i = 0; i < ret; i++) {
            switch (signals[i]) {
                case SIGALRM: {
                    timeout = true;
                    break;
                }
                case SIGTERM: {
                    stop_server = true;
                    break;
                }
            }
        }
    }
    return true;
}

void webServer::dealRead(int sockfd) {
    // 创建定时器临时变量，将该连接对应的定时器取出来
    timerClass* temp_timer = usersTimerData[sockfd].timer;

    // proactor
    if (users[sockfd].read_once()) {
        LOG_INFO("dealRead with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
        // 将读取到的数据封装成一个请求对象并插入请求队列
        m_pool->append(users + sockfd);

        // 若有数据传输，则将定时器往后延迟3个TIMESLOT
        // 并对新的定时器在链表上的位置进行调整
        if (temp_timer) {
            adjustTimer(temp_timer);
        }
    } else {
        deleteTimer(temp_timer, sockfd);
    }

    // TODO reactor
}

void webServer::dealWrite(int sockfd) {
    timerClass* temp_timer = usersTimerData[sockfd].timer;

    // proactor
    if (users[sockfd].write_once()) {
        LOG_INFO("dealWrite to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

        // 若有数据传输，则将定时器往后延迟3个单位
        // 并对新的定时器在链表上的位置进行调整
        if (temp_timer) {
            adjustTimer(temp_timer);
        }
    } else {
        deleteTimer(temp_timer, sockfd);
    }

    // TODO reactor
}
