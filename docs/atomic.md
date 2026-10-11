# C++ `<atomic>`：从 CPU Cache 到 Linux Server

> 面向 tina 的学习笔记。预备知识：CSAPP 中的 Cache / 多核、Linux 的线程和共享地址空间、pthread Mutex、Futex、epoll、C++23。先讲“为什么”，再讲 API。本文示例以普通 C++ 源文件的 `#include <atomic>` 为准；在 tina 的 C++23 模块里可使用 `import std;` 获得相应标准库声明。

## 0. 先画一张地图

同一个进程中的多个线程共享虚拟地址空间，所以它们可能同时访问同一个变量：

~~~cpp
int counter = 0;

// 线程 A、B 都执行：
++counter;
~~~

看上去只有一行，实际通常是“读取旧值 → 加一 → 写回”。两线程可能都读取 0、都写回 1，结果少加了一次。**在 C++ 内存模型中，若没有同步且存在冲突的读写，这还是数据竞争（data race），属于未定义行为**；不只是“偶尔少算一次”。

`std::atomic<T>` 主要解决两件事：

1. **原子操作**：对这个对象的读 / 写 / 读改写不会被其他线程观察为“完成了一半”。例如两个 `fetch_add(1)` 不会丢失更新。
2. **跨线程同步与顺序约束**：通过内存序（memory order），规定哪些普通内存读写必须在别的线程看来先于或后于某个原子操作。

**不要把三件事混在一起：**

- **原子性**：一次读改写作为一个不可分割的操作参与并发。
- **缓存一致性（coherence）**：各核心对同一内存位置的缓存副本遵守硬件的一致性规则。
- **内存顺序 / 同步（ordering / synchronization）**：多个地址上的操作如何被编译器、CPU 排序，以及何时建立 C++ 的 happens-before（先行发生）关系。

MESI 类协议主要帮助实现缓存一致性，**不自动替你建立跨变量的 C++ 同步关系**。原子变量也不是“绕开 Cache，直接读写 DRAM”。

## 1. 最常用的类型和操作

~~~cpp
#include <atomic>
#include <cstdint>

std::atomic<bool> stopping{false};
std::atomic<std::uint64_t> next_id{1};

bool old = stopping.load();         // 原子读取
stopping.store(true);               // 原子写入
bool was = stopping.exchange(false);// 原子交换，返回旧值

auto id = next_id.fetch_add(1);     // 返回旧值，并将自身加 1
auto prev = next_id.fetch_sub(1);   // 返回旧值，并将自身减 1
~~~

不写内存序参数时，这些操作**默认使用 `std::memory_order_seq_cst`**（顺序一致性），最容易先写对。

常见 API：

| API | 含义 | 典型场景 |
| --- | --- | --- |
| `load(order)` | 原子读 | 读取退出标志 / 统计值 |
| `store(value, order)` | 原子写 | 发布状态 |
| `exchange(value, order)` | 原子交换并返回旧值 | “只有第一个调用者能成功启动” |
| `fetch_add / fetch_sub` | 原子加减并返回旧值 | 计数器、分配唯一 ID |
| `fetch_and / fetch_or / fetch_xor` | 原子位运算 | 状态位掩码 |
| `compare_exchange_weak/strong` | 比较并有条件地更新（CAS） | 状态机、自旋锁、无锁算法 |
| `wait / notify_one / notify_all`（C++20） | 等值等待和通知 | 简单状态变化通知 |

例如，分配连接 ID：

~~~cpp
std::atomic<std::uint64_t> next_connection_id{1};

std::uint64_t allocate_id() {
    return next_connection_id.fetch_add(
        1, std::memory_order_relaxed
    );
}
~~~

为什么返回值不会重复（在计数器未发生回绕的范围内）？因为 **fetch_add 是一个整体的 read-modify-write（RMW，读—改—写）原子操作**。多个核心不会都成功拿到同一个旧值。

区别于下面这种**错误**写法：

