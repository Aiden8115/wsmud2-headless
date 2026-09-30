// thsafe.hpp — 线程安全队列：触发引擎 worker 线程与主线程的消息传递
// 输入/输出队列都装 std::function<void()>：由发送方 push，由消费方线程执行。
#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>

template <typename T>
class ConcurrentQueue {
public:
    ConcurrentQueue() = default;

    // 阻塞入队（通知 worker 唤醒）
    void push(T t) {
        std::lock_guard<std::mutex> lk(m_);
        q_.push(std::move(t));
        cv_.notify_one();
    }
    // 阻塞出队；stop() 置位后若队列清空则返回 false（消费线程据此退出）
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
        if (stop_ && q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }
    // 非阻塞出队（供主线程每帧排空输出队列；无数据返回 false）
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }
    void stop() {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        cv_.notify_all();
    }
    void reset() {
        std::lock_guard<std::mutex> lk(m_);
        while (!q_.empty()) q_.pop();
        stop_ = false;
    }

private:
    std::queue<T> q_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};