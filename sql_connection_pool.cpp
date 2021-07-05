#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>

#include "sql_connection_pool.h"
#include "log.h"

using namespace std;

connection_pool::connection_pool() {
    m_CurConn = 0;
    m_FreeConn = 0;
}

connection_pool* connection_pool::GetInstance() {
    static connection_pool connPool;
    return &connPool;
}

// 构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int port, int MaxConn) {
    m_url = url;
    m_Port = port;
    m_User = User;
    m_Password = PassWord;
    m_DatabaseName = DBName;

    for (int i = 0; i < MaxConn; i++) {
        MYSQL *conn = NULL;
        conn = mysql_init(conn);

        if (conn == NULL) {
            Log::get_instance()->write_log(3, "MySQL Error: %s\n", mysql_error(conn));
            exit(1);
        }

        conn = mysql_real_connect(conn, "localhost", "root", "123456", "web", port, NULL, 0);
        if (!conn) {
            Log::get_instance()->write_log(3, ("error occurs: %s", mysql_error(conn)));
            exit(1);
        }

        connList.push_back(conn);
        ++m_FreeConn;

    }
    reserve = sem(m_FreeConn);

    m_MaxConn = m_FreeConn;
}

// 当有请求时，从数据库连接池中返回一个可用练级额，更新使用和空闲连接数
MYSQL* connection_pool::GetConnection() {
    MYSQL *conn = NULL;
    if (0 == connList.size()) return NULL;

    reserve.wait();

    lock.lock();

    conn = connList.front();
    connList.pop_front();

    --m_FreeConn;
    ++m_CurConn;

    lock.unlock();
    return conn;
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *conn) {
    if (NULL == conn) return false;
    lock.lock();

    connList.push_back(conn);

    ++m_FreeConn;
    --m_CurConn;

    lock.unlock();

    reserve.post();
    return true;
}

// 销毁数据库连接池
void connection_pool::DestroyPool() {
    lock.lock();
    if (connList.size() > 0) {
        list<MYSQL*>::iterator it;
        for (it = connList.begin(); it != connList.end(); ++it) {
            MYSQL* conn = *it;
            mysql_close(conn);
        }
        m_CurConn = 0;
        m_FreeConn = 0;
        connList.clear();
    }
    lock.unlock();
}

// 当前空闲的连接数
int connection_pool::GetFreeConn() {
    return this->m_FreeConn;
}

connection_pool::~connection_pool() {
    DestroyPool();
}


/*******************
*   connectionRAII
*******************/

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool) {
    *SQL = connPool->GetConnection();

    connRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII() {
    poolRAII->ReleaseConnection(connRAII);
}