#include "http_conn.h"

const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站根目录，文件夹内存放请求的资源和跳转的html文件
const char* doc_root = "/home/v7/linuxLearning/webServer/resources";

unordered_map<string, string> umapUserPasswd;
locker sqlLock;

int setnoblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// EPOLLONESHOT作用：我们希望一个socket只能同时被一个线程操作
// epoll在某次循环中唤醒一个事件，并用某个工作进程去处理该fd，此后如果不注册EPOLLONESHOT
// 在该fd时间如果工作线程处理的不及时，主线程仍会唤醒这个事件，并另派线程池中另一个线程也来处理这个fd。
// 为了避免这种情况,EPOLLSHOT相当于说，某次循环中epoll_wait唤醒该事件fd后，就会从注册中删除该fd
// 也就是说以后不会epollfd的表格中将不会再有这个fd,也就不会出现多个线程同时处理一个fd的情况
// 当该线程处理完后，需要通过epoll_ctl重置epolloneshot事件
// TODO ET/LT
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;

    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnoblocking(fd);
}

void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close) {
    if (real_close && (m_sockfd != -1)) {
        epoll_ctl(m_epollfd, EPOLL_CTL_DEL, m_sockfd, 0);
        close(m_sockfd);
        m_sockfd = -1;
        // 关闭一个连接的同时将客户计数减一
        m_user_count--;
    }
}

// 初始化mysql
void http_conn::init_mysql(sqlConnPool* sqlPool) {
    MYSQL* aMysqlQuery = NULL;
    sqlConnPoolRAII mysql(&aMysqlQuery, sqlPool);

    int res = mysql_query(aMysqlQuery, "SELECT username,passwd FROM user");
    if (res) {
        LOG_ERROR("%s", "mysql_query SELECT Error");
    }
    MYSQL_RES* query_res = mysql_store_result(aMysqlQuery);

    while (MYSQL_ROW row = mysql_fetch_row(query_res)) {
        string tempName(row[0]);
        string tempPasswd(row[1]);
        umapUserPasswd.insert(make_pair(tempName, tempPasswd));
    }
}

void http_conn::init(int sockfd, const sockaddr_in& addr, int closeLog, sqlConnPool* sqlPool) {
    m_sockfd = sockfd;
    m_address = addr;
    m_closeLog = closeLog;
    h_sqlPool = sqlPool;

    addfd(m_epollfd, m_sockfd, true);
    m_user_count++;

    init();
}

void http_conn::init() {
    bytes_to_send = 0;
    bytes_have_send = 0;

    // 主状态机的起始状态
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机-用于解析出一行内容
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;

    // m_checked_idx为当前正在分析的字符temp在读缓冲区m_read_buf的位置
    // m_read_idx为读缓冲区m_read_buf中已经读入数据末尾的下一个位置
    // 缓冲区m_read_buf的第0~m_checked_idx已分析，下面的循环逐字符分析m_checked_idx~(m_read_idx-1)
    // printf("m_checked_idx=%d\tm_read_idx=%d\n", m_checked_idx, m_read_idx);
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];

        // 如果当前是\r字符，则有可能会读取到完整行
        if (temp == '\r') {
            // 如果'\r字符是目前buffer中最后一个读到的客户数据，
            // 则本次读取数据不完整，返回LINE_OPEN表示需要继续读取
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN;
            }
            // 如果下一个字符是'\n'，则读取到了完整的行，将\r\n改为\0\0
            else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            // 否则出错
            return LINE_BAD;
        }
        // 如果当前字符是\n，也有可能读取到完整行
        // 一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        else if (temp == '\n') {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    // 若所有内容都分析完毕也没有遇到'\r\n'，返回LINE_OPEN,表示还需要继续接收
    return LINE_OPEN;
}

