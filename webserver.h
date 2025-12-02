#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "lock/locker.h"

//最大文件描述符数量
const int MAX_FD = 65536;
//最大事件数
const int MAX_EVENT_NUMBER = 10000;

class WebServer {
public:
    WebServer();
    ~WebServer();

    //初始化服务器配置（端口，数据库等）
    void init(int port);

    //启动服务器
    void start();
private:
    //初始化网络通信（socket, bind, listen)
    void eventListen();
    //启动事件循环
    void eventLoop();

    //基础属信
    int m_port;

    //Epoll相关
    int m_epollfd;
    int m_listenfd;
    epoll_event events[MAX_EVENT_NUMBER];
};
#endif
