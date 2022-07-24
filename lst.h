#ifndef LST_H
#define LST_H
#include<time.h>
#include<arpa/inet.h>
#include<vector>
#define BUFFER_SIZE 64
using namespace std;
class util_timer;


class util_timer{
public:
    util_timer():prev(NULL),next(NULL){}

public:
    time_t expire; //任务的超时时间，这里使用绝对时间
    int sockfd; //这个节点对应的连接
    util_timer*prev;
    util_timer*next;
};

class sort_timer_lst{
public:
    sort_timer_lst():head(),tail(){}
    ~sort_timer_lst();//析构函数，负责销毁链表
    void add_timer(util_timer*timer);//添加定时器到链表中
    void adjust_timer(util_timer*timers);//调整链表
    void del_timer(util_timer*timer);//删除一个定时器
    vector<int> tick();

private:
    //一个辅助的重载函数，被公有的add_timer调用，负责将一个添加到Head之后的链表中
    void add_timer(util_timer*timer,util_timer*lst_head);
    util_timer*head;
    util_timer*tail;

};

#endif
