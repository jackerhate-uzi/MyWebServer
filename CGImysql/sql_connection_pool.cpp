#include "sql_connection_pool.h"
#include <mysql/mysql.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <list>
connection_poll::connection_poll() {
  current_conn = 0;
  free_conn = 0;
}

connection_poll *connection_poll::GetInstance() {
  static connection_poll conn_poll;
  return &conn_poll;
}
connection_poll::~connection_poll() {
  DestroyPool();
}
void connection_poll::init(std::string url, std::string user,
                           std::string password, std::string database_name,
                           int port, int max_conn, int close_log) {
  _url = url;
  _user = user;
  _password = password;
  _database_name = database_name;
  _port = port;
  _close_log = close_log;

  for (int i = 0; i < max_conn; i++) {
    MYSQL *con = nullptr;
    con = mysql_init(con);

    if (con == nullptr) {
      std::cout << "Error: " << mysql_error(con);
      exit(1);
    }

    con = mysql_real_connect(con, url.c_str(), user.c_str(), password.c_str(), database_name.c_str(), port, nullptr, 0);

    if (con == nullptr) {
      std::cout << "Error: " << mysql_error(con);
      exit(1);
    }

    conn_list.push_back(con);
    ++free_conn;
  }
  reserve = sem(free_conn);
  max_conn = free_conn;
}
MYSQL *connection_poll::GetConnection() {
  MYSQL *con = nullptr;

  if (conn_list.size() == 0) {
    return nullptr;
  }

  reserve.wait();
  lock.lock();

  con = conn_list.front();
  conn_list.pop_front();

  --free_conn;
  ++current_conn;

  lock.unlock();
  return con;
}
bool connection_poll::ReleaseConnection(MYSQL *conn) {
  if (conn == nullptr) {
    return false;
  }

  lock.lock();

  conn_list.push_back(conn);
  ++free_conn;
  --current_conn;

  lock.unlock();

  reserve.post();
  return true;
}
int connection_poll::GetFreeConn() {
  return this->free_conn;
}
void connection_poll::DestroyPool() {
  lock.lock();
  if (conn_list.size() > 0) {
    std::list<MYSQL *>::iterator it;
    for (it = conn_list.begin(); it != conn_list.end(); ++it) {
      MYSQL *con = *it;
      mysql_close(con);
    }
    current_conn =0;
    free_conn = 0;
    conn_list.clear();
  }
  lock.unlock();
}

connectionRAII::connectionRAII(MYSQL **con, connection_poll *conn_pool)
{
  *con = conn_pool ->GetConnection();

  connRAII = *con;
  poolRAII = conn_pool;
}
connectionRAII::~connectionRAII()
{
  poolRAII->ReleaseConnection(connRAII);
}