~~~cpp
// 即使 next_connection_id 是 atomic，也不是一次原子加一！
auto id = next_connection_id.load();
next_connection_id.store(id + 1);
~~~

中间可以插进另一个线程，导致重复 ID。使用 `fetch_add` 才把“读旧值+修改”变成一个原子 RMW。

补充：`atomic<T>` 并不支持所有 T 的任意操作。支持哪些成员与操作数类型有关；它也通常不能像普通变量一样随便复制赋值整个 atomic 对象。

## 2. CPU 到底怎样实现原子操作？

先考虑多核 CPU：

~~~text
Core 0 ── L1/L2 Cache ──┐
                        ├── 一致性互连 / 共享缓存 / 内存系统
Core 1 ── L1/L2 Cache ──┘
~~~

两个核心同时对同一位置做 `fetch_add`，硬件必须保证 RMW 操作按某个顺序完成，不能让两个核心同时基于同一个旧值完成修改。

在 x86-64 上，常见的原子 RMW 可能编译成带 `lock` 语义的指令（例如 `lock xadd`、`lock cmpxchg`）；CPU 利用缓存一致性和相关机制维持原子性。**这不意味着每次都“锁整个内存总线”**，更不意味着每次都进入 Linux 内核。

在不同架构上，可能使用别的原子指令或机制；最终由编译器和目标平台决定。

再注意两点：

- `std::atomic` **不保证所有类型的原子操作都 lock-free**。可用 `a.is_lock_free()` 查询运行时实现情况，`std::atomic<T>::is_always_lock_free` 查询编译期保证。
- “lock-free”只描述某种非阻塞进展性质，**不是零竞争、零缓存行转移或零延迟**。热点原子变量依然可能成为性能瓶颈。

## 3. CAS：Compare-And-Swap 是什么？

CAS（比较并交换）的思想很朴素：

> “如果这个变量仍然等于我以为的旧值，就改成新值；否则告诉我失败。”

~~~cpp
std::atomic<int> state{0};

int expected = 0;
bool success = state.compare_exchange_strong(expected, 1);
~~~

这相当于一个**不可分割**的条件动作：

~~~text
若 state == expected(0)：state ← 1，返回 true
否则：返回 false，并把 state 的实际值写入 expected
~~~

因此失败时的 `expected` **会被更新**，不一定还是你原来传进去的 0！

### strong 和 weak

- `compare_exchange_strong`：比较真的相等时，不会凭空报告比较失败。
- `compare_exchange_weak`：**允许伪失败**（明明相等也返回 false），但某些 CPU 上更适合放在重试循环中。

~~~cpp
std::atomic<int> state{0};
int expected = 0;

while (!state.compare_exchange_weak(
    expected, 1,
    std::memory_order_acquire,
    std::memory_order_relaxed
)) {
    expected = 0; // 再次尝试把 0 改为 1
}
~~~

这段只演示 CAS 重试，不是推荐的线程池等待方式：若长时间拿不到状态，空转会消耗 CPU。

双内存序版本里，第 3 个参数是**成功**时的顺序；第 4 个是**失败**时的顺序。失败只执行读取，没有成功的写入，因此失败顺序不能是 `release` 或 `acq_rel`，也不能比成功顺序更强。

CAS 是实现很多自旋锁、无锁链表和状态机的基本积木，但无锁结构里还有 ABA、对象回收等难题；**会用 CAS ≠ 已经能安全写无锁队列**。

## 4. 内存序：最容易理解错的部分

只记住一件事：**原子性管单次操作，内存序还管它与其他访问之间的关系。**

为什么需要“顺序”？

编译器为了优化可能重排指令，CPU 为了乱序执行和缓存性能也可能以不同于源码的时间安排执行、传播内存访问。硬件会遵守其内存模型，但 C++ 代码不能假定“源码先写的，另一个核一定先看到”。

`std::memory_order` 里最常见的是：

