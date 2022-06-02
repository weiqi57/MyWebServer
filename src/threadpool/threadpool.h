#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <cstdio>
#include <exception>
#include <list>

#include "../lock/locker.h"

template <typename T>
class threadpool {
   public:
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    // 往请求队列添加任务
    bool append(T *request);

   private:
    // 工作线程运行的函数，不断从工作队列中取出任务并执行
    // 注意该函数设置为静态的，因为C++的pthread_create第三个参数必须指向静态函数
    static void *worker(void *arg);
    void run();

   private:
    // 线程池中的线程数
    int m_thread_number;
    // 请求队列中允许的最大请求数
    int m_max_request;
    // 描述线程池的数组，其大小为m_thread_number
    pthread_t *m_threads;
    // 请求队列
    std::list<T *> m_workqueue;
    // 保护请求队列的互斥锁
    locker m_queuelocker;
    // 使用这个信号量标识是否有任务需要处理
    sem m_queuestat;
    // 标记是否结束进程
    bool m_stop;
};

template <typename T>
threadpool<T>::threadpool(int thread_number, int max_request)
    : m_thread_number(thread_number),
      m_max_request(max_request),
      m_threads(NULL),
      m_stop(false) {
    if ((thread_number <= 0) || (max_request < 0)) {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) {
        throw std::exception();
    }

    // 创建m_thread_number个线程，并将它们都设置为脱离线程
    for (int i = 0; i < m_thread_number; i++) {
        printf("create the %dth thread\n", i);
        // 注意第一个参数为指针，最后一次参数this指针，用于传递当前对象的所有成员
        // note here，在此处创建出的新线程直接调用worker()函数
        int ret = pthread_create(m_threads + i, NULL, worker, this);
        if (ret != 0) {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i])) {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
    m_stop = true;
}

// 主线程调用，往请求队列添加新的待处理任务
// 生产者
template <typename T>
bool threadpool<T>::append(T *request) {
    /*操作共享队列一定得加锁，因为它被所以线程共享*/
    m_queuelocker.lock();

    if (m_workqueue.size() >= m_max_request) {
        m_queuelocker.unlock();
        return false;
    }
    // 往请求队列添加新的任务
    // note here! 核心处理处
    m_workqueue.push_back(request);

    m_queuelocker.unlock();
    // 添加一个任务，信号量++
    m_queuestat.post();
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg) {
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

// 工作线程
// 消费者
template <typename T>
void threadpool<T>::run() {
    while (true) {
        // 处理一个任务，信号量--
        m_queuestat.wait();
        m_queuelocker.lock();

        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        // 工作线程从请求队列上取下一个任务，然后调用process函数处理
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if (!request) {
            continue;
        }

        // note here! 核心处理处
        // 模板的处理函数，应用时实现
        request->process();
    }
}

#endif