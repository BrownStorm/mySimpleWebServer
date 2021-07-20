#ifndef _REDIS_POOL_
#define _REDIS_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include <hiredis/hiredis.h>

#include "locker.h"
#include "log.h"

using namespace std;

class RedisPool
{
// public:
    
//     string m_url;           // 主机地址
//     string m_Port;          // 数据库端口号
//     // string m_User;          // 登录数据库用户名
//     // string m_Password;      // 使用数据库密码
//     // string m_DatabaseName;  // 使用数据库名

public:
    // 单例模式
    static RedisPool *GetInstance();
    void init(const char* url, const char* port, int maxConn);

    redisContext* GetConnection();                  // 获取redis连接
    bool ReleaseConnection(redisContext *conn);     // 释放连接
    int GetFreeConn();                              // 获取连接
    void DestroyPool();                             // 销毁所有连接

    int setString(string key, string value);
    string getString(string key);

    RedisPool();
    ~RedisPool();
private:
    string m_url;           // 主机地址
    string m_port;          // 数据库端口号
    int m_MaxConn;          // 最大连接数
    int m_CurConn;          // 当前已使用的连接数
    int m_FreeConn;         // 当前空闲的连接数
    locker lock;
    list<redisContext*> connList;  // 连接池
    sem reserve;
    
    // redisContext* _connect;
    // redisReply* _reply;

private:
    RedisPool(RedisPool* redis){};
};

class redisRAII {
public:
    redisRAII(redisContext **conn, RedisPool *connPool);
    ~redisRAII();
private:
    redisContext *connRAII;
    RedisPool *poolRAII;
};

#endif  //_REDIS_H_