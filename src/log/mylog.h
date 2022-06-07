#ifndef __MYLOG_H__
#define __MYLOG_H__

#include <string>

#include "blockqueue.h"

using std::string;

class mylog {
   public:
    static mylog* get_instance() {
        // C++11保证内部静态变量的线程安全性
        static mylog instance;
        return &instance;
    }

    // 实现日志创建、写入方式的判断。
    bool init(const char* file_name, int close_log = 0, int max_queue_zie = 0, int log_buf_size = 8192, int log_max_line = 800000);

    // 公有异步写日志，调用函数async_write_log()
    static void* async_write_log_thread(void* arg) {
        mylog::get_instance()->async_write_log();
        return NULL;
    }

    // 完成写入日志文件中的具体内容,主要实现日志分级、分文件、格式化输出内容
    void write_log(int level, const char* format, ...);

    // 强制刷新缓冲区
    void flush(void);

   private:
    mylog();
    ~mylog();

    // 异步写日志
    void* async_write_log();

   private:
    // 路径名
    char dir_name[128];
    // log文件名
    char log_name[128];

    // 日志最大行数
    int m_log_max_line;
    // 日志缓冲区大小
    int m_log_buf_size;
    // 日志行数记录
    long long m_count_lines;

    // 记录当前时间
    int m_today;
    // 打开log文件的指针
    FILE* m_fp;

    // 要输出的内容
    char* m_buf;
    blockqueue<string>* m_log_queue;

    // 标记是否异步写日志
    bool m_is_async;

    locker m_mutex;
    // 是否关闭日志 1表示关闭，0不关闭，使用webbench测试性能时需要关闭日志功能
    int m_closeLog;
};

//这四个宏定义在其他文件中使用，主要用于不同类型的日志输出
//可变参数宏 ##__VA_ARGS__
#define LOG_DEBUG(format, ...)                                      \
    if (m_closeLog == 0) {                                         \
        mylog::get_instance()->write_log(0, format, ##__VA_ARGS__); \
        mylog::get_instance()->flush();                             \
    }
#define LOG_INFO(format, ...)                                       \
    if (m_closeLog == 0) {                                         \
        mylog::get_instance()->write_log(1, format, ##__VA_ARGS__); \
        mylog::get_instance()->flush();                             \
    }
#define LOG_WARN(format, ...)                                       \
    if (m_closeLog == 0) {                                         \
        mylog::get_instance()->write_log(2, format, ##__VA_ARGS__); \
        mylog::get_instance()->flush();                             \
    }
#define LOG_ERROR(format, ...)                                      \
    if (m_closeLog == 0) {                                         \
        mylog::get_instance()->write_log(3, format, ##__VA_ARGS__); \
        mylog::get_instance()->flush();                             \
    }

#endif  // __MYLOG_H__