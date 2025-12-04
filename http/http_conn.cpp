#include "http_conn.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
// 定义 HTTP 响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the request file.\n";

// 初始化静态成员变量
int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

// --- Epoll 工具函数 ---

// 设置文件描述符为非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

/*
 * 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
 * ONESHOT保证操作系统最多触发一次事件，除非我们手动重置
 */
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    // EPOLLRDHUB:对方关闭连接
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 重置 EPOLLONESHOT
// 处理完一次请求以后，再次把socket设为可读，以便下次接收数据
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// ---http_conn 类实现 ---

// 关闭连接
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1)) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化（对外接口）
void http_conn::init(int sockfd, const sockaddr_in& addr)
{
    m_sockfd = sockfd;
    m_address = addr;

    // 端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到 epoll 监听，开启 ONESHOT
    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    init();
}

// 初始化连接（内部接口）
void http_conn::init()
{
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
    memset(m_write_buffer, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILE_REQUEST);
}

/*
 * 循环读取客户数据，直到无数据可读
 * 必须一次性读完
 */
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }

    int bytes_read = 0;
    while (true) {
        // 从 socket 读数据到 m_read_buf + m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);

        if (bytes_read == -1) {
            // EAGAIN 或 EWOULDBLOCK 说明缓冲区空了，读完了
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return false;
        } else if (bytes_read == 0) {
            return false; // 对方关闭连接
        }

        m_read_idx += bytes_read;
    }
    return true;
}

/*
 * 写 HTTP 响应
 * 这是一个分散写的操作，因为有两部分数据：
 * 1. 响应头（在 m_write_buffer中）
 * 2. 文件内容（nmap 映射的内存 m_file_address 中
 * writev 可以一次性把这两块不连续的内存发出去
 */
bool http_conn::write()
{
    int tmp = 0;

    // 如果没有数据要发（bytes_to_send == 0）
    // 这里的逻辑后续在 process_write 完善时会补充计算 bytes_to_send
    // 先写基础逻辑
    if (m_write_idx == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    // 核心循环
    while (1) {
        // writev 分散写
        tmp = writev(m_sockfd, m_iv, m_iv_count);

        if (tmp <= -1) {
            // 如果 TCP 写缓冲区满了，等待下一轮 EPOLLOUT 事件
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        // 这里需要处理“数据发送了一部分”的逻辑（滑动处理 iovec）
        // 为了简化 MIlestone 2,我们暂且假设一次性能发送完
        // 标准的mark server这里有复杂的 bytes_have_send 计算

        // 简单处理：假设发送成功
        unmap();
        if (m_linger) {
            // 如果是长连接，重置状态，继续监听读
            init();
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            return true;
        } else {
            // 短连接，发完就关
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            return false;
        }
    }
}

// --- 定义网站根目录 ---
// 指向resource
const char* doc_root = "./resource";

// --- 从状态机：解析一行 ---
// 从m_read_buf中找到\r\n,并将其转化为\0\0
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            // 如果\r是最后一个字符，说明这行还没有收全
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_idx + 1] == '\n') {
                // 如果下一个字符是\n,说明找到了一行的末尾
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if (temp == '\n') {
            //  处理上次只读到\r没有读到\n的情况
            m_read_buf[m_checked_idx - 1] = '\0';
            m_read_buf[m_checked_idx++] = '\0';
            return LINE_OK;
        }
        return LINE_BAD;
    }
    return LINE_OPEN; // 没找到换行符，继续接收
}
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    // 1. 获取请求方法
    // strpbrk检索字符串中第一个匹配 " \t" 的字符，用于分割 METHOD 和 URL
    m_url = strpbrk(text, " \t");
    if (!m_url) {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char* method = text;
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    // 2. 获取版本号
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';

    m_version += strspn(m_version, " \t"); // 跳过可能的连续空格

    // 仅支持http/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    // 3. 处理URL
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] == '/') {
        return BAD_REQUEST;
    }

    // 状态转移:请求行解析完毕，开始解析头部
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// --- http 头部解析 ---
// 格式如： Host: localhost \r\n Connection: keep-value
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    // 遇到空行，说明头部解析完毕
    if (text[0] == '\0') {
        // 如果是POST请求，还需要继续读取Content-Length长度的内容
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明是一个完整的 GET 请求
        return GET_REQUEST;
    }
    // 解析Connection头部
    else if (strncasecmp(text, "Connection:", 11)) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    }
    // 解析Content-Length头部
    else if (strncasecmp(text, "Content-Length", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    // 解析Host头部
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        //
    }
    return NO_REQUEST;
}