| 顺序 | 人话理解 | 适用 |
| --- | --- | --- |
| `relaxed` | 这次操作仍然原子，但不额外承担跨变量同步任务 | 独立计数器、唯一 ID |
| `release` | 发布：本线程此前的读写不能被移到这次发布之后 | 生产者写 ready |
| `acquire` | 接收：本线程之后的读写不能被移到这次获取之前 | 消费者读 ready |
| `acq_rel` | 对同一 RMW 同时获得 acquire 和 release 效果 | CAS / exchange 同时接力同步 |
| `seq_cst` | 在上述相应约束之外，让所有 seq_cst 原子操作服从一个全局一致的顺序 | 默认值；不确定时先用它 |

`memory_order_consume` 历史上与依赖关系有关，语义和实现长期有争议，**当前学习与项目代码中不要依赖它**。

### 4.1 release + acquire：发布一个普通数据

~~~cpp
int payload = 0;                // 普通 int，不是 atomic
std::atomic<bool> ready{false}; // 发布状态

void producer() {
    payload = 42; // 先准备数据
    ready.store(true, std::memory_order_release);
}

void consumer() {
    while (!ready.load(std::memory_order_acquire)) {
        // 演示自旋；实际不要长期空转
    }
    // 这里读取 payload == 42 是有同步保障的
    use(payload);
}
~~~

关键**不是**“ready 的新值比较神奇”，而是：

1. 生产者先写 `payload`，再 **release-store** `ready=true`。
2. 消费者 **acquire-load** `ready`，并真正读到该 release 写入的值。
3. 因此生产者 release 之前的写入，对消费者 acquire 之后的读取建立 **happens-before** 关系；此示例中对普通 `payload` 的读取合法。

**如果消费者读到的还是 false，就还没完成这次同步。** 只有两侧操作形成正确的同步关系，才有保证。

这个示例是**一次性发布**：假设 `payload` 发布后不再被生产者并发修改，且在两线程执行前完成初始化。不能把 `ready` 简单改回 false 就当成可无限重复使用的无锁信箱，那需要另外设计协议和对象生命周期。

### 4.2 relaxed：适合统计，不适合靠它发布对象

~~~cpp
std::atomic<std::uint64_t> handled_requests{0};

void on_request_completed() {
    handled_requests.fetch_add(1, std::memory_order_relaxed);
}
~~~

这个计数器只需要“每次 +1 不丢失”，不需要用它告诉别的线程“某个 Response 对象已经初始化完毕”。因此 relaxed 很合适。

但这样**不成立**：

~~~cpp
int payload = 0;
std::atomic<bool> ready{false};

void producer() {
    payload = 42;
    ready.store(true, std::memory_order_relaxed);
}

void consumer() {
    if (ready.load(std::memory_order_relaxed)) {
        use(payload); // 没有建立同步，可能发生数据竞争
    }
}
~~~

即使 x86 上“看着总能运行”，按 C++ 内存模型也没有因 relaxed flag 建立保证。**不要靠 CPU 某次实测表现替代语言级同步。**

### 4.3 seq_cst 是不是最安全？

对于单个原子对象的普通用法，默认 `seq_cst` 往往是最容易推理的选择。它要求全部 seq_cst 操作能纳入同一个顺序。

但这**不是**：

- 自动把几个不同变量的更新变成事务；
- 自动让普通 `unordered_map` 线程安全；
- 保证跨线程按墙上时钟发生的顺序观察所有事情；
- “每次都清空所有 CPU Cache / 刷 DRAM”。

如果你还不明确证明 relaxed / acquire-release 足够，就先用默认 seq_cst，压测后再优化，而不是一上来给所有地方加 relaxed。

## 5. atomic_flag 与 TTAS 自旋锁：和你的数据库锁对应起来

你之前做过 TTAS（Test-and-Test-and-Set）锁。为什么要“先普通读取，再尝试写锁”？

因为多个核心反复执行会修改缓存行所有权的 RMW，会造成更多缓存一致性流量。TTAS 在锁被占用时尽量只读，只有看起来空闲才执行 RMW。

