#include "http_conn.h"
#include "threadpool.h"
#include "log.h"
#include "redis_pool.h"

#include <mysql/mysql.h>
#include <fstream>
#include <iostream>

// 定义HTTP相应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "BAD Request";
const char* error_400_form = "Your rquest has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not requested file was not found on this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
// 网站的根目录
const char* doc_root = "/home/ltl/testLinux_code/myWebServer/4/root/";

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;
locker m_lock;
// map<string, string> user;

int setnonblocking(int sockfd)
{
    int old_option = fcntl(sockfd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(sockfd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int sockfd, bool oneshot=true)
{
    epoll_event event;
    event.data.fd = sockfd;
    event.events = EPOLLIN | EPOLLET;
    if (oneshot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &event);
    /*
        如果使用阻塞IO去读，在ET模式下，需要使用 while(1) 之类的循环，这就会导致在数据读完之后，最后一次 read 阻塞，因为所有的数据都已经读完了。
        而如果使用非阻塞IO，在ET模式下，循环读完数据之后会返回-1并将返回错误码EAGAIN，而不是简单的阻塞住。
    */
    setnonblocking(sockfd);
}

void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);  // 记住要del还不够，还需要close
}

void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

/*
void http_conn::initmysql_result(connection_pool *connPool) {
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlconn(&mysql, connPool);

    // 在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        user[temp1] = temp2;
    }
    // printf("initmysql_result\n");
    // string query = "select * from user";
    // printf("query execute:%s\n", query.c_str());
    // int t = mysql_query(mysql, query.c_str());
}
*/

// init-----------------

void http_conn::init(int sockfd, const sockaddr_in &address)
{
    m_sockfd = sockfd;
    m_address = address;
    // 避免TIME_WAIT状态：调试时使用
    int reuse = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd);
    m_user_count++;

    init();
}

void http_conn::init()
{
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_read_file, '\0', FILENAME_LEN);

    m_read_idx = 0;
    m_write_idx = 0;
    m_checked_idx = 0;
    m_start_line = 0;

    m_url = NULL;
    m_version = NULL;
    m_host = NULL;
    m_content_length = 0;
    m_linger = false;
    
    m_method = GET;
    m_check_state = CHECK_STATE_REQUESTLINE;    // 主状态机当前状态，初始化为“正在分析请求行”
    m_file_address = NULL;
    
    cgi = 0;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_state = 0;
    improv = 0;
}

// read---------------------

// 读取客户http请求。循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read()
{
    // 判断读缓冲区是否已满
    if(m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }

    int bytes_read = 0;
    // 循环读取
    while (1)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        //判断是否出错
        if (bytes_read == -1)
        {
            // 如果错误是EAGAIN，表示这一批数据已经读完，则break（在VxWorks和Windows上，EAGAIN的名字叫做EWOULDBLOCK）
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                /*
                    这里没有重新mod为什么？设置成oneshot不是触发一次之后除非再次mod就不再触发了么？
                    因为在这里不知道要设置成读还是写，要根据处理HTTP请求的结果来判断
                    而且如果现在设置成EPOLLIN，那么设置成oneshot的意义就不大了
                */
                break;
            }
            return false;
        }
        // 如果等待协议接收数据时网络中断了
        else if (bytes_read == 0)
        {
            return false;
        }
        // 都没问题则表示读取成功，m_read_idx记录最新的已经读入的客户端数据的最后一个字节的下一个位置
        m_read_idx += bytes_read;
        // 循环读取
    }
    m_timer->rotation = 10;
    return true;
}

// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATE http_conn::parse_line()
{
    /*
        checked_index指向buffer（应用程序的读缓冲区）中当前正在分析的字节；
        read_index指向buffer中客户数据的尾部的下一字节。
        buffer中的0~checked_index字节都已分析完毕，第checked_index~(read_index-1)字节由下面的循环挨个分析
    */
    char temp;
    for (; m_checked_idx < m_read_idx; m_checked_idx++)
    {
        // 获取当前要分析的字符
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                // 是一个完整行
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            // 如果'\r'是m_read_buf中的最后一个字符
            // 则说明这次没有读取到一个完整的行
            // 返回LINE_OPEN表示目前还需要继续读取客户数据才能进一步分析
            else if (m_checked_idx + 1 == m_read_idx)
            {
                return LINE_OPEN;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            // 要求 m_checked_idx > 1 是因为
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析HTTP请求行，获得请求方法，目标URL，以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    /*
        size_t strspn(str1, str2): str1 中第一个不在字符串 str2 中出现的字符下标。
        char* strpbrk(str1, str2): str1 中第一个匹配字符串 str2 中字符的字符串，不包含空结束字符。也就是说，依次检验字符串 str1 中的字符，当被检验字符在字符串 str2 中也包含时，则停止检验，并返回该字符串
        strncasecmp(str1, str2, n): 用来比较参数str1和str2字符串前n个字符，比较时会自动忽略大小写的差异。相同，则返回0；若s1大于s2，则返回大于0的值；若s1小于s2，则返回小于0的值。
        strchr(str, c): 参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
    */
    m_url = strpbrk(text, " \t");           // 略去多余的空格
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';                        // 得到method，并使m_url指向其首地址
    char* method = text;
    if (strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else if (strcasecmp(method, "POST") == 0) {
        m_method = POST;
        cgi = 1;
    }
    else 
    {
        return BAD_REQUEST;
    }
    m_url = m_url + strspn(m_url, " \t");   // 略去多余的空格
    m_version = strpbrk(m_url, " \t");

    if (!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';                    // 得到m_url，并使m_version指向其首地址
    m_version += strspn(m_version, " \t");  // 略去多余的空格

    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');         // 请求文件的名字（）带'/'
    }
    if (!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析HTTP请求的一个头部信息。包含：Connection（判断keep-alive）、Content-Length（content部分的长度）、Host：主机
http_conn::HTTP_CODE http_conn::parse_header(char* text)
{
    // 因为我们在parse_line从状态机中，每次获取到一个完整行，都会把最后的\r\n替换成\0\0
    // 所以说空行只有\0\0，当首字符为'\0'时表示头部字段解析完毕
    if (text[0] == '\0')
    {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体
        // 状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到一个完整的HTTP请求
        return GET_REQUEST;
    }
    // 处理Connection头部字段
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    // 处理Content-Length头部字段
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    // 处理Host头部字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "User-Agent:", 10) == 0)
    {
        text += 10;
        text += strspn(text, " \t");
    }
    else 
    {
        Log::get_instance()->write_log(3, "oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
}

// 我们没有真正的解析HTTP请求的消息体，只是简单地判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::pares_content(char* text)
{
    // 因为在主状态机中，先判断当前是否是content字段，再进入从状态机判断行是否完整
    // 所以可以使用content字段的长度 + 从状态机中已经读取的行的字符数，来判断是否完整的读入。
    if (m_read_idx >= m_content_length + m_checked_idx)
    {
        text[m_content_length] = '\0';
        // POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_read_file, doc_root);

    // strcpy(m_read_file + strlen(doc_root), m_url);

    int len = strlen(doc_root);
    const char *p = strrchr(m_url, '/');

    // 处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {
        // 根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_read_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来
        // user = 123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; i++) {
            name[i - 5] = m_string[i];
        }
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j) {
            password[j] = m_string[i];
        }
        password[j] = '\0';

        if (*(p + 1) == '3') {
            // 从数据库连接池中取一个连接
            MYSQL *mysql = NULL;
            connectionRAII mysqlconn(&mysql, connection_pool::GetInstance());

            // 如果是注册，先检测数据库中是否有重名的
            // 没有重名的，进行增加数据
            if (!mysql) {
                Log::get_instance()->write_log(3, "mysql error: %s", mysql_error(mysql));
            }
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            Log::get_instance()->write_log(1, sql_insert);

            // 在user表中检索username，passwd数据，浏览器端输入
            if (mysql_query(mysql, "SELECT username,passwd FROM user"))
            {
                LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
            }

            // 从表中检索完整的结果集
            MYSQL_RES *result = mysql_store_result(mysql);

            // 返回结果集中的列数
            int num_fields = mysql_num_fields(result);

            // 返回所有字段结构的数组
            MYSQL_FIELD *fields = mysql_fetch_fields(result);

            // 从结果集中获取下一行，一一比对
            bool password_is_true = false;
            while (MYSQL_ROW row = mysql_fetch_row(result))
            {
                string temp1(row[0]);
                string temp2(row[1]);
                if (temp1 == name && temp2 == password) {
                    RedisPool::GetInstance()->setString(name, password);
                    password_is_true = true;
                    strcpy(m_url, "/welcome.html");
                    break;
                }
            }
            if (!password_is_true) {
                m_lock.lock();
                
                int res = mysql_query(mysql, sql_insert);
                // user.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res) {
                    strcpy(m_url, "/log.html");
                } else {
                    strcpy(m_url, "/registerError.html");
                }
            } else {
                strcpy(m_url, "/registerError.html");
            }

            // if (user.find(name) == user.end()) {
            //     m_lock.lock();
                
            //     int res = mysql_query(mysql, sql_insert);
            //     user.insert(pair<string, string>(name, password));
            //     m_lock.unlock();

            //     if (!res) {
            //         strcpy(m_url, "/log.html");
            //     } else {
            //         strcpy(m_url, "/registerError.html");
            //     }
            // } else {
            //     strcpy(m_url, "/registerError.html");
            // }
            
            
        } 
        // 如果是登录，直接判断
        // 若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2') {
            string redis_password = RedisPool::GetInstance()->getString(name);
            if (redis_password == password) {
                strcpy(m_url, "/welcome.html");
            } else if (redis_password == "error") {

            } else if (redis_password == "") {
                // 先从连接池中取一个连接
                MYSQL *mysql = NULL;
                connectionRAII mysqlconn(&mysql, connection_pool::GetInstance());

                // 在user表中检索username，passwd数据，浏览器端输入
                if (mysql_query(mysql, "SELECT username,passwd FROM user"))
                {
                    LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
                }

                // 从表中检索完整的结果集
                MYSQL_RES *result = mysql_store_result(mysql);

                // 返回结果集中的列数
                int num_fields = mysql_num_fields(result);

                // 返回所有字段结构的数组
                MYSQL_FIELD *fields = mysql_fetch_fields(result);

                // 从结果集中获取下一行，一一比对
                bool password_is_true = false;
                while (MYSQL_ROW row = mysql_fetch_row(result))
                {
                    string temp1(row[0]);
                    string temp2(row[1]);
                    if (temp1 == name && temp2 == password) {
                        RedisPool::GetInstance()->setString(name, password);
                        password_is_true = true;
                        strcpy(m_url, "/welcome.html");
                        break;
                    }
                }
                if (!password_is_true) {
                    strcpy(m_url, "/logError.html");
                }
            } else {
                strcpy(m_url, "/logError.html");
            }
            
        }
    }

    if (*(p + 1) == '0') {
        char *m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_read_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '1') {
        char *m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_read_file + len, m_url_real, strlen(m_url_real));
    
        free(m_url_real);
    } else if (*(p + 1) == '5') {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_read_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '6') {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_read_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '7') {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_read_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else {
        strncpy(m_read_file + len, m_url, FILENAME_LEN - len - 1);
    }
    // cout << *(p + 1) << " " << m_url << endl;
    // stat：获取文件状态
    if (stat(m_read_file, & m_file_stat) < 0)
    {
        return NO_RESOURCE;
    }

    // S_ISDIR()：目标文件是目录
    if (!(m_file_stat.st_mode & S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }

    // S_ISDIR()：目标文件是目录
    if (S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST;
    }

    // 创建一个内存段，并将文件映射到内存段中
    // PROT_READ：内存段可读
    // MAP_PRIVATE：内存段为调用进程私有，对该内存段的修改不会反映到被映射的文件中
    int fd = open(m_read_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    close(fd);
    return FLIE_REQUEST;
}

// 主状态机
http_conn::HTTP_CODE http_conn::process_read()
{
    // 记录当前行的读取状态，其状态由从状态机中获取
    LINE_STATE line_status = LINE_OK;
    // 记录HTTP请求的处理结果
    HTTP_CODE ret = NO_REQUEST;
    char* text = NULL;
    // 1、因为我们只解析请求行和首部字段，不解析content，所以我们先判断接下来要解析的字段是否是content字段
    // 如果是，并且前一行完整，则不需要进入从状态机，直接进入循环
    // 如果不是，则需要进入从状态机，判断接下来的行的状态
    // CHECK_STATE_CONTENT状态是当首部字段全部读完，并且，Content-Length不为0时，会将m_check_state改为CHECK_STATE_CONTENT

    // 2、这里的line_status是判断当前行是否完整
    // 而m_check_line是由上一行判断的，这一行的状态
    while ((line_status == LINE_OK && m_check_state == CHECK_STATE_CONTENT) 
            || (line_status = parse_line()) == LINE_OK)
    {
        // 获取当前行
        text = get_line();
        // 记录下一行的起始位置
        m_start_line = m_checked_idx;
        Log::get_instance()->write_log(1, "got 1 http line: %s. Has %d bytes\n", text, m_start_line);
        switch (m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_header(text);
                if (ret == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if (ret == GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = pares_content(text);
                if (ret == GET_REQUEST)
                {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
                break;
            }
        }
    }
    return NO_REQUEST;
}

bool http_conn::write()
{
    // int bytes_to_send, bytes_have_send = 0;
    int temp = 0;

    int newadd = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        newadd = bytes_have_send - m_write_idx;
        bytes_to_send -= temp;  // meiyou
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            // m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_base = m_file_address + newadd;
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char* format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - m_write_idx - 1,  // 因为有'\0'，所以要-1
                            format, arg_list);
    va_end(arg_list);
    if (len >= WRITE_BUFFER_SIZE - m_write_idx - 1)
    {
        return false;
    }
    m_write_idx += len;
    return true;
    /*
        #include <stdio.h>
            int printf(const char *format, ...);                                 //输出到标准输出
            int fprintf(FILE *stream, const char *format, ...);                  //输出到文件
            int sprintf(char *str, const char *format, ...);                     //输出到字符串str中
            int snprintf(char *str, size_t size, const char *format, ...);       //按size大小输出到字符串str中
        
        v....与上面的函数一一对应，将上面的...对应的一个个变量用va_list调用所替代
        在函数调用前ap要通过va_start()宏来动态获取。
        va_start与va_end成对出现
    */
}

bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_header(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_bland_line();
}

bool http_conn::add_content_length(int length)
{
    return add_response("Content-Length: %d\r\n", length);
}

bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", m_linger == true ? "keep-alive" : "close");
}

bool http_conn::add_bland_line()
{
    return add_response("\r\n");
}

bool http_conn::add_content(const char* content)
{
    return add_response("%s", content);
}

// 对内存映射区执行munmap操作
bool http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = NULL;
    }
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE code)
{
    switch (code)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_header(strlen(error_500_form));
        if (!add_content(error_500_form))
        {
            return false;
        }
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(400, error_400_title);
        add_header(strlen(error_400_form));
        if (!add_content(error_400_form))
        {
            return false;
        }
        break;
    }
    case NO_RESOURCE:
    {
        add_status_line(404, error_404_title);
        add_header(strlen(error_404_form));
        if (!add_content(error_404_form))
        {
            return false;
        }
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_header(strlen(error_403_form));
        if (!add_content(error_403_form))
        {
            return false;
        }
        break;
    }
    case FLIE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_header(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else 
        {
            const char* ok_string = "<html><body></body></html>";
            add_header(strlen(ok_string));
            if (!add_content(ok_string))
            {
                return false;
            }
        }
    }
    default:
        break;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 工作线程调用的函数，处理用户请求。其中调用process_read();process_write();close_conn();
void http_conn::process()
{
    HTTP_CODE code = process_read();

    // 请求不完整，需要继续获取数据，所以不能向客户端写数据，而是要将sockfd改为EPOLLIN，并return
    if (code == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    // 将HTTP请求分析完，根据响应码返回相应写HTTP响应
    // 如果写（组织）数据的时候出现了问题，则直接close_conn();否则将sockfd改为EPOLLOUT
    if (!process_write(code))
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}

void http_conn::close_conn(bool real_close)
{
    if (real_close && m_sockfd != -1)
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
        m_twheel.del_timer(m_timer);
    }
    
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭
void http_conn::timer_cb_func(http_conn* user_data) {
    epoll_ctl(m_epollfd, EPOLL_CTL_DEL, user_data->m_sockfd, 0);
    assert(user_data);
    close(user_data->m_sockfd);
    Log::get_instance()->write_log(1, "close fd %d\n", user_data->m_sockfd);
}