// 解析HTTP请求体
http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机
// 驱动整个解析过程
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    // 循环条件
    // 1. 正在解析内容（CHECK_STATE_CONTENT）且行状态OK
    // 2. 或者解析出一行完整的数据（parse_line() == LINE_OK）
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK)) {
        // 获取一行数据
        text = get_line();
        // 移动到下一行起始位置
        m_start_line = m_checked_idx;

        switch (m_check_state) {
        case CHECK_STATE_REQUESTLINE: {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST) {
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER: {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST) {
                return BAD_REQUEST;
            } else if (ret == GET_REQUEST) {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT: {
            ret = parse_content(text);
            if (ret == GET_REQUEST) {
                return do_request();
            }
            line_status = LINE_OPEN;
            break;
        }
        default: {
            return INTERNAL_ERROR;
        }
        }
    }
    return NO_REQUEST;
}

// 处理最终请求
// 分析目标文件是否存在，如果存在则mmap到内存
http_conn::HTTP_CODE http_conn::do_request()
{
    // 构造绝对路径
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 获取文件状态
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // 权限判断(S_IROTH:其他人可读)
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDEN_REQUEST;
    }

    //  判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);

    // 创建内存映射
    // 将磁盘文件直接映射到进程内存，避免内核到用户的拷贝
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    close(fd);
    return FILE_REQUEST; // 成功
}

// 响应模块
//
// 解除内存映射
void http_conn::unmap()
{
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char* format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);

    // vsnprintf 将格式化字符串写入缓冲区
    int len = vsnprintf(m_write_buffer + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_content(const char* content) { return add_response("%s", content); }

// 添加状态行
bool http_conn::add_status_line(int status, const char* title) { return add_response("%s %d %s\r\n", "HTTP/1.1", status, title); }

// 添加通用头部
bool http_conn::add_headers(int content_length)
{
    add_content_length(content_length);
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_type() { return add_response("Content-Type: %s\r\n", "text/html"); }

bool http_conn::add_content_length(int content_length) { return add_response("Content-Length: %d\r\n", content_length); }

bool http_conn::add_linger() { return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close"); }

bool http_conn::add_blank_line() { return add_response("%s", "\r\n"); }

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret) {
    case INTERNAL_ERROR: {
        add_status_line(500, error_500_title);
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
    case FORBIDEN_REQUEST: {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form)) {
            return false;
        }
        break;
    }
    case NO_RESOURCE: {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_403_form)) {
            return false;
        }
        break;
    }
    case FILE_REQUEST: {
        // 请求成功
        add_status_line(200, ok_200_title);
        if (m_content_length != 0) {
            // 如果有请求体长度
            add_content_length(m_content_length);
            add_linger();
            add_blank_line();
        } else {
            // 设置响应头
            add_headers(m_file_stat.st_size);
        }

        // 配置 writev 的 iovec
        // iov[0] 指向 m_write_buffer
        m_iv[0].iov_base = m_write_buffer;
        m_iv[0].iov_len = m_write_idx;

        // iov[1] 指向 m_file_address
        m_iv[1].iov_base = m_file_address;
        m_iv[1].iov_len = m_file_stat.st_size;

        m_iv_count = 2;
        return true;
    }
    default:
        return false;
    }

    // 对于非 FILE_REQUEST 的情况，只发送 HEADER 部分
    m_iv[0].iov_base = m_write_buffer;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}
