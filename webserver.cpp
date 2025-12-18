/**
 * @file
 * @brief WenServer类核心实现文件
 */

#include "webserver.h"

#include <asm-generic/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <system_error>

#include "http/http_conn.h"
#include "timer/lst_timer.h"

WebServer::WebServer() {
  // 初始化变量
  m_port = 0;
  m_epollfd = -1;
  m_listenfd = -1;
  // 预分配http_conn对象
  users = new http_conn[MAX_FD];
  users_timer = new client_data[MAX_FD];
  m_TIMESLOT = TIMESLOT;
}

WebServer::~WebServer() {
  close(m_epollfd);
  close(m_listenfd);
  close(m_pipefd[1]);
  close(m_pipefd[0]);
  delete[] users;
  delete[] users_timer;
}

void WebServer::init(int port) { m_port = port; }

/**
 * @brief 初始化网络监听端口以及 Epoll 配置
 * @details 该函数执行服务器启动前的所有网络准备工作,具体步骤如下
 * 1. 创建 TCP/IPv4 socket
 * 2. 设定端口复用,允许服务器重启后立即使用同一端口
 * 3. 绑定服务器地址和端口
 * 4. 开启监听
 * 5. 创建epoll实例并注册监听套接字
 * 6. 创建全双工管道用于统一信号处理
 * 7, 设置信号处理回调及定时器
 */
void WebServer::eventListen() {
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
  address.sin_addr.s_addr = htonl(INADDR_ANY);  // 监听所有网卡
  address.sin_port = htons(m_port);

  ret = bind(m_listenfd, (struct sockaddr*)&address, sizeof(address));
  assert(ret >= 0);

  // 4. 开启监听(blacklog = 5)
  ret = listen(m_listenfd, 5);
  assert(ret >= 0);

  // 5. 创建epoll对象
  m_epollfd = epoll_create(5);
  assert(m_epollfd != -1);
  http_conn::m_epollfd = m_epollfd;
  Utils::u_epollfd = m_epollfd;

  // 6. 将监听socket(listenfd)加入Epoll
  utils.addfd(m_epollfd, m_listenfd, false, 1);
  // 7. 创建管道
  ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
  assert(ret != -1);
  Utils::u_pipefd = m_pipefd;
  // 8. 设置管道写端为非阻塞
  utils.setnonblocking(m_pipefd[1]);

  // 9. 设置读端为 LT 非阻塞,并加入epoll监听
  utils.addfd(m_epollfd, m_pipefd[0], false, 0);

  // 10. 设置信号处理函数
  utils.addsig(SIGPIPE, SIG_IGN);
  utils.addsig(SIGALRM, utils.sig_handler, false);
  utils.addsig(SIGTERM, utils.sig_handler, false);

  // 11. 启动第一次定时闹钟
  alarm(m_TIMESLOT);
}

/**
 * @brief 定时器初始化
 *
 * @param connfd 连接文件
 * @param client_address 客户端接口
 */
void WebServer::timer(int connfd, struct sockaddr_in client_address) {
  users[connfd].init(connfd, client_address);

  // 初始化定时器数据
  users_timer[connfd].address = client_address;
  users_timer[connfd].sockfd = connfd;

  // 创建定时器节点
  util_timer* timer = new util_timer;
  timer->user_data = &users_timer[connfd];
  timer->cb_func = cb_func;

  // 设置绝对超时时间
  time_t cur = time(nullptr);
  timer->expire = cur + 3 * m_TIMESLOT;
  users_timer[connfd].timer = timer;

  // 加入链表
  utils.m_timer_lst.add_timer(timer);
}

void WebServer::adjust_timer(util_timer* timer) {
  time_t cur = time(nullptr);
  timer->expire = cur + 3 * m_TIMESLOT;
  utils.m_timer_lst.adjust_timer(timer);
}

/**
 * @brief 处理非活动连接的定时器
 *
 * @param timer 指向当前超时的定时器节点
 * @param sockfd 该定时器关联的客户端 Socket 文件描述符
 */
void WebServer::deal_timer(util_timer* timer, int sockfd) {
  if (timer == NULL) {
    return;
  }
  timer->cb_func(&users_timer[sockfd]);
  if (timer) {
    utils.m_timer_lst.del_timer(timer);
  }
}

/**
 * @brief 处理新的客户端连接请求
 *
 * @return true 成功处理所有挂起的连接请求
 * @return false 过程中发生严重错误
 */
bool WebServer::deal_client_data() {
  struct sockaddr_in client_address;
  socklen_t client_addrlength = sizeof(client_address);

  while (true) {
    int connfd = accept(m_listenfd, (struct sockaddr*)&client_address,
                        &client_addrlength);
    if (connfd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      return false;
    }

    if (http_conn::m_user_count >= MAX_FD) {
      utils.show_error(connfd, "Internal server busy");
      close(connfd);
      continue;
    }
    timer(connfd, client_address);
  }
  return true;
}

/**
 * @brief 处理从信号管道接收到的信号
 *
 * @param[out] timeout 如果收到 SIGALRM,此值将被置为 true
 * @param[out] stop_server 如果收到 SIGTERM,此值将被置为 true
 * @return false 读取管道失败或管道为空
 */
bool WebServer::deal_signal(bool& timeout, bool& stop_server) {
  int ret = 0;
  // int sig;
  char signals[1024];

  ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
  if (ret == -1 || ret == 0) {
    return false;
  } else {
    for (int i = 0; i < ret; i++) {
      switch (signals[i]) {
        case SIGALRM: {
          timeout = true;
          break;
        }
        case SIGTERM: {
          stop_server = true;
          break;
        }
      }
    }
  }
  return true;
}
/*
 * 事件循环
 */
void WebServer::eventLoop() {
  bool timeout = false;
  bool stop_server = false;

  while (!stop_server) {
    int num = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);

    if (num < 0 && errno != EINTR) {
      break;
    }

    for (int i = 0; i < num; i++) {
      int sockfd = events[i].data.fd;

      // 1. 新连接到来
      if (sockfd == m_listenfd) {
        bool flag = deal_client_data();
        if (false == flag) {
          continue;
        }
      }
      // 2. 处理信号
      else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) {
        bool flag = deal_signal(timeout, stop_server);
        if (false == flag) {
          printf("deal signal failure\n");
        }
      } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
        util_timer* timer = users_timer[sockfd].timer;
        deal_timer(timer, sockfd);
      }
      // 4. 处理读事件
      else if (events[i].events & EPOLLIN) {
        util_timer* timer = users_timer[sockfd].timer;

        // 读一次数据
        if (users[sockfd].read_once()) {
          if (timer) {
            adjust_timer(timer);
          }
          users[sockfd].process();
        } else {
          deal_timer(timer, sockfd);
        }
      }
      // 处理写事件
      else if (events[i].events & EPOLLOUT) {
        util_timer* timer = users_timer[sockfd].timer;

        // 写一次数据
        if (users[sockfd].write()) {
          if (timer) {
            adjust_timer(timer);
          }
        } else {
          deal_timer(timer, sockfd);
        }
      }
    }
    if (timeout) {
      utils.timer_handler();
      timeout = false;
    }
  }
}

void WebServer::start() {
  eventListen();
  eventLoop();
}
