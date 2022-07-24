#include"lst.h"
#include<stdio.h>
sort_timer_lst::~sort_timer_lst()
{
    util_timer*tmp = head;
    while(tmp) {
        head = tmp->next;
        delete tmp;
        tmp = head;     
    }
}

void sort_timer_lst::add_timer(util_timer*timer)
{
    if(!timer) return;
    if(head == NULL) {
        head = tail = timer;
        return;
    }
    
    if(timer->expire < head->expire) {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer,head);
}
//调整函数，调整定时器在链表中的位置
void sort_timer_lst::adjust_timer(util_timer*timer)
{
    if(!timer) return;
    util_timer*tmp = timer->next;
    //在以下情况不用调整
    if(!tmp||timer->expire < tmp->expire) return;

    if(timer == head) 
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer,head);
        
    }
    else{
        timer->next->prev = timer->prev;
        timer->prev->next = timer->next;
        add_timer(timer,timer->next);
    }
}
void sort_timer_lst::add_timer(util_timer*timer, util_timer*lst_head)
{
    util_timer*prev = lst_head;
    util_timer*tmp = prev->next;
    while(tmp) {
        if(timer->expire < tmp->expire) 
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        else{
            prev = tmp;
            tmp = tmp->next;
        }
        if(!tmp) 
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }
}
void sort_timer_lst::del_timer(util_timer*timer)
{
    if(!timer) 
    {
        return;
    }
    if(timer == head && timer == tail) {
        delete head;
        head = tail = NULL;
        return;
    }
    if(timer == head) {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if(timer == tail) {
        timer->prev->next = NULL;
        tail = timer->prev;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
vector<int> sort_timer_lst::tick()
{
    vector<int> result;
    if(!head) 
    {
        return result;
    }
    printf("timer tick\n");
    time_t cur = time(NULL);
    util_timer*tmp = head;
    while(tmp) {
        if(cur < tmp->expire) {
            break;
        }
        int sockfd = tmp->sockfd;
        result.push_back(sockfd);
        head = tmp->next;
        if(head) 
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
    return result;
}