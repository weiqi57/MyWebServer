#include "timer.h"
#include "../http/http_conn.h"

timerList::~timerList() {
    timerClass* curr = head;
    while (curr) {
        head = curr->next;
        delete curr;
        curr = head;
    }
}

void timerList::addAtimer(timerClass* timer) {
    if (!timer) {
        return;
    }

    if (!head) {
        head = tail = timer;
        return;
    }

    // 如果新的定时器超时时间小于当前头部结点
    // 直接将当前定时器结点作为头部结点
    if (timer->expire <= head->expire) {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    // 如果新的定时器超时时间大于当前尾部结点
    if (timer->expire >= tail->expire) {
        tail->next = timer;
        timer->prev = tail;
        tail = timer;
        return;
    }

    // 不是上述情况的话，就需要逐渐遍历链表，找到合适位置
    addAtimer(head, timer);
}

void timerList::adjustAtimer(timerClass* timer) {
    if (!timer) {
        return;
    }

    timerClass* next_timer = timer->next;
    if ((next_timer == nullptr) || (timer->expire < next_timer->expire)) {
        return;
    }

    // 被调整定时器是链表头结点，将定时器取出，重新插入
    if (timer == head) {
        head = head->next;
        head->prev = nullptr;
        timer->next = nullptr;
        addAtimer(head, timer);
    }
    // 被调整定时器在内部，将定时器取出，重新插入
    else {
        timer->next->prev = timer->prev;
        timer->prev->next = timer->next;
        timer->prev = nullptr;
        timer->next = nullptr;
        addAtimer(head, timer);
    }
}
// 在链表中删除一个定时器
void timerList::deleteAtimer(timerClass* timer) {
    if (!timer) {
        return;
    }
    if ((timer == head) && (timer == tail)) {
        delete timer;
        head = tail = nullptr;
        return;
    }

    if (timer == head) {
        head->next->prev = nullptr;
        head = head->next;
        delete timer;
        return;
    }
    if (timer == tail) {
        tail = tail->prev;
        tail->next = nullptr;
        delete timer;
        return;
    }

    // 被删除的定时器在链表内部，常规链表结点删除
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

// 定时任务处理函数
void timerList::tick() {
    if (!head) {
        return;
    }

    time_t time_now = time(NULL);
    timerClass* currTimer = head;
    while (currTimer) {
        // 链表容器为升序排列
        // 当前时间小于定时器的超时时间，后面的定时器也没有到期
        if (time_now < currTimer->expire) {
            break;
        }

        // 当前定时器到期，则调用回调函数，执行定时事件
        currTimer->timer_cb_fun(currTimer->user_data);

        head = currTimer->next;
        if (head) {
            head->prev = nullptr;
        }
        delete currTimer;
        currTimer = head;
    }
}

void timerList::addAtimer(timerClass* head, timerClass* timer) {
    timerClass* curr_timer = head;
    timerClass* prev_timer = nullptr;

    while (curr_timer) {
        if (curr_timer->expire > timer->expire) {
            timer->next = curr_timer;
            curr_timer->prev = timer;
            timer->prev = prev_timer;
            prev_timer->next = timer;
            break;
        }
        prev_timer = curr_timer;
        curr_timer = curr_timer->next;
    }

    if (!curr_timer) {
        prev_timer->next = timer;
        timer->prev = prev_timer;
        tail = timer;
    }
}

void util::init(int timeslot) {
    m_TIMESLOT = timeslot;
}

int util::setNonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void util::addfd(int epollfd, int fd, bool one_shot, int flagET) {
    epoll_event event;
    event.data.fd = fd;

    if (flagET == 1) {
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;

    } else {
        event.events = EPOLLIN | EPOLLRDHUP;
    }

    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setNonblocking(fd);
}

void util::sig_alarm_handler(int sig) {
    // 为保证函数的可重入性，保留原来的errno
    // 可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_erro = errno;
    int msg = sig;
    // 将信号值从管道写端写入，传输字符类型，而非整型
    send(u_pipefd[1], (char*)&msg, 1, 0);

    errno = save_erro;
}

void util::addsig(int sig, void(handler)(int), bool restart) {
    struct sigaction act;
    memset(&act, '\0', sizeof(act));

    act.sa_handler = handler;
    if (restart) {
        act.sa_flags |= SA_RESTART;  // SA_RESTART 重新调用被该信号终止的系统调用
    }
    //将所有信号添加到信号集中,调用信号处理函数时屏蔽所有信号
    sigfillset(&act.sa_mask);
    assert(sigaction(sig, &act, NULL) != -1);
}

void util::timerHandler() {
    m_timerList.tick();
    alarm(m_TIMESLOT);
}

void util::showError(int connfd, const char* info) {
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

// util静态成员，从webserver中赋值用于同步信息
int util::u_epollfd = 0;
int* util::u_pipefd = 0;

class util;
// 定时事件的回调函数，函数作用为删除非活动socket
void timer_cb_fun(clientData* clientTimerData) {
    // 删除非活动连接在socket上的注册事件
    epoll_ctl(util::u_epollfd, EPOLL_CTL_DEL, clientTimerData->sockfd, 0);
    assert(clientTimerData);

    // 关闭该文件描述符
    close(clientTimerData->sockfd);
    http_conn::m_user_count--;
}