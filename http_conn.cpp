#include"http_conn.h"
#include<sys/epoll.h>

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
// 网站的根目录
const char* doc_root = "/root/webserver/resources";
int http_conn::m_user_count = 0; 
int http_conn::m_epollfd = -1;
//设置文件描述符非阻塞
void setnonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);
    old_flag |= O_NONBLOCK;
    fcntl(fd, F_SETFL, old_flag);
}
void addfd( int epollfd, int fd, bool one_shot ) {
    epoll_event event;
    event.data.fd = fd;
    //EPOLLIN是有数据达到，可以读取
    //连接断开可以检测
    event.events = EPOLLIN | EPOLLRDHUP |EPOLLET;

    if(one_shot) {
        event.events | EPOLLONESHOT;
    }

    
    /*
    EPOLLSHOT的作用主要用于多线程中
    epoll在某次循环中唤醒一个事件，并用某个工作进程去处理该fd，此后如果不注册EPOLLSHOT,
    在该fd时间如果工作线程处理的不及时，主线程仍会唤醒这个事件，并另派线程池中另一个线程也来处理这个fd。
    为了避免这种情况，需要在注册时间时加上EPOLLSHOT标志，EPOLLSHOT相当于说，某次循环中epoll_wait唤醒该事件fd后，
    就会从注册中删除该fd,也就是说以后不会epollfd的表格中将不会再有这个fd,也就不会出现多个线程同时处理一个fd的情况。
    */
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //设置文件描述符非阻塞,et模式要把文件描述符设置为非阻塞
    setnonblocking(fd);
}
void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

//修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {

    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

//初始化客户端的信息，并将其加入到文件描述符中
void http_conn::init(int sockfd, const sockaddr_in& addr) {
    //m_socket是客户端的端口号
    m_sockfd = sockfd;
    m_address = addr;
    //下面两行在实际使用时应该去掉
    //int reuse = 1;
    //setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    addfd(m_epollfd, m_sockfd, true);
    m_user_count++;//总用户数增加
    init();
}

//关闭连接

void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;//客户总数减1
    }
}

//循环读取所有数据，直到无数据刻度或对方关闭连接
//可以用csapp上的函数优化
bool http_conn::read(){
    if(m_read_idx >= READ_BUFFER_SIZE) return false;
    int bytes_read = 0;
    while(true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1) {
            //在非阻塞模式下没有读到数据怎么办 https://blog.csdn.net/Jiangtagong/article/details/119899853
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                //没有数据
                break;
            }
            return false;
        }
        else if(bytes_read == 0) return false;
        m_read_idx += bytes_read;
        //printf("%s\n",m_read_buf);
    }


    return true; 
}

// 主状态机，解析请求
//根据是请求行，请求头，和体分别做解析操作
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
                || ((line_status = parse_line()) == LINE_OK)) {
        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;
        //printf( "got 1 http line: %s\n", text );

        switch ( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ) {
        temp = m_read_buf[ m_checked_idx ];
        if ( temp == '\r' ) {
            if ( ( m_checked_idx + 1 ) == m_read_idx ) {
                return LINE_OPEN;
                //不太理解
            } else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ) {
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if( temp == '\n' )  {
            if( ( m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ) {
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}


// 解析HTTP请求的一个头部信息
//在循环里逐行解析请求头，如果碰到以下字段就保存请求头的信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {   
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，（请求行、请求头、请求体）
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

// 解析HTTP请求行，获得请求方法，目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    m_url += strspn(m_url, " \t");
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version += strspn(m_version, " \t");
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }
    /**
     * http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 '/'（一个无符号字符）的位置。并返回这个位置，如果没有返回空指针
        m_url = strchr( m_url, '/' );
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
    return NO_REQUEST;
}
// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

void http_conn::init()
{
    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
//这里因为设置了EPOLLONESHOT事件，所以每次使用都要再modfd一下
void http_conn::process() {

    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST ) {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }
    
    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}

http_conn::HTTP_CODE http_conn::do_request()
{   //将网站的根目录先复制到m_real_file中
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //字符串拼接函数，m_url不是完整地址而是目标主机下的地址，所以要拼接成完整的地址
    //GET /index/html HTTP/1.1 m_url = /index/html (就算是完整的也在处理函数里跳过了)
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    //获取状态信息，-1失败返回错误，0成功
    if(stat(m_real_file, &m_file_stat) < 0) {
        return NO_REQUEST;
    }
    //其他用户是否有读权限
    if(!(m_file_stat.st_mode & S_IROTH)) 
    {
        //表示没有足够的访问权限
        return FORBIDDEN_REQUEST;
    }
    //如果是目录文件
    if(S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }
    int fd = open(m_real_file, O_RDONLY);
    //分配一段共享内存，my_file_address是请求的资源在内存中的位置
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
//写HTTP响应
bool http_conn::write()
{
    int temp = 0;
    if(bytes_to_send == 0) 
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while(1) 
    {
        //分散写，将响应头部和数据发送出去
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if(temp <= -1) 
        {
            //如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件。
            //虽然在此期间，服务器·无法接受到同一客户下一个请求，但这可以保证连接的完整性
            if(errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        //这个小于等于号说明发送成功
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0)
        {
            // 没有数据要发送了
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
//往写缓冲区中写入待发送的数据
bool http_conn::add_response(const char * format, ...)
{
    if(m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    //第一个参数是输出位置，第二个是传输的字节数，第三个是可变参数的模式，最后是可变参数列表
    //https://blog.csdn.net/qq_43899283/article/details/124349972
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        return false;
    } 
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char * title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive":"close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content)
{
    return add_response("%s", content);
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
        case INTERNAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form)) 
            {
                return false;
            }
        }
        case BAD_REQUEST:
        {
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form))
            {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form)) 
            {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form))
            {
                return false;
            }
            break;
        }
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            //文件大小不为0
            if(m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                return true;
            }
            //文件大小为0
            else
            {
                const char * ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if( !add_content(ok_string))
                {
                    return false;
                }
            }
        }
        default:
        {
            return false;
        }
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}