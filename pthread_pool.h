#ifndef PTHREAD_POOL_H
#define PTHREAD_POOL_H
#include<pthread.h>
#include<list>
#include"locker.h"
#include<cstdio>
#include<exception>
//模板类的声明和定义要放在一起，否则C++编译器无法找到函数的定义
// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template<typename T>
class threadpool {
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    //在C++中使用pthread_creat函数时，第三个参数必须指向一个静态函数
    static void* worker(void* arg);
    void run();

private:
    // 线程的数量
    int m_thread_number;  
    
    // 描述线程池的数组，大小为m_thread_number    
    pthread_t * m_threads;

    // 请求队列中最多允许的、等待处理的请求的数量  
    int m_max_requests; 
    
    // 请求队列
    std::list< T* > m_workqueue;  

    // 保护请求队列的互斥锁
    locker m_queuelocker;   

    // 是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程          
    bool m_stop;                    
};
//T代表任务

//构造函数，初始化线程
template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) :
m_thread_number(thread_number),m_max_requests(max_requests),m_stop(false),m_threads(NULL) {
    //如果这些小于0，抛出异常
    if(m_thread_number <= 0 || m_max_requests <= 0) {
        throw std::exception();
    }
    m_threads = new pthread_t[m_thread_number];

    if(!m_threads) throw std::exception();

    //创建thread_number个线程，并将他们设置为线程分离

    for(int i = 0; i < m_thread_number; i++) {
        printf("cread %dth thread\n",i);
        //这些函数如果不返回0，就直接抛出异常
        if(pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete [] m_threads;
            throw std::exception();
        }
        //设置线程分离属性，被分离的线程在线程终止后自动释放资源
        if(pthread_detach(m_threads[i])) {
            delete [] m_threads;
            throw std::exception();          
        }
    }
}
//析构函数，释放线程数组
template<typename T>
threadpool< T >::~threadpool() {
    delete [] m_threads;
    m_stop = true;
}

//添加一个任务
template<typename T>
bool threadpool< T >::append(T * request) {
    //操作工作队列一定要加锁，因为他们被所有线程共享
    m_queuelocker.lock();
    //请求队列的数量大于我设置的最大请求队列数
    if(m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    //将请求队列加入到队尾
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    //信号量增加
    m_queuestat.post();
    return true;

}

//每个线程执行的函数
template<typename T>
void * threadpool<T>::worker(void * arg) {
    threadpool * pool = (threadpool*) arg;
    //为什么要这么干，类的静态成员函数要访问类的动态对象，只有两种方法
    //1.通过类的静态对象来调用。如果只有一个实例，类的静态函数可以通过类的唯一实例来访问动态函数成员
    //2.将类的对象作为参数传递给静态函数，在静态函数中引用这个对象，并调用其动态方法
    //本例将参数设置为this指针，在worker函数中获取该指针，采用第二种方法。
    pool->run();
    return pool;
}
template<typename T>
void  threadpool<T>::run() {
    //这里一直在循环
    while(!m_stop) {
        //信号量（加锁）减一
        //如果信号量为0，就会阻塞，只有一个线程会获得资源
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        T * request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request) continue;
        request->process();
    }
}
#endif