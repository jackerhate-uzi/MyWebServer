#include "http_conn.h"
#include <asm-generic/socket.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>

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

// 占位函数：解除内存映射，稍后实现
void http_conn::unmap()
{
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 占位函数：核心业务逻辑，稍后实现
void http_conn::unmap()
{
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 占位函数：核心业务逻辑，稍后实现
void http_conn::process()
{
    // 1. 解析 HTTP 请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    // 2. 生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }

    // 3. 注册写事件，等待内核发送
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}

// 下面是空的实现，防止编译报错，下一步我们会填满它们
http_conn::HTTP_CODE http_conn::process_read() { return NO_REQUEST; }
bool http_conn::process_write(HTTP_CODE ret) { return true; }
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) { return NO_REQUEST; }
http_conn::HTTP_CODE http_conn::parse_headers(char* text) { return NO_REQUEST; }
http_conn::HTTP_CODE http_conn::parse_content(char* text) { return NO_REQUEST; }
http_conn::HTTP_CODE http_conn::do_request() { return NO_REQUEST; }
http_conn::LINE_STATUS http_conn::parse_line() { return LINE_OK; }
bool http_conn::add_response(const char* format, ...) { return true; }
bool http_conn::add_content(const char* content) { return true; }
bool http_conn::add_status_line(int status, const char* title) { return true; }
bool http_conn::add_headers(int content_length) { return true; }
bool http_conn::add_content_type() { return true; }
bool http_conn::add_content_length(int content_length) { return true; }
bool http_conn::add_linger() { return true; }
bool http_conn::add_blank_line() { return true; }
