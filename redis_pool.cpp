#include <stdio.h>
#include <string.h>
#include <iostream>

#include "redis_pool.h"

RedisPool::RedisPool() {
    m_CurConn = 0;
    m_FreeConn = 0;
}

RedisPool* RedisPool::GetInstance() {
    static RedisPool redisPool;
    return &redisPool;
}

// 构造初始化
void RedisPool::init(const char* url, const char* port, int maxConn) {
    m_url = url;
    m_port = port;

    for (int i = 0; i < maxConn; ++i) {
        redisContext* conn = NULL;
        conn = redisConnect(url, atoi(port));

        if (conn == NULL) {
            Log::get_instance()->write_log(3, "Redis Error: connect error\n");
            exit(1);
        }

        connList.push_back(conn);
        ++m_FreeConn;
    }
    reserve = sem(m_FreeConn);
    m_MaxConn = m_FreeConn;
}

// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
redisContext* RedisPool::GetConnection() {
    redisContext* conn = NULL;
    if (0 == connList.size()) return NULL;

    
    reserve.wait();

    lock.lock();

    conn = connList.front();
    connList.pop_back();

    --m_FreeConn;
    ++m_CurConn;

    lock.unlock();
    return conn;
}

// 释放当前使用的连接
bool RedisPool::ReleaseConnection(redisContext* conn) {
    if (NULL == conn) return false;
    lock.lock();

    connList.push_back(conn);
    ++m_FreeConn;
    --m_CurConn;

    lock.unlock();
    reserve.post();
    return true;
}

// 销毁Redis连接池
void RedisPool::DestroyPool() {
    lock.lock();
    if (connList.size() > 0) {
        list<redisContext*>::iterator it;
        for (it = connList.begin(); it != connList.end(); ++it) {
            redisContext* conn = *it;
            redisFree(conn);
        }
        m_CurConn = 0;
        m_FreeConn = 0;
        connList.clear();
    }
    lock.unlock();
}

// 当前空闲的连接数
int RedisPool::GetFreeConn() {
    return this->m_FreeConn;
}

RedisPool::~RedisPool() {
    DestroyPool();
}

int RedisPool::setString(string key, string value) {
    redisContext* redis =  RedisPool::GetInstance()->GetConnection();
    redisRAII raii(&redis, RedisPool::GetInstance());
    if(redis == NULL || redis->err)     // Error flags, 错误标识，0表示无错误
    {
        Log::get_instance()->write_log(3, "Redis init Error !!!\n");
        return -1;
    }
    redisReply *reply;
    reply = (redisReply *)redisCommand(redis, "SET %s %s",  key.c_str(), value.c_str());    //执行写入命令
    Log::get_instance()->write_log(1, "set string type = \n", reply->type); //获取响应的枚举类型
    int result = 0;
    if(reply == NULL) {
        result = -1;
        Log::get_instance()->write_log(3, "set string fail : reply->str = NULL\n");
        return -1;
    } else if(strcmp(reply->str, "OK") == 0) {  //根据不同的响应类型进行判断获取成功与否
        result = 1;
    } else {
        result = -1;
        Log::get_instance()->write_log(3, "set string fail: %s\n", reply->str);
    }
    freeReplyObject(reply);     //释放响应信息
    return result;
}

string RedisPool::getString(string key) {
    redisContext* redis =  RedisPool::GetInstance()->GetConnection();
    redisRAII raii(&redis, RedisPool::GetInstance());
    if(redis == NULL || redis->err)
	{
        Log::get_instance()->write_log(3, "Redis init Error!\n");
        return "error";
	}
	redisReply *reply;
    reply = (redisReply *)redisCommand(redis,"GET %s", key.c_str());
    Log::get_instance()->write_log(1, "get string type = %d\n", reply->type);
 
	if(reply == NULL)
	{
        Log::get_instance()->write_log(3, "ERROR getString: reply = NULL! maybe redis server is down\n");
        return "error";
	}
    else if(reply->len <= 0)
	{		
        freeReplyObject(reply);
        return "";
	}
	else
	{
        // stringstream ss;
        // ss << reply->str;
        // freeReplyObject(reply);
        // return ss.str();
        string s = reply->str;
        freeReplyObject(reply);
        return s;
    }
}

/*******************
*   redisRAII
*******************/

redisRAII::redisRAII(redisContext ** conn, RedisPool *redisPool) {
    *conn = redisPool->GetConnection();

    connRAII = *conn;
    poolRAII = redisPool;
}

redisRAII::~redisRAII() {
    poolRAII->ReleaseConnection(connRAII);
}




