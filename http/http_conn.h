#ifndef HTTPCONN_H
#define HTTPCONN_H

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>  // va_list,va_start,va_end
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>  // mmap
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>  // struct iovec/writv
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>
#include <unordered_map>

#include "../mysqlpool/sqlconnpool.h"

using std::string;

class http_conn {
   public:
    // 文件名的最大长度
    static const int FILENAME_LEN = 200;
    // 读缓冲区的大小
    static const int READ_BUFFER_SIZE = 2048;
    // 写缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;
    /* HTTP请求方法，支持GET，POST */
    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATCH };
    // 主状态机解析客户请求时，所处的状态
    enum CHECK_STATE {
        // 当前正在分析请求行
        CHECK_STATE_REQUESTLINE = 0,
        // 当前正在分析头部字段
        CHECK_STATE_HEADER,
        // 当前正在分析内容
        CHECK_STATE_CONTENT
    };

    // 服务器处理HTTP请求的可能结果
    enum HTTP_CODE {
        // 请求不完整，需要继续读取客户数据
        NO_REQUEST,
        // 获得了一个完整的HTTP请求,调用do_request完成请求资源映射
        GET_REQUEST,
        // HTTP请求有语法错误或请求资源为目录,跳转process_write完成响应报文
        BAD_REQUEST,
        // 无资源,跳转process_write完成响应报文
        NO_RESOURCE,
        // 客户对资源无访问权限,跳转process_write完成响应报文
        FORBIDDEN_REQUEST,
        // 请求资源可以正常访问,跳转process_write完成响应报文
        FILE_REQUEST,
        // 服务器内部错误
        INTERNAL_ERROR,
        // 客户端已关闭连接
        CLOSED_CONNECTION
    };
    // 从状态机的三种可能状态，即行的读取状态
    enum LINE_STATUS {
        // 读取到一个完整的行
        LINE_OK = 0,
        // 读取到的行出错
        LINE_BAD,
        // 行数据尚不完整
        LINE_OPEN
    };

   public:
    http_conn() {}
    ~http_conn() {}

   public:
    // 初始化新接收的连接，其内部调用私有方法init()进一步初始化
    void init(int sockfd, const sockaddr_in& addr,int closeLog,sqlConnPool* sqlPool);
    // 关闭HTTP连接
    void close_conn(bool real_close = true);
    // 处理客户请求
    void process();
    // 循环读取客户数据，直到无数据可读或对方关闭连接
    bool read();
    // 响应报文写入函数
    bool write();

    sockaddr_in* get_address() { return &m_address; }

    void init_mysql(sqlConnPool* sqlPool);

   private:
    // 重载初始化
    void init();
    // 主状态机-从m_read_buf读取内容，并解析HTTP请求
    HTTP_CODE process_read();
    // 向m_write_buf写入数据，填充HTTP应答
    // 根据服务器处理HTTP请求的结果，决定返回给客户端的数据
    bool process_write(HTTP_CODE ret);

    // 这组函数被process_read调用以分析HTTP请求
    // 主状态机-分析http请求的请求行
    HTTP_CODE parse_request_line(char* text);
    // 主状态机-分析http请求的头部字段
    HTTP_CODE parse_headers(char* text);
    // 主状态机-分析http请求的消息体(解析POST请求时用到)
    HTTP_CODE parse_content(char* text);
    // 当得到一个完整，正确的HTTP请求时，生成响应报文
    HTTP_CODE do_request();
    // HTTP_CODE do_request(sqlConnPool* sqlPool);

    // m_start_line是已经解析的字符
    // get_line用于将指针向后偏移，指向未处理的字符
    char* get_line() { return m_read_buf + m_start_line; }

    // 从状态机用于解析出一行内容
    LINE_STATUS parse_line();

    void unmap();

    //根据响应报文格式，生成对应8个部分以填充HTTP应答，以下函数均由do_request调用
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    // 写回HTTP应答的状态行
    bool add_status_line(int status, const char* title);
    // 写回HTTP应答的头部字段
    void add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_content_type();  // update
    bool add_linger();
    bool add_blank_line();

   public:
    // 所有socket上的事件都被注册到同一个epoll内核事件表，该文件描述符为静态的
    static int m_epollfd;
    // 统计用户数量
    static int m_user_count;
    int postFlag;

   private:
    // 该http连接的socket
    int m_sockfd;
    // 客户的socket的地址
    sockaddr_in m_address;

    // 读缓冲区,存储读取的请求报文数据
    char m_read_buf[READ_BUFFER_SIZE];
    // 表示读缓冲区m_read_buf中已经读入的数据的最后一个字节的下一个位置
    int m_read_idx;
    // 当前正在分析的字符在读缓冲区的位置
    int m_checked_idx;
    // 当前正在解析的行在m_read_buf中的起始位置
    int m_start_line;

    // 写缓冲区,存储发出的响应报文数据
    char m_write_buf[WRITE_BUFFER_SIZE];
    // 写缓冲区中待发送的字节数
    int m_write_idx;

    // 主状态机当前所处的状态
    CHECK_STATE m_check_state;
    // 请求的方法，取值为GET/POST
    METHOD m_method;

    // 以下为解析请求报文中对应的6个变量
    // 客户请求的目标文件的完整路径,其内容等于doc_root + m_url
    char m_real_file[FILENAME_LEN];
    // 客户请求的目标文件的文件名
    char* m_url;
    // http协议版本号，支持HTTP/1.1
    char* m_version;
    // 主机名
    char* m_host;
    // HTTP请求的消息体的长度
    int m_content_length;
    // HTTP请求是否要求保持连接
    bool m_linger;

    // 客户请求的目标文件被mmap到内存的起始位置
    char* m_file_address;
    // 目标文件的状态，通过它我们可以判断文件是否存在，是否为目录，是否可读
    struct stat m_file_stat;
    // struct iovec {
    //     void  *iov_base;    /* Starting address */
    //     size_t iov_len;     /* Number of bytes to transfer */
    // };
    // 用于快速读取数据的一个字节块，指出首地址和字节块的长度
    // 其中iovec 结构体的字段 iov_base 指向一个缓冲区，
    // 这个缓冲区存放的是网络接收的数据（readv），或者网络将要发送的数据（writev）
    struct iovec m_iv[2];
    int m_iv_count;  //被写内存块的数量
    // update

    char* m_string;       // 存储请求的消息体内容
    int bytes_to_send;    //剩余发送字节数
    int bytes_have_send;  //已发送字节数

    
    int m_closeLog;     // 是否关闭日志

    sqlConnPool* h_sqlPool;
};

// sqlConnPool* http_conn::sqlPool = sqlConnPool::getInstance();

#endif  // __HTTP_CONN_H__