// 主状态机-解析HTTP请求
http_conn::HTTP_CODE http_conn::process_read() {
    // 记录当前行的读取状态,在while循环中由从状态机返回不同状态
    LINE_STATUS line_status = LINE_OK;
    // 记录HTTP请求的处理结果
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    // 其中parse_line()是从状态机用于解析出一行内容
    // 在POST请求报文中，消息体的末尾没有任何字符，所以不能使用从状态机的状态
    // 这里转而使用主状态机的状态作为循环入口条件。解析完消息体后，报文的完整解析就完成了，
    // 但此时主状态机的状态还是CHECK_STATE_CONTENT,并在完成消息体解析后，将line_status变量更改为LINE_OPEN
    // 此时可以跳出循环，完成报文解析任务。
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK)) {
        text = get_line();
        LOG_INFO("HTTP request line is: %s", text);

        // m_start_line当前正在解析的行在m_read_buf中的起始位置
        // m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;

        // m_check_state 记录主状态机当前的状态
        switch (m_check_state) {
            // 分析请求行
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            // 分析头部字段 和 空行
            case CHECK_STATE_HEADER: {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                // 读到了完整请求,跳转到报文响应函数do_request()
                else if (ret == GET_REQUEST) {
                    return do_request();
                }
                break;
            }
            // 分析http请求的消息体
            case CHECK_STATE_CONTENT: {
                ret = parse_content(text);
                // 读到了完整POST请求后，跳转到报文响应函数do_request()
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                // 解析完消息体即完成报文解析，避免再次进入循环，更新line_status
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    // 若没有因为读到一个完整的请求而返回，则表示还需要继续读取客户数据才能做进一步分析
    // 在工作线程的设计中循环process_read()的上层process()函数
    return NO_REQUEST;
}

// 分析http请求的请求行，获得请求方法，目标url及http版本号
// 请求行示例 GET http://www.baidu.com/index.html HTTP/1.1
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // strpbrk()找到请求行中最先含有空格和\t任一字符的位置并返回
    // 示例中返回\thttp://www.baidu.com/index.html HTTP/1.1
    m_url = strpbrk(text, " \t");

    // 如果没有空格或\t，则报文格式有误
    if (!m_url) {
        return BAD_REQUEST;
    }

    // 将该位置改为\0，用于将前面数据（GET）取出
    *m_url++ = '\0';

    // 取出数据，并通过与GET和POST比较，以确定请求方式
    char* method = text;
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else if (strcasecmp(method, "POST") == 0) {
        m_method = POST;
        postFlag = 1;
    } else {
        return BAD_REQUEST;
    }

    // m_url继续跳过空格或\t字符
    // 示例中返回http://www.baidu.com/index.html HTTP/1.1
    // strspn函数返回 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_url += strspn(m_url, " \t");

    // 继续判断判断HTTP版本号，示例中返回\tHTTP/1.1
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    // 由于是通过地址直接修改字符串，此时m_url为http://www.baidu.com/index.html
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    // 对请求资源前7个字符进行判断
    // 有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    // 示例返回/index.html HTTP/1.1
    if (strncasecmp(m_url, "http://", 7) == 0) {
        // m_url为www.baidu.com/index.html
        m_url += 7;
        // /index.html
        m_url = strchr(m_url, '/');
    }

    //同样增加https情况
    if (strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    // 一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    // 当url为/时，显示欢迎界面
    if (strlen(m_url) == 1) {
        strcat(m_url, "homePage.html");
    }

    //请求行处理完毕,交由主状态机继续分析头部字段
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 分析http请求的头部字段和空行
// Connection: keep-alive
// Content-length: 40
// Host: www.baidu.com
// 空行
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    // 判断是空行还是请求头部字段
    if (text[0] == '\0') {
        // 判断是GET还是POST请求
        if (m_content_length != 0) {
            // POST请求，状态机转移到解析消息体状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们得到了完整的HTTP请求
        return GET_REQUEST;
    }
    // 处理头部字段的Connection字段
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        // 跳过空格和\t字符
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            // 设置长连接标志
            m_linger = true;
        }
    }
    // 处理头部字段的Content-length字段
    else if (strncasecmp(text, "Content-length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    // 处理头部字段的Host字段
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        // LOG_INFO("ops!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 分析http请求的消息体
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
    // 判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';

        // POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

// 当得到一个完整，正确的HTTP请求时，分析目标文件的属性，若目标文件存在，对所有用户可读且不是目录
// 则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request() {
    // 将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    // 使用p找到m_url中/的位置,p为'/'所在的位置,*(p+1)为'/'后的一个字符'0'/'1'/'2'...
    const char* p = strrchr(m_url, '/');

    // 实现登录和注册校验
    if (postFlag == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {
        // 根据标志判断是登录检测还是注册检测
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);

        free(m_url_real);

        // 将用户名和密码提取出来
        // m_string示例：user=123&password=123
        string loginName, loginPassword;
        int i = 0;
        for (i = 5; m_string[i] != '&'; i++) {
            loginName += m_string[i];
        }
        for (int j = i + 10; m_string[j] != '\0'; j++) {
            loginPassword += m_string[j];
        }

        char flagP = *(p + 1);
        switch (flagP) {
            case '3': {
                string sqlInsert = "INSERT INTO user(username,passwd) VALUES('";
                sqlInsert += loginName;
                sqlInsert += "','";
                sqlInsert += loginPassword;
                sqlInsert += "')";
                LOG_INFO("sqlInsert is:%s", sqlInsert.c_str());

                if (umapUserPasswd.count(loginName)) {
                    strcpy(m_url, "/registerError.html");
                } else {
                    MYSQL* aMysqlInsert = NULL;
                    sqlConnPoolRAII mysql(&aMysqlInsert, h_sqlPool);

                    //向数据库中插入数据时，需要通过锁来同步数据
                    sqlLock.lock();
                    int res = mysql_query(aMysqlInsert, sqlInsert.c_str());
                    sqlLock.unlock();
                    umapUserPasswd.insert(make_pair(loginName, loginPassword));

                    // mysql_query插入成功返回0
                    if (res == 0) {
                        LOG_INFO("insert success!");
                        strcpy(m_url, "/login.html");
                    } else {
                        strcpy(m_url, "/registerError.html");
                    }
                }
                break;
            }
            case '2': {
                // std::cout << "loginName is:---------" << loginName << "-------password is:------" << loginPassword << "--------" << std::endl;
                if (umapUserPasswd.count(loginName) && umapUserPasswd[loginName] == loginPassword) {
                    strcpy(m_url, "/welcome.html");
                } else {
                    strcpy(m_url, "/loginError.html");
                }
                break;
            }
        }
    }
    char flagP2 = *(p + 1);
    switch (flagP2) {
        // 如果请求资源为/0，表示跳转注册界面
        case '0': {
            char* m_url_real = (char*)malloc(sizeof(char) * 200);
            strcpy(m_url_real, "/register.html");
            strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

            free(m_url_real);
            break;
        }
        // 如果请求资源为/1，表示跳转登录界面
        case '1': {
            char* m_url_real = (char*)malloc(sizeof(char) * 200);
            strcpy(m_url_real, "/login.html");
            strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
            free(m_url_real);
            break;
        }
        // 请求图片页面
        case '5': {
            char* m_url_real = (char*)malloc(sizeof(char) * 200);
            strcpy(m_url_real, "/picture.html");
            strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
            free(m_url_real);
            break;
        }
        // 请求视频页面
        case '6': {
            char* m_url_real = (char*)malloc(sizeof(char) * 200);
            strcpy(m_url_real, "/video.html");
            strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
            free(m_url_real);
            break;
        }
        // 如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
        // /res/favicon.ico
        default: {
            strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
            LOG_INFO("m_url is %s", m_real_file);
            break;
        }
    }
    // 通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    // 失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }
    // 判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }
    // 判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // 以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    // 完成映射后就可以关闭文件描述符，避免浪费和占用
    close(fd);

    // 表示请求文件存在，且可以访问
    return FILE_REQUEST;
}

void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//  HTTP应答示例
//  HTTP/1.1 200 OK
//  Content-Length: 8024
//  Content-Type: text/html;charset=gbk
//  空行
//  响应正文
//  根据服务器处理HTTP请求的结果，决定返回给客户端的数据
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR: {
            //状态行/响应行
            add_status_line(500, error_500_title);
            //消息报头/响应头
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) {
                return false;
            }
            break;
        }
        case BAD_REQUEST: {
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form)) {
                return false;
            }
            break;
        }
        case NO_RESOURCE: {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form)) {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST: {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) {
                return false;
            }
            break;
        }
        case FILE_REQUEST: {
            //状态行
            add_status_line(200, ok_200_title);
            //如果请求的资源存在
            if (m_file_stat.st_size != 0) {
                add_headers(m_file_stat.st_size);
                //第一个iovec指针指向响应报文缓冲区m_write_buf，长度指向m_write_idx
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                //第二个iovec指针指向mmap返回的文件指针m_file_address，长度指向文件大小
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;

                //发送的全部数据为响应报文头部信息和文件大小
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                // LOG_INFO("FILE_REQUEST  bytes_to_send=%d\n", bytes_to_send);
                return true;
            }
            // 如果请求的资源大小为0，则返回空白html文件
            else {
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string)) {
                    return false;
                }
            }
            break;
        }
        default: {
            return false;
        }
    }
    // 除FILE_REQUEST状态外，其余状态只写回状态行
    // 申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    // debug here
    bytes_to_send = m_write_idx;
    return true;
}

