#ifndef LOCKERH
#define LOCKERH

#include <semaphore.h>
#include <pthread.h>
#include <exception>


// 封装信号量的类
class sem
{
public:
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0)    // 参数：信号量、类型（0表示当前进程内的局部信号量，否则此信号量可以在进程间共享）、信号量初值
        {
            // 构造函数没有返回值，可以通过抛出异常类来报告错误
            throw std::exception();
        }
    }

    sem (int num) {
        if (sem_init(&m_sem, 0, num) != 0) {
            throw std::exception();
        }
    } 

    ~sem()
    {
        sem_destroy(&m_sem);
    }

    // 等待信号量
    bool wait()
    {
        return sem_wait(&m_sem) == 0;   // P操作：以原子操作形式将信号量减1。如果信号量值为0，则阻塞，直到信号量非零
    }

    // 增加信号量
    bool post()
    {
        return sem_post(&m_sem) == 0;   // V操作：以原子操作形式将信号量加1。
    }
private:
    sem_t m_sem;

};

// 封装互斥锁的类
class locker
{
public:
    locker()
    {
        // 参数：互斥锁；类型（NULL表默认类型，为普通锁，等待该锁的线程将按照优先级形成一个队列。如果对普通锁再次加锁将引发死锁）
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }

    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

// 封装条件变量的类
class cond
{
public:
    cond()
    {
        // if (pthread_mutex_init(&m_mutex, NULL) != 0)
        // {
        //     throw std::exception();
        // }
        // 参数：互斥锁；类型（和互斥锁类似）
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            throw std::exception();
        }
    }

    ~cond()
    {
        // pthread_mutex_destroy(&m_mutex);
        pthread_cond_destroy(&m_cond);
    }

    // 等待条件变量
    bool wait(pthread_mutex_t *m_mutex)
    {
        // pthread_cond_wait()函数用于等待目标条件变量，mutex参数是用于保护条件变量的互斥锁，以确保Pthread_cond_wait操作的原子性
        // 在调用pthread_cond_wait()之前，要确保互斥锁mutex已经加锁，否则将导致不可预期的后果
        // 在pthread_cond_wait执行时，先将电泳线程放入条件变量的等待队列中，然后将互斥锁mutex解锁
        // 当pthread_cond_wait成功返回时，mutex将会再次锁上
        int ret = 0;
        // pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex);
        // pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }

    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        // pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        // pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }

    // 唤醒等待条件变量的线程
    bool signal()
    {
        return pthread_cond_signal(&m_cond);
    }

    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }


private:
    // static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;

};


#endif