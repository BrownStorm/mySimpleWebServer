#ifndef THREADPOOLH
#define THREADPOOLH

#include <queue>
#include "locker.h"
#include "log.h"


template<typename T>
class threadpool
{
public:
    threadpool(int thread_number = 8, int max_request = 100000);
    ~threadpool();
    bool append(T* request);

private:
    static void* work(void* arg);

private:
    int m_thread_number;            // 线程池中最大线程数量
    pthread_t* m_thread;            // 描述线程池的数组
    int m_max_request;              // 最大请求数，即工作队列中可滞留的最大任务数
    std::queue<T*> m_workqueue;     // 请求队列
    locker m_queuelocker;           // 保护请求队列的互斥锁
    sem m_quetestat;                // 是否有任务需要处理
    bool m_stop;                    // 是否结束线程
};

template<typename T>
threadpool<T>::threadpool(int thread_num, int max_request): 
    m_thread_number(thread_num), m_max_request(max_request), m_thread(NULL), m_stop(false)
{
    if (m_thread_number <= 0 || m_max_request <= 0)
    {
        throw std::exception();
    }
    m_thread = new pthread_t[m_thread_number];
    if (!m_thread)
    {
        throw std::exception();
    }
    for (int i = 0; i < m_thread_number; i++)
    {
        Log::get_instance()->write_log(1, "create the %dth thread\n", i);
        // 线程地址，属性，线程要运行的函数，此函数的参数
        if (pthread_create(&m_thread[i], NULL, work, this) != 0) 
        {   
            delete [] m_thread;
            throw std::exception();
        }
        // 线程默认的状态是joinable，意思是pthread_exit()之后不会释放线程所占用的堆栈和线程描述符
        // 调用pthread_detach则将其状态改为unjoinable，确保资源的释放
        if (pthread_detach(m_thread[i]))
        {
            delete [] m_thread;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool()
{
    delete [] m_thread;
    m_stop = true;
}

template<typename T>
void* threadpool<T>::work(void* arg)
{
    threadpool* pool = (threadpool*)arg;

    // 原run()
    while (!pool->m_stop)
    {
        pool->m_quetestat.wait();
        pool->m_queuelocker.lock();
        if (pool->m_workqueue.empty())
        {
            pool->m_queuelocker.unlock();
            continue;
        }
        T* request = pool->m_workqueue.front();
        pool->m_workqueue.pop();
        pool->m_queuelocker.unlock();
        if (!request)
        {
            continue;
        }
        request->process();
    }

    return pool;
}

template<typename T>
bool threadpool<T>::append(T* request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_request)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push(request);
    m_queuelocker.unlock();
    m_quetestat.post();
    return true;
}
#endif