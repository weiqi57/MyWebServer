#ifndef __BLOCKQUEUE_H__
#define __BLOCKQUEUE_H__

#include <stdio.h>
#include <cstdlib>

#include "../lock/locker.h"
template <typename T>
class blockqueue {
   public:
    blockqueue(int max_size = 1000) {
        if (max_size <= 0) {
            exit(-1);
        }

        m_max_size = max_size;
        m_array = new T[m_max_size];
        m_size = 0;
        m_front = -1;
        m_end = -1;
    }
    ~blockqueue() {
        m_mutex.lock();
        if (m_array != NULL) {
            delete[] m_array;
        }
        m_mutex.unlock();
    }

    bool isFull();
    bool isEmpty();
    bool getFront(T& item);
    bool getEnd(T& item);
    bool enqueue(T& item);
    bool dequeue(T& item);
    int getSize();
    int getMaxSize();

   private:
    locker m_mutex;
    cond m_cond;

    T* m_array;

    int m_size;
    int m_max_size;
    int m_front;
    int m_end;
};

template <typename T>
bool blockqueue<T>::isFull() {
    m_mutex.lock();
    if (m_size >= m_max_size) {
        m_mutex.unlock();
        return true;
    }
    m_mutex.unlock();
    return false;
}

template <typename T>
bool blockqueue<T>::isEmpty() {
    m_mutex.lock();
    if (m_size == 0) {
        m_mutex.unlock();
        return true;
    }
    m_mutex.unlock();
    return false;
}

template <typename T>
bool blockqueue<T>::getFront(T& item) {
    if (isEmpty()) {
        return false;
    }

    m_mutex.lock();
    item = m_array[m_front];
    m_mutex.unlock();

    return true;
}

template <typename T>
bool blockqueue<T>::getEnd(T& item) {
    if (isEmpty()) {
        return false;
    }

    m_mutex.lock();
    item = m_array[m_end];
    m_mutex.unlock();
    return true;
}

// 入队
template <typename T>
bool blockqueue<T>::enqueue(T& item) {
    // if (isFull()) {
    //     return false;
    // }

    m_mutex.lock();
    if (m_size >= m_max_size) {
        m_cond.broadcast();
        m_mutex.unlock();
        return false;
    }

    m_end = (m_end + 1) % m_max_size;
    m_array[m_end] = item;
    m_size++;

    m_cond.broadcast();
    m_mutex.unlock();
    return true;
}

// 出队列
template <typename T>
bool blockqueue<T>::dequeue(T& item) {
    m_mutex.lock();

    while (m_size <= 0) {
        // 当重新抢到互斥锁，pthread_cond_wait返回为0
        if (!m_cond.wait(m_mutex.get())) {
            m_mutex.unlock();
            return false;
        }
    }
    m_front = (m_front + 1) % m_max_size;
    item = m_array[m_front];

    m_size--;
    m_mutex.unlock();

    return true;
}

// 获得队列当前大小
template <typename T>
int blockqueue<T>::getSize() {
    int size = 0;
    m_mutex.lock();
    size = m_size;
    m_mutex.unlock();
    return size;
}

// 获得队列最大大小
template <typename T>
int blockqueue<T>::getMaxSize() {
    int maxSize = 0;

    m_mutex.lock();
    maxSize = m_max_size;
    m_mutex.unlock();

    return maxSize;
}

#endif  // __BLOCKQUEUE_H__