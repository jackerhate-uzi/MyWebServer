#include "webserver.h"
int main(){
    //创建服务器实例
    WebServer server;

    //初始化端口9006
    server.init(9006);

    //启动
    server.start();

    return 0;
}
