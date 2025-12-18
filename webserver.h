#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>

#include "http/http_conn.h"
#include "lock/locker.h"
#include "timer/lst_timer.h"
// 最大文件描述符数量
const int MAX_FD = 65536;
// 最大事件数
const int MAX_EVENT_NUMBER = 10000;
const int TIMESLOT = 5;

/**
 * @class WebServer
 * @brief WebServer类用于封装所有操作
 *
 */
class WebServer {
 public:
  WebServer();
  ~WebServer();

  // 初始化服务器配置（端口，数据库等）
  void init(int port);

  // 启动服务器
  void start();

  // 用于存储所有连接的客户端信息
  http_conn* users;

 private:
  // 初始化网络通信（socket, bind, listen)
  void eventListen();
  // 启动事件循环
  void eventLoop();
  // 初始化新连接的定时器
  void timer(int connfd, struct sockaddr_in client_address);
  // 如果有数据传输,延长定时器
  void adjust_timer(util_timer* timer);
  // 删除定时器并关闭连接
  void deal_timer(util_timer* timer, int sockfd);
  // 处理客户端新连接
  bool deal_client_data();
  // 处理信号
  bool deal_signal(bool& timeout, bool& stop_server);

 public:
  // 基础属性
  int m_port;

  // Epoll相关
  int m_epollfd;
  int m_listenfd;
  epoll_event events[MAX_EVENT_NUMBER];

  // 定时器资源
  client_data* users_timer;
  Utils utils;
  int m_pipefd[2];
  // int m_listenfd;
  int m_TIMESLOT;
};
#endif
