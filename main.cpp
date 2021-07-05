#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include <assert.h>
#include <libgen.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "timer_wheel.h"
#include "log.h"
#include "sql_connection_pool.h"

#define MAXFD               65535
#define MAX_EVENT_NUMBER    10000
#define TIMESLOT            1
#define TIMEOUT             100
#define BACKLOG             5

extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd(int epollfd, int fd);
extern int setnonblocking(int fd);

static int timer_sig_pipefd[2];
// 利用lst_timer.h中的升序链表来管理定时器
// static timer_wheel t_wheel;

timer_wheel http_conn::m_twheel;

void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(timer_sig_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

void addsig(int sig, void(handler)(int), bool restart=true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void show_error(int connfd, const char* info)
{
    Log::get_instance()->write_log(1, "%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭
void cb_func(http_conn* user_data) {
    epoll_ctl(user_data->m_epollfd, EPOLL_CTL_DEL, user_data->m_sockfd, 0);
    assert(user_data);
    close(user_data->m_sockfd);
    Log::get_instance()->write_log(1, "close fd %d\n", user_data->m_sockfd);
}

void timer_handler() {
    // 定时处理任务，实际上就是调用tick函数
    http_conn::m_twheel.tick();
    // 因为一次alarm调用只会引起一次SIGALRM信号，所以我们要重新定时，以不断触发SIGALRM信号
    alarm(TIMESLOT);
}

int main(int argc, char* argv[])
{
    if (argc <= 2)
    {
        Log::get_instance()->write_log(1, "usage: %s ip port_number\n", basename(argv[0]));
        return 1;
    }
    char* ip = argv[1];
    int port = atoi(argv[2]);
    int ret = 0;

    // 开启异步写日志 
    int LOGWrite = 1;
    // 默认日志不关闭
    int m_close_log = 0;

    //初始化日志
    if (1 == LOGWrite)
        Log::get_instance()->init("./ServerLog/Log", m_close_log, 2000, 800000, 800);
    else
        Log::get_instance()->init("./ServerLog/Log", m_close_log, 2000, 800000, 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(SIGPIPE, &sa, NULL) != -1);

    threadpool<http_conn>* pool = NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch(...)
    {
        return 1;
    }
    
    http_conn* users = new http_conn[MAXFD];
    assert(users);

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    // 第一个值：0表示关闭，1表示开启；第二个值：如果开启，套接口关闭时内核将拖延一段时间（这里是拖延0秒，就是立即关闭）
    // 
    // struct linger lin = {1, 0};     
    // setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, BACKLOG);
    assert(ret >= 0);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(1);
    assert(epollfd != -1);
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, timer_sig_pipefd);  // 创建管道
    assert(ret != -1);
    setnonblocking(timer_sig_pipefd[1]);
    addfd(epollfd, timer_sig_pipefd[0], false);

    //设置信号处理函数
    addsig(SIGALRM, sig_handler);
    addsig(SIGTERM, sig_handler);
    bool stop_server = false;

    bool timeout = false;
    alarm(TIMESLOT);        // 设置定时周期，即TIMESLOT为一个周期。一个周期触发一次tick()函数

    /* 启动数据库池 */
    connection_pool* connPool;
    string user = "root";           // 登陆数据库用户名
    string passWord = "123456";     // 登陆数据库密码
    string databaseName = "web";    // 使用数据库名
    int sql_num = 8;                // 数据库连接池大小

    // 初始化数据库连接池
    connPool = connection_pool::GetInstance();
    connPool->init("localhost", user, passWord, databaseName, port, sql_num);
    // connPool->init("192.168.136.123:858", user, passWord, databaseName, port, sql_num);

    // 初始化数据库读取表
    users->initmysql_result(connPool);

    while (1)
    {
        int count = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (count < 0 && errno != EINTR)
        {
            Log::get_instance()->write_log(3, "epoll failure\n");
            break;      // 不能return，因为还有很多东西没有close，还有很多内存没有释放
        }
        for (int i = 0; i < count; i++)
        {
            int sockfd = events[i].data.fd;         // 有事件的sockfd
            // 如果这个sockfd是listenfd的话，则表示有新的连接进来
            // 我们需要创建一个新的fd，名为connfd，作为与新连接沟通的fd
            // 如果此时epoll管理的fd数量小于最大值，则需要将其init，并加入epoll（在init中做了）
            // 否则传递给客户端信息，表示网络正忙
            if (events[i].data.fd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addresslen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addresslen);
                if (connfd < 0)
                {
                    Log::get_instance()->write_log(3, "errno is %d", errno);
                    return 1;
                }
                if (http_conn::m_user_count >= MAXFD)
                {
                    const char* info = "Internet busy\n";
                    Log::get_instance()->write_log(1, "%s\n", info);
                    send(connfd, info, sizeof(info), 0);
                    continue;
                }
                users[connfd].init(connfd, client_address);
                tw_timer* timer = http_conn::m_twheel.add_timer(TIMEOUT);
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                users[connfd].m_timer = timer;
            }
            // 处理信号
            else if ((sockfd == timer_sig_pipefd[0]) && (events[i].events & EPOLLIN)) 
            {
                int sig;
                char signals[1024];
                ret = recv(timer_sig_pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) {
                    // handle the error
                    continue;
                }
                else if (ret == 0) {
                    continue;
                }
                else {
                    for (int i = 0; i < ret; i++) {
                        switch (signals[i])
                        {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务。
                                // 这是因为定时任务的 优先级不是很高，我们优先处理其他更重要的任务
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            // 如果是error，则直接关闭（remove、close、用户数量减1）
            else if (events[i].events & EPOLLERR)
            {
                users[sockfd].close_conn();
            }
            // 如果是有数据要读，则根据read的结果（看看数据是否完整）判断是否要将其加入任务队列
            else if (events[i].events & EPOLLIN){
                if (users[sockfd].read())
                {
                    pool->append(users + sockfd);
                }
                else 
                {
                    users[sockfd].close_conn();
                }
            }
            // 如果是有数据要写，则根据写的结果判断是否要关闭
            else if (events[i].events & EPOLLOUT)
            {
                if (users[sockfd].write())
                {
                    
                }
                else 
                {
                    users[sockfd].close_conn();
                }
            }
            else
            {
                return 1;
            }
            // 最后处理定时时间，因为IO时间有更高的优先级。
            // 当然，这样做将导致定时任务不能精确地按照预期时间执行
            if (timeout) {
                timer_handler();
                timeout = false;
            }
        }
    }
    // 关闭所有fd，释放所有内存
    close(listenfd);
    close(epollfd);
    delete [] users;
    delete pool;
    return 0;
}