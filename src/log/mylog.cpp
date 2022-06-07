#include "mylog.h"

#include <pthread.h>
#include <stdarg.h>  // va_list,va_start,va_end
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <iostream>
#include <string>

mylog::mylog() {
    m_count_lines = 0;
    m_is_async = false;
}

mylog::~mylog() {
    if (m_fp != NULL) {
        fclose(m_fp);
    }
}

// 实现日志创建、写入方式的判断
// 写入方式通过初始化时是否设置队列大小max_queue_size（表示在队列中可以放几条数据）来判断
// 若队列大小为0，则为同步，否则为异步
// 参数为日志文件名、队列大小、日志缓冲区大小、最大行数
bool mylog::init(const char* file_name, int close_log, int max_queue_size, int log_buf_size, int log_max_line) {
    // 异步处理
    if (max_queue_size > 0) {
        m_is_async = true;
        // 创建阻塞队列
        m_log_queue = new blockqueue<string>(max_queue_size);

        // 线程回调函数async_write_log_thread()
        pthread_t pid;
        pthread_create(&pid, NULL, async_write_log_thread, NULL);
    }
    m_closeLog = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);

    m_log_max_line = log_max_line;

    time_t t = time(NULL);
    struct tm* sys_time_tm = localtime(&t);

    string log_path = "/home/v7/linuxLearning/webServer/log/";
    char log_full_name[256] = {0};
    // int snprintf(char *str, size_t size, const char *format, ...)
    // 将可变参数(...)按照 format 格式化成字符串，并将字符串复制到 str 中
    // size为要写入的字符的最大数目 2022-05-19-ServerLog
    // %02d是将数字按宽度为2输出，若数据位数不到2位，则左边补空格
    snprintf(log_full_name, 255, "%d-%02d-%02d-%s", (sys_time_tm->tm_year) + 1900, (sys_time_tm->tm_mon) + 1, sys_time_tm->tm_mday, file_name);

    m_today = sys_time_tm->tm_mday;
    // "a"追加到一个文件。写操作向文件末尾追加数据。如果文件不存在，则创建文件
    string log_file = log_path + log_full_name;
    std::cout << "log_file is :" << log_file << std::endl;
    m_fp = fopen(log_file.c_str(), "a");
    if (m_fp == NULL) {
        return false;
    }
    return true;
}

void mylog::write_log(int level, const char* format, ...) {
    char s[16] = {0};

    switch (level) {
        case 0:
            strcpy(s, "[debug]:");
            break;
        case 1:
            strcpy(s, "[info]:");
            break;
        case 2:
            strcpy(s, "[warn]:");
            break;
        case 3:
            strcpy(s, "[erro]:");
            break;
        default:
            strcpy(s, "[info]:");
            break;
    }

    time_t t = time(NULL);
    struct tm* sys_time_tm = localtime(&t);

    m_mutex.lock();
    m_count_lines++;
    // 日志不是今天 或 写入的日志行数是最大行的倍数
    if (m_today != sys_time_tm->tm_mday || m_count_lines % m_log_max_line == 0) {
        fflush(m_fp);
        // 关闭旧的文件描述符，需要创建新的日志文件
        fclose(m_fp);

        char tail[16] = {0};
        snprintf(tail, 255, "%d_%02d_%02d_", (sys_time_tm->tm_year) + 1900, (sys_time_tm->tm_mon) + 1, sys_time_tm->tm_mday);

        char new_log[256] = {0};
        // 如果时间不是今天，则创建今天的日志，同时更新m_today和m_count
        if (m_today != sys_time_tm->tm_mday) {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = sys_time_tm->tm_mday;
            m_count_lines = 0;
        }
        // 超过了最大行，在之前的日志名基础上加后缀, m_count/m_split_lines
        else {
            snprintf(new_log, 255, "%s%s%s_%lld", dir_name, tail, log_name, m_count_lines / m_log_max_line);
        }
        m_fp = fopen(new_log, "a");
    }
    m_mutex.unlock();

    string log_str;
    va_list arg_list;
    va_start(arg_list, format);
    m_mutex.lock();

    // 写入具体的日志内容      内容格式：时间 + 内容
    // 时间格式化，snprintf成功返回写字符的总数，其中不包括结尾的null字符
    int n = snprintf(m_buf, 30, "%d/%02d/%02d %02d:%02d:%02d %s ", (sys_time_tm->tm_year) + 1900, (sys_time_tm->tm_mon) + 1, sys_time_tm->tm_mday, sys_time_tm->tm_hour, sys_time_tm->tm_min, sys_time_tm->tm_sec, s);
    // 内容格式化
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, arg_list);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';

    log_str = m_buf;
    m_mutex.unlock();
    // 异步写
    if (m_is_async && !m_log_queue->isFull()) {
        m_log_queue->enqueue(log_str);
    }
    // 同步写
    else {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(arg_list);
}

// fflush()会强迫将缓冲区内的数据写回参数stream 指定的文件中
// 在prinf()后加上fflush(stdout); 强制马上输出到控制台
// 避免可能下一个数据再上一个数据还没输出完毕后就将其覆盖
void mylog::flush(void) {
    m_mutex.lock();
    fflush(m_fp);
    m_mutex.unlock();
}

void* mylog::async_write_log() {
    string single_log;
    // 从阻塞队列中取出一条日志内容，写入到文件m_fp中
    while (m_log_queue->dequeue(single_log)) {
        m_mutex.lock();
        // fputs将single_log写入到m_fp
        fputs(single_log.c_str(), m_fp);
        m_mutex.unlock();
        LOG_DEBUG("async in while loop, finish a log, wait for next log");
    }
    return NULL;
}