#ifndef SQL_CONNECTION_POOL_H
#define SQL_CONNECTION_POOL_H

#include <list>
#include <string>
#include <mysql/mysql.h>
#include "../lock/locker.h"

/**
 * @class connection_poll
 * @brief MYSQL数据库连接池类
 * 使用单例模式管理数据库连接
 * 依赖 locker 类进行线程同步
 */
class connection_poll {
  // 单例模式，构造函数私有化
  private:
  connection_poll();
  ~connection_poll();

  int max_conn;       ///< 最大连接数
  int current_conn;   ///< 当前已使用的连接数
  int free_conn;      ///< 当前空闲的连接数
  locker lock;        ///< 互斥锁
  std::list<MYSQL *> conn_list; ///< 连接池列表
  sem reserve;        ///< 信号量，用于指示是否有连接可用
 public:
  /**
   * @brief 获取连接池的单例实例
   *
   * @return connection_poll* 单例指针
   */
  static connection_poll *GetInstance();
  /**
   * @brief 初始化连接池
   *
   * @param url 数据库地址
   * @param user 数据库用户名
   * @param password 数据库密码
   * @param database_name 数据库名
   * @param port 数据库端口
   * @param max_conn 最大连接数
   * @param close_log 是否关闭日志
   */
  void init(std::string url, std::string user, std::string password, std::string database_name, int port, int max_conn, int close_log);
  /**
   * @brief 从池中获取一个可用连接
   *
   * @return MYSQL* MYSQL连接指针
   */
  MYSQL *GetConnection();
  /**
   * @brief 释放连接回池中
   *
   * @param conn 要释放的连接
   * @return bool 释放是否成功
   */
  bool ReleaseConnection(MYSQL *conn);
  /**
   * @brief 获取当前空闲连接数
   *
   * @return int 空闲连接数
   */
  int GetFreeConn();
  /**
   * @brief 销毁连接池
   */
  void DestroyPool();

  std::string _url;           ///< 主机地址
  std::string _port;          ///< 数据库端口
  std::string _user;          ///< 数据库登陆用户名
  std::string _password;      ///< 数据库登陆密码
  std::string _database_name; ///< 使用的数据库名
  int _close_log;             ///< 日志开关
};


class connectionRAII {
public:
  /**
   * @brief 构造函数，获取连接
   *
   * @param con [out] 指向Mysql连接指针的指针，用于传出获取到的连接
   * @param conn_pool 连接池实例
   */
  connectionRAII(MYSQL **con, connection_poll *conn_pool);
  /**
   * @brief 析构函数：释放连接
   */
  ~connectionRAII();

private:
  MYSQL *connRAII;            ///< 持有Mysql连接
  connection_poll *poolRAII;  ///< 所属的连接池
};


#endif  // !SQL_CONNECTION_POOL_H
