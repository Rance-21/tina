export module runtime.config;
import std;
/*
8 (P-core) * 2 + 16(E-core) logical cpus

支持 128 位（16 字节）原子比较并交换指令。
在无锁队列/无锁栈中，可以把“64 位指针 + 64 位版本号标签”打包成 128 位结构体做单指令原子 CAS，
从硬件层面根治指针复用的 ABA 问题

waitpkg（UMONITOR / UMWAIT / TPAUSE）：
现代 Intel 引入的用户态低功耗等待指令。当必须做极短自旋等待某个内存地址变化时，
可以让核心进入轻量级浅睡眠节能状态，避免死循环把超线程兄弟核的执行单元彻底堵死

修复了 Meltdown 漏洞

$$\text{容量} = \text{Sets} \times \text{Ways} \times \text{Line Size} =
 64 \times 12 \times 64\text{ B} = 49152\text{ B} = 48\text{ KiB}$$

 单路处理器，只有一个本地内存节点（NUMA Node 0），
 所有 32 个逻辑核心访问这 15691 MB（16 GB）内存的物理延迟完全一致

 每一对相邻的偶数与奇数逻辑核（0 和 1、2 和 3……一直到 14 和 15）都拥有完全相同的物理 CORE
编号以及完全相同的 L1d:L1i:L2 缓存编号 （例如 CPU 0 和 CPU 1 都是
0:0:0:0）。它们在硅片上是同一颗核心，绑核时如果把两个重负载线程同时绑在 0 和 1 上，
 会互相争抢同一套执行单元和 L1/L2 缓存；只绑偶数号（0, 2, 4, 6, 8, 10, 12,
14）才能让每个线程独享整颗大核

 CPU 16 ~ 31
独立物理结构： 对应物理 CORE 8 ~ 23（共 16 颗独立物理小核）。E 核不支持超线程，因此 1 个逻辑 CPU
编号严格对应 1 颗独立的物理核心。 频率与算力定位： MAXMHZ 统一为 4100 MHz

绝大多数 P 核的 MAXMHZ 为 5600 MHz，唯独 CPU 8 ~ 11（对应物理 CORE 4 和 CORE 5）最高睿频达到 5800
MHz

4 个 E-core 组成一个集群，共享 L2 Cache。而同一 P-core 上的两个逻辑 CPU 则共享更多核心内部资源
*/
export namespace runtime {
struct ServerConfig {
    // 每个 I/O 线程维护自己的 EventLoop。
    std::size_t io_threads{2};
    std::size_t worker_threads{2};

    // WorkerPool 中最多允许排队的任务数。
    std::size_t max_pending_tasks{4096};

    // 验证合法性
    void validate() const {
        if (io_threads == 0) {
            throw std::invalid_argument("io_threads must be > 0");
        }

        if (worker_threads == 0) {
            throw std::invalid_argument("worker_threads must be > 0");
        }

        if (max_pending_tasks == 0) {
            throw std::invalid_argument("max_pending_tasks must be > 0");
        }
    }
};

} // namespace runtime