// 往写缓冲区m_write_buf写入待发送的数据
// 使用C可变参数
bool http_conn::add_response(const char* format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }
    // 创建一个va_list类型的可变参数列表
    va_list arg_list;

    // 将变量arg_list初始化为传入参数
    va_start(arg_list, format);

    // 将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    // vsnprintf将可变参数格式化输出到一个字符数组
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);

    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;

    // 宏va_end来清理赋予va_list变量的内存。
    va_end(arg_list);
    LOG_INFO("response message to write is:%s", m_write_buf);

    return true;
}

// 写回HTTP应答的状态行 状态行示例：HTTP/1.1 200 OK
bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 写回HTTP应答的头部字段，具体的添加文本长度、连接状态和空行
void http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

// 添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}

// 添加文本类型，这里是html
bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger() {
    return add_response("Connnection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

// 添加空行
bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

// 添加文本content
bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}

// 循环读取客户数据，直到无数据可读或对方关闭连接
// 读取到m_read_buffer中，并更新m_read_idx
bool http_conn::read_once() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }

    int bytes_read = 0;

    // ET
    while (true) {
        // 从套接字接收数据，存储在m_read_buf缓冲区
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            // 非阻塞ET模式下，需要一次性将数据读完
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        } else if (bytes_read == 0) {
            return false;
        }
        // 修改m_read_idx的读取字节数
        m_read_idx += bytes_read;
    }
    // end ET

    return true;
}

// 写HTTP响应
bool http_conn::write_once() {
    int temp = 0;

    //若要发送的数据长度为0,表示响应报文为空
    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1) {
        // writev函数聚集写,用于在一次函数调用中写多个非连续缓冲区
        // 成功则返回已写的字节数，若出错则返回-1

        // 将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0) {
            if (errno == EAGAIN) {
                //重新注册写事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            //如果发送失败，但不是缓冲区问题，取消映射
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        if (bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;

            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        //继续发送第一个iovec头部信息的数据
        else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        //判断条件，数据已全部发送完
        if (bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            // 浏览器的请求为长连接
            if (m_linger) {
                //重新初始化HTTP对象
                init();
                return true;
            } else {
                // 关闭改socket连接
                return false;
            }
        }
    }
}

// 由线程池的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() {
    // 报文解析
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        // 注册并监听读事件，继续读数据
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    LOG_DEBUG("finish process_read()");
    // 报文响应
    // 服务器子线程调用process_write完成响应报文，随后注册epollout事件
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    // 注册并监听写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
    LOG_DEBUG("finish process_write()");
}
