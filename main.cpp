#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include"locker.h"
#include"pthread_pool.h"
#include<signal.h>
#include<iostream>
#include "http_conn.h"
#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量
#define TIMESLOT 5 //设定超时时间
using namespace std;
static int pipefd[2];
// 添加文件描述符
//想想这里为什么一定要加extern,函数使用之前必须声明
extern void addfd( int epollfd, int fd, bool one_shot );
extern void removefd( int epollfd, int fd );
extern int setnonblocking(int);

//捕捉 SIGPIPE信号
//void( *handler )(int) 函数指针，终于看懂了
void addsig(int sig, void( *handler )(int)){
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    //handler是处理函数
    sa.sa_handler = handler;
    //在信号捕捉函数处理的过程中，临时阻塞某些信号
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}
void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

void timer_handler(threadpool< http_conn >* pool)
{
    pool->lst.tick();
    alarm(TIMESLOT);
}

//采用ET模式
//模拟 Proactor 模式
int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi( argv[1] );
    //忽略SIGPIPE信号
    addsig( SIGPIPE, SIG_IGN );
    //信号处理函数负责将信号传递给主循环
    addsig(SIGALRM, sig_handler);

    //信号处理机制
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);

    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }
    //创建一个数组用于保存所有的客户端信息
    http_conn* users = new http_conn[ MAX_FD ];


    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );


    if(listenfd == -1) {
        perror("socket");
        exit(-1);
    }
    /*
    INADDR_ANY
    转换过来就是0.0.0.0，泛指本机的意思，也就是表示本机的所有IP，因为有些机子不止一块网卡，多网卡的情况下，这个就表示所有网卡ip地址的意思。
    比如一台电脑有3块网卡，分别连接三个网络，那么这台电脑就有3个ip地址了，如果某个应用程序需要监听某个端口，那他要监听哪个网卡地址的端口呢？

    如果绑定某个具体的ip地址，你只能监听你所设置的ip地址所在的网卡的端口，其它两块网卡无法监听端口，如果我需要三个网卡都监听，那就需要绑定3个ip，也就等于需要管理3个套接字进行数据交换，这样岂不是很繁琐？

    所以出现INADDR_ANY，你只需绑定INADDR_ANY，管理一个套接字就行，不管数据是从哪个网卡过来的，只要是绑定的端口号过来的数据，都可以接收到。
    */
    ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 端口复用，在绑定之前设置
    //端口复用的作用：
    //防止服务器重启时之前绑定的端口还未释放
    //程序突然退出没有释放端口
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    if(ret == -1) {
        perror("bind");
        exit(-1);
    }
    //listen函数的第二参数用来设定正在连接或者等待连接的最大数量（TCP连接是一个过程）
    ret = listen( listenfd, 5 );

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    // 添加到epoll对象中
    addfd( epollfd, listenfd, false );

    addfd(epollfd, pipefd[0], false);
    http_conn::m_epollfd = epollfd;

    //负责处理是否超时的变量
    bool timeout = false;
    //先发送一次信号
    alarm(TIMESLOT);

    while(true) {
        if (timeout) {
            vector<int> result = pool->lst.tick();
            for(auto fd : result) {
                users[fd].close_conn();
                printf("hello close\n");
                users[fd].timer = NULL;
                }
                alarm(TIMESLOT);    
                timeout = false;
    }     

        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        

        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            if( sockfd == listenfd ) {

                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );

                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {
                    //目前连接数满了
                    //给客户端一个信息，服务端正忙
                    close(connfd);
                    continue;
                }
                //将新客户的数据初始化，放在数组中
                users[connfd].init( connfd, client_address);

                //将链表放入线程池中
                util_timer * timer = new util_timer;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                timer->sockfd = connfd;
                users[sockfd].timer = timer;
                pool->lst.add_timer(timer);
               
            }
            //处理信号
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1) {
                    continue;
                }
                else if(ret == 0) {
                    continue;
                }
                else 
                {
                    for(int i = 0; i< ret; i++) {
                        
                        switch(signals[i])
                        {
                            case SIGALRM:
                            {
                                //printf("hello\n");
//用timeout变量标识有定时任务需要处理，但不立即处理定时任务，这是因为定时任务的优先级不是很高，
//我们优先处理其他更重要的任务     
                                timeout = true;
                                break;
                            }                               
                        }
                    }
                }
            }
            //对方异常断开或者错误等事件            
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                pool->lst.del_timer(users[sockfd].timer);
                users[sockfd].timer = NULL;
                users[sockfd].close_conn();
                
            } 
            //et模式一次性读取所有数据
            else if(sockfd != pipefd[0] && events[i].events & EPOLLIN) {

                if(users[sockfd].read()) {
                    //读取所有数据后将其添加到，工作队列的末尾
                    pool->append(users + sockfd);
                    util_timer * timer = users[sockfd].timer;
                    if(timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        printf("adjust timer once\n");
                        pool->lst.adjust_timer(timer);
                    } 
                } else {
                    pool->lst.del_timer(users[sockfd].timer);
                    users[sockfd].timer = NULL;
                    users[sockfd].close_conn();
                }
               

            }  
            else if( events[i].events & EPOLLOUT ) {

                if( !users[sockfd].write() ) {
                    pool->lst.del_timer(users[sockfd].timer);
                    users[sockfd].timer = NULL;
                    users[sockfd].close_conn();
                }

            }
        }
    }
    
    close( epollfd );
    close( listenfd );
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    delete pool;
    return 0;
}