C++20 起 `atomic_flag` 支持 `test()`：

~~~cpp
#include <atomic>

class TtasLock {
public:
    void lock() noexcept {
        for (;;) {
            // 已经加锁时先只读，减少失败 RMW
            while (flag_.test(std::memory_order_relaxed)) {
                // 真正工程实现可考虑 pause/backoff
            }

            if (!flag_.test_and_set(std::memory_order_acquire))
                return; // 抢到锁
        }
    }

    void unlock() noexcept {
        flag_.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};
~~~

- `test_and_set()`：原子设置为 true，并返回修改前的旧值。
- 旧值 false：你是拿到锁的线程；旧值 true：别人已经拿到。
- `clear(release)`：释放锁，并发布临界区里的修改。
- 后继线程成功的 `test_and_set(acquire)`：获得锁并接收这些修改。
- 仅用于忙等观察的 `test(relaxed)`：不承担锁的同步职责；真正拿到锁时再 acquire。

**这把锁适合学习，不推荐直接替换 tina 的线程池 Mutex**：它可能不公平，线程持锁时间长或被操作系统抢占时会让等待线程白白烧 CPU。需要阻塞等待、调度与公平性时，pthread Mutex 往往更合适。

## 6. atomic::wait / notify：是不是 Futex？

C++20 支持等待一个原子值**不再等于指定旧值**：

~~~cpp
std::atomic<bool> ready{false};

void consumer() {
    ready.wait(false, std::memory_order_acquire);
    // 返回时 ready 已不再是 false
}

void producer() {
    ready.store(true, std::memory_order_release);
    ready.notify_one();
}
~~~

注意：`wait(false)` 的含义不是“等待 false”，而是“**只要当前还是 false，就等待它变化**”。

相比 `while (!ready.load()) {}`，这种写法可以避免持续忙等。Linux 上标准库可能借助 Futex 实现阻塞与唤醒；**标准不要求一定调用 futex**，因此不能等同于直接使用 Linux 系统调用。

限制：等待的是**一个原子对象的值变化**；不能自动替代“队列是否为空 + 多个 Worker + shutdown”这种由多变量组成的条件。它也存在 ABA 限制：值短暂变化又变回旧值，等待者不一定观察到中间过程。实现层会处理虚假唤醒，但调用方仍要考虑协议状态。

## 7. 其他值得认识但暂时不必深入的设施

- **`std::atomic_ref<T>`（C++20）**：把符合条件的现有普通对象作为原子对象访问；需要注意对齐、生命周期，以及相关并发访问必须按其规则使用 atomic_ref，不能同时用普通读写混用。
- **`std::atomic<std::shared_ptr<T>>`（C++20）**：原子地发布 / 更新 shared_ptr 本身；不代表所指向的 T 自动线程安全。
- **`std::atomic_thread_fence`**：内存栅栏，构造与原子操作相关的排序关系。初期先用明确的 acquire/release 原子操作，避免自己拼复杂的 fence 协议。
- **`std::atomic_signal_fence`**：主要限制编译器在同一线程中围绕信号处理相关操作的重排，不是跨 CPU 的硬件同步工具。
- **`volatile`**：在 C++ 中**不是**通用线程同步机制，不能代替 atomic；更不能靠 volatile 修复数据竞争。

## 8. 放回 tina：现在什么该 atomic，什么不该？

对照仓库现有实现：

| tina 中的状态 | 当前方式 | 为什么 |
| --- | --- | --- |
| `net::EventLoop::started_` | `atomic<bool>` 的 `exchange(true)` | 多个调用者竞争“谁第一次运行”时，只让一个成功 |
| `net::EventLoop::stopping_` | 原子读写 + 队列 Mutex + eventfd 唤醒 | 事件循环要看到停止状态；`post()` 与 `stop()` 还要协调任务接受的边界 |
| `runtime::WorkerPool::stopping_` | **普通 bool + pthread Mutex** | 所有访问都在同一个锁的保护下；不需要为了“多线程”改成 atomic |
| `net::EventLoop::tasks_` | `deque` + `mutex` | 原子化一个队列指针不能保证复杂容器所有内部操作线程安全 |
| `service::KvService` 的 HashMap | 当前通过 KV Mutex 保护 | SET/GET/DEL 是复合操作，不是给 map 加个 atomic 就能并发 |
| 新增的连接 ID / 简单统计值 | 可考虑 `fetch_add(relaxed)` | 只需要唯一计数，不需要发布其他对象 |

尤其注意，**`stopping_.load()` 读到 true 不会唤醒卡在 `epoll_wait(-1)` 的线程**。这也是 tina 中 `stop()` 还需要 `eventfd` 的原因：atomic 负责状态，eventfd 负责通知内核等待者。

另一个常见误区：`EventLoop::post()` 中的队列 Mutex 不只是保护 deque 不出错，它的 unlock / lock 也建立了任务对象从提交线程到执行线程的同步。因此这条路径**不需要为了每个 Task 再造一个原子 ready 标志**。

## 9. 性能直觉：怎样才能真的更快？

1. **争用热点决定成本**。一个被上百线程频繁 `fetch_add` 的全局计数器，可能比每线程局部计数再合并慢得多。
2. **伪共享（False Sharing）**：两个互不相关的 atomic 落在同一缓存行，不同核心修改时也可能反复搬运这一行。必要时可考虑按缓存行分离数据（例如 `alignas(64)` 仅作常见平台示意，实际缓存行大小要依据目标平台）。
3. **memory_order_relaxed 不是万能加速开关**。在 x86 上，许多 relaxed 和 acquire/release 的单纯 load/store 可能生成相同的机器指令；真正瓶颈经常在缓存行争用、队列竞争和唤醒。
4. **原子 ≠ 阻塞互斥锁**。原子操作可以是实现 Mutex 的构件，Mutex 还要处理等待、唤醒、持有者调度与临界区范围等问题。
5. **做 benchmark 前先保证程序没有 data race**。未定义行为下的压测结果没有工程意义。

## 10. 自测：三道贴近 Server 的题

**Q1**：两个 Worker 都执行 `counter.store(counter.load() + 1)`，而 counter 是 atomic，会不会丢更新？

**会。** load 与 store 各自原子，但组合不是一次原子 RMW。要用 `fetch_add(1)` 或 CAS 循环。

**Q2**：EventLoop 的普通 `deque<Task>` 已经使用 Mutex 保护了，Worker 提交任务后为什么还要 write(eventfd)？

**Mutex 保证内存状态和跨线程同步，不负责唤醒阻塞在 epoll 的线程**。eventfd 让 epoll 能观察到“有新任务”。

**Q3**：把 `KvService` 的一个 `bool ready` 改成 atomic，是否就可以多个 Worker 无锁访问整个 `unordered_map`？

**不能。** ready 是一个变量；HashMap 的桶、节点、扩容、迭代器、对象生命周期是另一套复合共享状态。必须另外设计同步策略。

---

## 11. 记忆顺序与参考

建议按这条学习线推进：

**data race → load/store/exchange/fetch_add → CAS → relaxed/acquire/release → TTAS → wait/notify → 性能争用 → 无锁内存回收（以后再学）。**

主要参考（有英文，正文已给出中文解释）：

- [cppreference：std::atomic](https://en.cppreference.com/w/cpp/atomic/atomic)
- [cppreference：std::memory_order](https://en.cppreference.com/w/cpp/atomic/memory_order)
- [cppreference：std::atomic_flag](https://en.cppreference.com/w/cpp/atomic/atomic_flag)
- [Linux man-pages：futex(2)](https://man7.org/linux/man-pages/man2/futex.2.html)

**一句话总结**：atomic 帮你安全地在多个 CPU 执行流之间操作少量共享状态；它不自动解决队列、连接、HashMap 的复合一致性，也不替代 Linux 的等待与唤醒机制。
