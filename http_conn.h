#ifndef HTTPCONNH
#define HTTPCONNH

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <map>

#include "locker.h"
#include "timer_wheel.h"
#include "timer_wheel.h"
#include "sql_connection_pool.h"

class tw_timer;

class http_conn
{
public:

    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    static const int FILENAME_LEN = 200;
    static int m_epollfd;
    static int m_user_count;
    static timer_wheel m_twheel;
    tw_timer* m_timer;
    int m_sockfd;
    MYSQL *mysql;
    int m_state;    // 读为0，写为1
    /*
        解析客户端请求时，主状态机所处的状态
        CHECK_STATE_REQUESTLINE:    正在分析请求行
        CHECK_STATE_HEADER:         正在分析请求头部
        CHECK_STATE_CONTENT:        正在分析请求正文
    */
    enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0, 
                      CHECK_STATE_HEADER,
                      CHECK_STATE_CONTENT};
    /*
        读取行时，从状态机所处状态
        LINE_OK:        读取了完整的一行
        LINE_BAD:       行出错
        LINE_OPEN:      行数据尚且不完整
    */
    enum LINE_STATE {LINE_OK = 0, LINE_BAD, LINE_OPEN};

    /*
        服务器处理HTTP请求可能的结果
        NO_REQUEST:         请求不完整
        GET_REQUEST:        获得了一个完整的客户请求
        BAD_REQUEST:        客户请求语法出错
        NO_RESOURCE:        没有所要求的资源
        FORBDDEN_REQUEST:   客户对资源没有访问权限
        INTERNAL_ERROR:     服务器内部出错
        CLOSED_CONNECTION:  客户端已经关闭
    */
    enum HTTP_CODE {NO_REQUEST, GET_REQUEST, 
                    BAD_REQUEST, NO_RESOURCE, 
                    FORBIDDEN_REQUEST, FLIE_REQUEST, 
                    INTERNAL_ERROR, CLOSED_CONNECTION};
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTION, CONNECT, PATCH};

public:
    http_conn(){}
    ~http_conn(){}

    void process();     // 工作线程调用的函数，处理用户请求。其中调用process_read();process_write();close_conn();

    bool read();        // 读取客户http请求。循环读取客户数据，直到无数据可读或者对方关闭连接
    bool write();       // 写http相应，使用循环方式，将内存数据块中的数据以writev的方式写入sockfd，最后释放内存块

    void init(int sockfd, const sockaddr_in& address);        // 初始化
    void init(int sockfd, const sockaddr_in &addr, char *, int , int, string user, string passwd, string sqlname);
    void close_conn(bool real_close=true);  // 关闭连接

    void timer_cb_func(http_conn* user_data);   // 定时器回调函数

    // bool read_once();

    sockaddr_in *get_address()
    {
        return &m_address;
    }

    void initmysql_result(connection_pool* connPool);
    int improv;

private:
    // 初始化所需用到的辅助函数
    void init();

    // 这一组函数用来分析http请求，process_read()被process()调用；其余被process_read()调用
    HTTP_CODE process_read();                   // 解析http请求（主状态机）
    LINE_STATE parse_line();                    // 分析行是否完整（从状态机）
    HTTP_CODE parse_request_line(char* text);   // 分析请求行
    HTTP_CODE parse_header(char* text);         // 分析请求头部
    HTTP_CODE pares_content(char* text);        // 分析请求正文
    HTTP_CODE do_request();                     // 处理请求，即读取目标文件，将文件内容映射到内存中
    char* get_line() { return m_read_buf + m_start_line; }

    // 这一组函数用来填充http应答，process_write()被process()调用；其余被process_write()调用
    bool process_write(HTTP_CODE ret);                      // 根据服务器处理HTTP请求的结果，决定返回给客户端的内容。
                                                            // 调用add_status_line();add_header();add_content()
                                                            // 将各种函数调用add_response()所得到的写缓冲数据，放入内存块（以便在write()函数中，调用writev写入sockfd）
    bool unmap();                                           // 对内存映射区执行munmap操作，销毁mmap出的内存块。write()中调用
    bool add_status_line(int status, const char* title);    // 写响应状态行，调用add_response();
    bool add_header(int content_length);                    // 写响应头，调用add_content_length();add_linger();add_bland_line()
    bool add_content(const char* content);                  // 调用add_reaponse();
    bool add_content_length(int content_length);            // 调用add_response();
    bool add_linger();                                      // 调用add_response();
    bool add_bland_line();                                  // 调用add_response();
    bool add_response(const char* format, ...);             // 往写缓冲中写入待发送的数据
    bool add_content_type();

    


private:
    
    sockaddr_in m_address;

    char m_read_buf[READ_BUFFER_SIZE];      // 读缓冲区
    char m_write_buf[WRITE_BUFFER_SIZE];    // 写缓冲区
    char m_read_file[FILENAME_LEN];         // 客户请求的目标文件的完整路径，为doc_root + m_url。doc_root为网站根目录

    int m_read_idx;         // 读缓冲区中，已经读入的客户端数据的最后一个字节的下一个位置；read()中用以检测读缓冲区是否已满
    int m_write_idx;        // 写缓冲区中，待发送的字节
    int m_checked_idx;      // 当前正在分析的字符在读缓冲区的位置，从状态机中表示当前正在分析的字符，主状态机中表示当前行的最后一个字节的下一个位置
    int m_start_line;       // 当前正在解析的行的起始位置

    METHOD m_method;        // 请求方法
    char* m_url;            // 客户请求的目标文件名
    char* m_version;        // 版本号
    char* m_host;           // 主机名
    int m_content_length;   // HTTP请求的消息体的长度
    bool m_linger;          // HTTP请求是否要求保持连接
    
    CHECK_STATE m_check_state;  // 主状态机所处状态  
    char* m_file_address;       // 客户请求的目标文件被mmap映射到内存的起始位置（mmap为内存映射文件的一种方法）
    struct stat m_file_stat;    // 目标文件的状态（是否存在，是否为文件夹，是否可读，大小等信息）
    struct iovec m_iv[2];       // 我们将采用writev集中写操作，将http相应写入sockfd，所以会有这两个变量，
    int m_iv_count;             // 其中iovec表示被写内存块，m_iov_count表示内存块数量，在process_write函数中，根据响应码判断是1还是2
                                // 数量为2是因为，一个内存块用于写相应头部，一个写目标文件内容，如果目标文件不存在则只用到一个

    int cgi;                // 是否启用POST
    char *m_string;         // 存储请求头数据
    // int bytes_to_send;
    // int bytes_have_send;
    // char *doc_root;

    map<string, string> m_users;
    // int m_TRIGMode;
    int m_close_log;
    int bytes_to_send;
    int bytes_have_send;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];

};

#endif