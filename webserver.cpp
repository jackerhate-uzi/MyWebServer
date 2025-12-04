#include <asm-generic/socket.h>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http/http_conn.h"
#include "webserver.h"

//--- 工具函数开始 ---

/*
 * 设置文件描述符为非阻塞模式
 * 在Epoll下，必须使用非阻塞IO
 * 否则一个阻塞的read会把整个服务器卡死
 */
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

/*
 * 将文件描述符添加到epollfd的监听队列中
 * epollfd：Epoll句柄
 * fd：要监听的文件描述符
 * enable_et：是否开启ET模式
 */
void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;

    // EPOLLIN:数据可读
    // EPOLLRDHUB:TCP连接被对方关闭
    event.events = EPOLLIN | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    // 设置非阻塞
    setnonblocking(fd);
}

// --- 工具函数结束 ---

WebServer::WebServer()
{
    // 初始化变量
    m_port = 0;
    m_epollfd = -1;
    m_listenfd = -1;
    // 预分配http_conn对象
    users = new http_conn[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    delete[] users;
}

void WebServer::init(int port) { m_port = port; }

/*
 * 网络编程四部曲
 * socket -> bind -> listen -> accept
 */
void WebServer::eventListen()
{
    // 1. 创建 socket (TCP/Ipv4)
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    // 2. 设置端口复用
    // 作用：即使服务器崩溃重启，处于TIME_WAIT状态的端口也能被立即再次使用
    int ret = 0;
    int opt = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. 绑定地址和端口
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有网卡
    address.sin_port = htons(m_port);

    ret = bind(m_listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= 0);

    // 4. 开启监听(blacklog = 5)
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    // 5. 创建epoll对象
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    // 6. 将监听socket(listenfd)加入Epoll
    addfd(m_epollfd, m_listenfd);
}

/*
 * 事件循环
 */
void WebServer::eventLoop()
{
    while (true) {
        // 等待事件发生.-1 表示永久阻塞,直到有事件
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);

        if (number < 0 && errno != EINTR) {
            break;
        }

        // 处理所有发生的新事件
        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            // 情况1：有新的客户端连接进来
            if (sockfd == m_listenfd) {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);

                // 接受连接
                int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlength);
                if (connfd < 0) {
                    continue;
                }

                // 初始化连接对象
                users[connfd].init(connfd, client_address);
            }
            // 2. 处理异常事件
            else if (events[i].events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
                users[sockfd].close_conn();
            }
            // 3. 处理读事件
            else if (events[i].events & EPOLLIN){
                // 一次性读完所有数据
                if (users[sockfd].read_once()) {
                    // 核心：调用业务逻辑处理 === HTTP解析 -> 生成响应
                    users[sockfd].process();
                } else {
                users[sockfd].close_conn();
                }
            }
            // 4. 处理写事件
            else if (events[i].events & EPOLLOUT) {
               // 一次性写完所有数据
               if (!users[sockfd].write()) {
                    users[sockfd].close_conn();
               }
            }
        }
    }
}


void WebServer::start()
{
    eventListen();
    eventLoop();
}
