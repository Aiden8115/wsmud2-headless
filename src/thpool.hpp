// thpool.hpp — 共享触发器 worker 线程池：多条账号复用固定数量的后台线程。
// 早期实现是"每账号一条 std::thread"：账号多了会为每个登录角色各占一个 CPU 核心，
// 线程数随账号数无限增长，对小主机负担过大。
// 现改为收敛为 2 条线程的模型：
//   - 主线程（1 条）：专职"非触发器"——网络收发、协议解析、UI 渲染、状态、账号管理；
//   - 触发器 worker（默认 1 条）：全部账号的触发器脚本串行执行。重脚本只在后台 worker
//     上跑，不冻结主线程事件循环；
// 即程序线程总数恒为 2（除非用 WSMUD_WORKERS 显式调大触发 worker 的并行度）。
// 其余性质：
//   - 空闲 worker 阻塞在条件变量上，不占 CPU 核心，只在真正执行脚本时才用核；
//   - 账号数可任意多，超出线程数时多个账号复用同一条 worker（顺序执行，仍是后台
//     单线程，不冻结 UI 事件循环）；
//   - QuickJS runtime 始终在账号绑定的同一条 worker 上创建与释放（线程亲和），
//     账号只要保持绑定不换 worker 即安全。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "thsafe.hpp"

// 全局有界 worker 线程池（无 namespace 的单例，供所有账号共享）
class WorkerPool {
public:
    static WorkerPool& instance();

    // 绑定一条 worker 并返回其索引；倾向未使用的空闲 worker，否则共享负载最轻者。
    // 账号在整个生命周期保持该绑定（QuickJS 线程亲和）。永不失败。
    int acquire();
    // 归还绑定，减少该 worker 的持有人计数（可被其他账号复用）。
    void release(int w);
    // 向指定 worker 提交任务，由其线程执行。
    void submit(int w, std::function<void()> fn);
    // 指定 worker 队列是否为空（供主线程高频 tick 的合并判断）。
    bool qempty(int w);
    // 停止并 join 所有 worker（程序退出时调用一次；重复调用幂等）。
    void shutdown();

private:
    WorkerPool() = default;
    ~WorkerPool() { shutdown(); }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void ensure_started();                  // 首次使用时才真正建线程（无账号则为零线程）
    void loop(int idx);                     // 单条 worker 的运行体

    int n_ = 0;                             // worker 条数（ensure_started 后定下来）
    std::vector<std::thread> ths_;          // 各 worker 线程
    // ConcurrentQueue 不可移动：用 std::deque（增长时不移动已有元素），元素就地构造
    std::deque<ConcurrentQueue<std::function<void()>>> qs_;  // 各 worker 的任务队列
    std::vector<int> usage_;                // 各 worker 当前绑定的账号数（>1 表示被共享）
    std::mutex usage_mu_;                   // 保护 usage_
    std::once_flag start_flag_;             // 确保线程只创建一次
    std::atomic<bool> stopped_{false};      // shutdown 后不可再提交
};

// ---------- 实现（头文件即源，随用随编译） ----------

inline WorkerPool& WorkerPool::instance() {
    static WorkerPool p;
    return p;
}

inline void WorkerPool::ensure_started() {
    std::call_once(start_flag_, [this] {
        // 默认仅 1 条触发器 worker（与主线程合起来共 2 条线程）。
        // 如需多个触发器并行，可用 WSMUD_WORKERS 增大（>=1）。
        int n = 1;
        if (const char* env = std::getenv("WSMUD_WORKERS")) {
            int e = std::atoi(env);
            if (e >= 1) n = e;
        }
        n_ = n;
        // ConcurrentQueue 不可移动：用 std::deque，resize 就地默认构造、增长不搬元素
        qs_.resize(static_cast<std::size_t>(n));
        usage_.assign(static_cast<std::size_t>(n), 0);
        ths_.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            ths_.emplace_back(&WorkerPool::loop, this, i);
    });
}

inline void WorkerPool::loop(int idx) {
    for (;;) {
        std::function<void()> fn;
        if (!qs_[static_cast<std::size_t>(idx)].pop(fn)) break;  // stop 且队列清空 → 退出
        fn();
    }
}

inline int WorkerPool::acquire() {
    ensure_started();
    std::lock_guard<std::mutex> lk(usage_mu_);
    int best = 0;
    for (int i = 1; i < n_; ++i)
        if (usage_[static_cast<std::size_t>(i)] < usage_[static_cast<std::size_t>(best)]) best = i;
    ++usage_[static_cast<std::size_t>(best)];
    return best;
}

inline void WorkerPool::release(int w) {
    std::lock_guard<std::mutex> lk(usage_mu_);
    if (w >= 0 && w < n_ && usage_[static_cast<std::size_t>(w)] > 0)
        --usage_[static_cast<std::size_t>(w)];
}

inline void WorkerPool::submit(int w, std::function<void()> fn) {
    if (stopped_.load() || w < 0 || static_cast<std::size_t>(w) >= qs_.size()) return;
    qs_[static_cast<std::size_t>(w)].push(std::move(fn));
}

inline bool WorkerPool::qempty(int w) {
    if (static_cast<std::size_t>(w) >= qs_.size()) return true;
    return qs_[static_cast<std::size_t>(w)].empty();
}

inline void WorkerPool::shutdown() {
    if (stopped_.exchange(true)) return;   // 已关闭则直接返回
    ensure_started();                       // 确保线程已建（否则 n_=0 无对象可停）
    for (auto& q : qs_) q.stop();
    for (auto& t : ths_)
        if (t.joinable()) t.join();
    qs_.clear();
    ths_.clear();
}