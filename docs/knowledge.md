- **MTU（Maximum Transmission Unit，最大传输单元）**：数据链路层单个帧所能承载的最大网络层（IP 包）字节数。标准以太网 MTU 为 **1500 字节**（本地回环网卡 `lo` 的 MTU 通常为 65536 字节）。
- **MSS（Maximum Segment Size，最大报文段长度）**：TCP 单个报文段所能携带的最大**应用层数据**长度（不含 TCP 头和 IP 头）：

[链路层 Ethernet] ┌─────────────┬─────────────┬─────────────┬────────────────────────┬──────────┐
                    │以太网头 14B │ IP 头 20B+  │ TCP 头 20B+ │     RPC Frame (N B)    │ FCS 4B   │
                    └─────────────┴─────────────┴─────────────┴────────────────────────┴──────────┘
                                  ├─────────────────── MTU (标准以太网 1500 Bytes) ────┤
                                                              ├────── MSS (1460B) ────►│

-------------------------------------
Nagle 算法：如果当前有未确认的已发送报文（in-flight），且本次准备发送的数据小于一个 MSS，Nagle 算法会强制推迟发送，将后续小包合并为一个大包发出，以减少网络上的微小数据包（可用 TCP_NODELAY 关闭）。

-------------------------------------
TSO/GSO 判断：如果网卡支持硬件分段，TCP 会直接构造一个包含几十 KB 数据的“巨型 skb”，并打上 GSO 标记，直接交给下层。

-------------------------------------
NAPI 机制
在高流量冲击下，网卡硬中断只触发一次，随后 CPU 核心完全进入软中断（ksoftirqd）轮询状态，持续不断地将网卡 Ring Buffer 中的数据捞出来。只有当队列被彻底清空时，系统才会重新开启硬件中断。这样就把“海量不可控的硬件中断”转换成了“内核按配额受控处理的轮询循环”。

[数据到达]
   │
网卡把数据 DMA 写入内存 Ring Buffer
   │
触发硬件中断 (Hard IRQ)
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ 硬中断处理函数（只做两件事，耗时微秒级）：                    │
│ 1. 关中断：向网卡寄存器写指令，关闭该网卡的硬件接收中断         │
│ 2. 摇人：把网卡实例挂进 CPU 的轮询列表，触发软中断            │
└────────────────────────────────────────────────────────┘
   │
   ▼
[退出硬中断，CPU 恢复正常调度]
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ 内核软中断线程（ksoftirqd 执行 net_rx_action 轮询）：       │
│ 1. 调用网卡驱动注册的 poll() 函数                          │
│ 2. 按照预算配额（默认 budget = 64），一口气捞一批包          │
│ 3. 将这批包递交给上层协议栈（IP/TCP 处理）                   │
└────────────────────────────────────────────────────────┘
   │
   ├─► 如果队列里还有包：让出 CPU 片刻，下一轮软中断继续捞
   │
   └─► 如果 Ring Buffer 已经空了（收光了）：
         1. 将网卡移出轮询列表
         2. 重新开启网卡的硬件中断（等待下一次有包时唤醒）

-------------------------------------
**端口号（16 位无符号整数，`0 ~ 65535`）**：
IP 地址只负责把数据包路由到目标主机，而**端口号**决定了内核收到数据包后应该把它交给这台机器上的**哪一个进程 / Socket**：
   - `0 ~ 1023`：**知名端口（Well-Known Ports）**，绑定需要 `root` 权限或 `CAP_NET_BIND_SERVICE` 能力（如 `22` SSH、`80` HTTP、`443` HTTPS）。
   - `1024 ~ 49151`：**注册端口（Registered Ports）**，普通用户进程可直接绑定（`tina` 默认使用 `3490` 端口）。
   - `32768 ~ 60999`：**临时端口 / 动态端口（Ephemeral Ports）**，范围由 `/proc/sys/net/ipv4/ip_local_port_range` 控制。客户端调用 `connect()` 前无需手动 `bind()`，内核会自动从该范围内挑选一个空闲端口作为客户端源端口。

在 Linux 内核网络栈中，“端口”并没有一个专门叫 struct port 的结构体实体。对于内核而言，端口只是一个用来查找 Socket（套接字）的哈希键（Hash Key）

-------------------------------------
DMA（Direct Memory Access，直接内存访问）
网卡收到数据 ──(网卡控制器自主走 PCIe 总线)──> 直接写进主机物理内存
搬运完成后，网卡发个中断通知 CPU

预借场地：操作系统在内存里开辟一个环形数组（RX Ring Buffer），每个槽位填上一个可用的物理内存地址（由内核提前分配好的空 sk_buff）。

硬件搬运：网线上的电/光信号被网卡芯片转化为数字信号，网卡控制器直接掌控 PCIe 总线，根据 Ring Buffer 里的物理地址，悄无声息地把数据塞进主机的物理内存。

-------------------------------------
零拷贝技术的演化梯队
为了消除这两次耗费 CPU 的内存拷贝，Linux 发展出多代机制：

① mmap() + write()：消除读侧 CPU 拷贝
buf = mmap(NULL, len, PROT_READ, MAP_SHARED, file_fd, 0);
write(socket_fd, buf, len);

原理：mmap() 直接将内核 Page Cache 的物理页通过页表映射到了用户虚拟地址空间。用户进程直接读写这片内存，省去了 copy_to_user。
效果：4 次上下文切换，3 次拷贝（2 次 DMA + 1 次 CPU）。
缺陷：如果写入时有其他进程截断（truncate）该文件，访问越界虚拟内存会触发 SIGBUS 信号导致进程异常崩溃；且依然保留了向 Socket Buffer 的 CPU 拷贝。

② sendfile() + 硬件 Scatter-Gather DMA：真正的“传输零拷贝”
sendfile(socket_fd, file_fd, &offset, len);

[ 用户空间 ]        (无需介入数据流，仅 1 次系统调用，2 次上下文切换)
───────────────────────────────────────────────────────────────────────
[ 内核空间 ]        Page Cache ───────────── (完全无 CPU 数据拷贝) ─────────┐
                         │                                              │
                         │ 仅拷贝内存地址与长度 (几十字节元数据)               │
                         ▼                                              ▼
                    Socket Buffer (skb 挂载 page 指针)             Direct DMA
                                                                        │
[ 硬件设备 ]                                                          [ 网卡 ]

实现原理：
1,系统调用进入内核，内核从 Page Cache 定位到文件的物理页（Page）。
2,内核分配 sk_buff，但完全不拷贝数据，只在 skb_shared_info->frags 数组中填入这批 Page 的物理内存指针、偏移量和长度。
3,网卡开启 DMA 时，由于支持 Scatter-Gather，网卡芯片自己根据描述符里的多个物理地址，从内存中不连续的 Page Cache 页面逐个拉取数据，硬件内部拼成 TCP 数据帧发出去。

效果：2 次上下文切换，2 次 DMA 拷贝，0 次 CPU 拷贝。

③ splice()：面向任意流的内核管道拼接
sendfile 只能用于文件到网卡，无法用于两套 Socket 之间的代理转发（如反向代理服务器 Nginx、Envoy）。splice() 填补了这一空白：
splice(fd_in, off_in, pfd[1], NULL, len, SPLICE_F_MOVE);
splice(pfd[0], NULL, fd_out, off_out, len, SPLICE_F_MOVE);

原理：基于 Linux 管道缓冲区（struct pipe_inode_info）。数据在流经内核时，并不复制内部字节，而是在管道环形缓冲队列中仅仅传递页面指针引用（struct pipe_buffer）。
效果：两端都是流设备时，数据完全在内核空间以“页引用传递”的方式流动，全程零 CPU 拷贝。

④ MSG_ZEROCOPY：纯用户态 Buffer 的零拷贝（Linux 4.14+）
前面介绍的 sendfile 强依赖内核的 Page Cache。如果数据是应用层动态生成的（例如在内存中实时序列化的 Protobuf 或加密后的数据块），如何实现零拷贝？

Linux 提供了 send(fd, buf, len, MSG_ZEROCOPY)：

1. 用户线程: send(..., MSG_ZEROCOPY)
      │
2. 内核协议栈: 调用 pin_user_pages() 钉住这批用户内存物理页
      │ 将页面指针直接填入 skb，网卡直接 DMA 读取用户虚拟地址映射的物理页
      ▼
3. 数据发出，网卡触发中断释放 DMA 映射
      │
4. 内核向 Socket 错误队列 (Error Queue) 压入一条释放通知
      ▼
5. 用户线程: 通过 epoll 捕获 POLLERR，调用 recvmsg(..., MSG_ERRQUEUE)
      │ 确认这块内存已经安全发完，此时才能复用或释放这片内存 Buffer

关键约束：
必须异步回收：在收到内核的 Error Queue 通知前，用户代码绝对不能修改这块 Buffer，否则发送出去的数据会被污染。
临界点效应（Break-even point）：pin_user_pages()、TLB 刷新以及 Error Queue 异步通知的开销并不小。
通常只有发送块大小大于 10KB ~ 16KB 时，MSG_ZEROCOPY 的收益才会超过单纯的 copy_from_user。

⑤ 终极形态：内核旁路与内存池（AF_XDP / DPDK）
当追求数千万 PPS（Packets Per Second）的极限网络吞吐时，
上述所有方案依旧存在系统调用和内核网络栈遍历的固有损耗。
现代方案如 AF_XDP（XDP Sockets） 彻底重构了内存模型：

UMEM（共享内存区）：在用户态预先申请一大块连续物理内存作为 Packet Pool，通过 mmap() 与内核共享。
零拷贝轮询：网卡收到数据包后，直接通过 DMA 写入 UMEM 的指定槽位。内核通过无锁环形队列（Ring Buffer：Fill Ring, Rx Ring, Tx Ring, Completion Ring）仅向用户空间传递数据块的偏移下标（Descriptor）。

结果：不仅 CPU 拷贝次数为 0，连系统调用、sk_buff 分配和内核协议栈全部被绕过，达成了真正的硬件级直通。

-----------------------------------
高性能服务端（如 Nginx、Envoy、Redis）极力推崇用 signalfd 或掩码线程来“规避 EINTR”，根本目的从来不是为了省下那几百纳秒的 CPU 切换开销，而是为了彻底解决系统设计中的“正确性灾难”与“控制流撕裂”。
这背后有四个在系统级编程中极其致命的原因：

1. 非阻塞 I/O 下，read() 压根不等网线
如果用的是多线程阻塞 I/O，等待网络包确实要几毫秒。但在基于 epoll 的高性能 Reactor 架构中，所有套接字都是 O_NONBLOCK：
当 epoll_wait() 唤醒通知可读时，说明网卡早已通过 DMA 把数据塞进了内核的 Socket 接收缓冲区。
此时调用的 read() 本质上只是做一次极快的内存拷贝（从内核内存复制到用户态内存），耗时通常在 数十纳秒到几微秒 之间，根本不存在“等待网络传输”的过程。
在这个时间尺度上，一次异步信号的介入、内核修改用户栈帧并执行 Handler、再退出并返回 EINTR 的开销，已经与这次内存拷贝本身的耗时处于同一数量级了。

2. 真正的噩梦：异步信号安全（Async-Signal-Safety）
这是规避传统信号机制最核心的原因。
当一个信号到达时，内核会强行暂停线程当前的指令流，在当前线程的用户栈上强行塞入一个栈帧去执行信号处理函数。这意味着信号处理函数可以在任意一条汇编指令之间被插队执行。
这直接导致了灾难性的死锁与内存破坏风险：
不能使用堆内存：C/C++ 的 malloc()、free()、new 底层都有全局锁或桶锁。如果线程在执行 malloc() 时刚好拿到了锁，瞬间被信号打断，而信号处理函数内部恰好记录了一条日志或实例化了一个对象（触发了 malloc），该线程会在同一把锁上自己把自己永久死锁。
不能使用常规锁：任何互斥锁（std::mutex、pthread_mutex_t）在信号函数里都不可安全获取。
标准库近乎全废：POSIX 标准中明确标注为“异步信号安全（Async-Signal-Safe）”的函数只有区区几十个底层系统调用（如 write、sigaction）。你平时写的几乎所有 C++ 逻辑、日志框架、业务类，都绝对不允许在传统信号处理函数中调用。

如果用 signalfd 把信号转为普通文件事件，你就必须在极其狭窄的安全边界内写出蹩脚的代码（通常只能在 Handler 里置一个 sig_atomic_t 标志位），甚至可能因某个库函数内部用了非安全操作而引发难以复现的线上崩溃。

3. 控制流撕裂与防御性代码爆炸
如果在系统中放任 EINTR 的发生，意味着你的代码中每一个可能阻塞或与内核交互的地方都变成了一个潜在的异常退出点。
为了保证健壮性，每个调用都必须被包上一层循环：

C++
// 每一个系统调用都要防一手 EINTR
ssize_t n;
while ((n = read(fd, buf, len)) < 0 && errno == EINTR);
while ((n = write(fd, buf, len)) < 0 && errno == EINTR);
while ((nready = epoll_wait(epfd, events, MAX, -1)) < 0 && errno == EINTR);

更恶心的是边缘情况的竞态：
很多调用在处理部分数据后被打断，返回值并不是 -1 配合 EINTR，而是返回了已经处理的部分字节数（Partial Read/Write），上层的状态机逻辑需要同时兼顾短读/短写与打断重试。
只要团队中有一个人漏写了一处 errno == EINTR 的重试检查，当运维执行热重载（发送 SIGHUP）或监控拉取（发送 SIGUSR1）时，就会导致该网络连接因为一次无害的信号触发意外断开。
还有前文提到的 close(fd)：在 Linux 上被打断绝对不能重试，否则在多线程竞争下会把别的新连接误关。
为了应对一个在正常运行时极低概率发生的信号，让 100% 的网络路径代码都背负上复杂的防御性分支和出错风险，在软件工程上是非常廉价且容易翻车的做法。

主线程启动前使用 sigprocmask() / pthread_sigmask() 屏蔽感兴趣的所有信号（如 SIGINT（signal interrupt，如ctrl + C）、SIGTERM（））。
调用 signalfd() 创建对应的 sfd，sfd会捕获这些信号
把 sfd 挂载到主事件循环的 epoll 中监听 EPOLLIN。
当操作系统产生信号时，内核直接将信号转化为字节流写入 sfd，直接唤醒 epoll_wait。

1. BSD 文件锁：flock(2)
源于 BSD Unix，属于最古老、最直观的锁。

C
#include <sys/file.h>
int flock(int fd, int operation);
操作模式：

LOCK_SH：共享锁（读锁）

LOCK_EX：互斥/排他锁（写锁）

LOCK_UN：解锁

LOCK_NB：非阻塞标志（与上述按位或，若冲突不休眠，直接返回 -1 置 errno = EWOULDBLOCK）

锁定粒度：全文件锁。无法指定对文件中的某一段字节范围加锁。

内核绑定关系：绑定在 struct file 上：

fork() 继承：父进程加了 flock，fork() 出子进程，子进程共享同一个 struct file，因此子进程也持有该锁。

dup() 共享：通过 dup()、dup2() 复制出来的 fd，调用 flock(fd2, LOCK_UN) 会直接把 fd1 上的锁解掉。

多进程/多 open() 隔离：如果同一个进程对同一路径 open() 两次得到 fd1 和 fd2，在 fd1 上加独占锁后，尝试在 fd2 上加锁会被自身阻塞（因为它们是两个独立的 struct file）。

2. POSIX 记录锁：fcntl(2)（F_SETLK）
为满足数据库、并发更新等需要锁定“文件某一条记录/某段偏移量”的需求，POSIX 制定了字节范围锁规范。

C
#include <fcntl.h>
int fcntl(int fd, int cmd, struct flock *lock);
// cmd: F_SETLK (非阻塞加锁/解锁), F_SETLKW (阻塞加锁), F_GETLK (查询锁冲突)
锁定粒度：字节范围锁（Range Lock）。通过 struct flock 指定：

C
struct flock fl;
fl.l_type   = F_WRLCK;    // F_RDLCK (读), F_WRLCK (写), F_UNLCK (解锁)
fl.l_whence = SEEK_SET;   // SEEK_SET, SEEK_CUR, SEEK_END
fl.l_start  = 0;          // 起始偏移量
fl.l_len    = 1024;       // 锁定长度 (传 0 代表直到文件末尾 EOF，无论文件后续如何扩容)
内核绑定关系：绑定在 (inode, PID) 二元组上。
这个设计是 UNIX 历史上最受诟病的设计之一，直接带来了两个灾难性的陷阱：

陷阱 A：任何一个 close() 都会引爆全盘解锁
只要当前进程关闭了指向该文件的“任何一个”描述符，该进程持有的该文件的所有 POSIX 锁全部会被内核强制清除！

C
int fd1 = open("data.db", O_RDWR);
fcntl(fd1, F_SETLK, &fl); // 成功加上了锁！

int fd2 = open("data.db", O_RDWR); // 比如某个基础库、日志组件无意间也打开了同一个文件
close(fd2); 

// 灾难发生：调用 close(fd2) 的瞬间，fd1 上持有的锁被内核全部静默释放！
// 此时外部其他进程可以肆无忌惮地修改 data.db，引发数据损坏！
陷阱 B：fork() 完全不继承
子进程不会继承父进程持有的 POSIX 记录锁。如果子进程也去访问文件，甚至可以和父进程互相冲突加锁。

陷阱 C：同一进程内的多线程无法互斥
由于锁绑定在 PID 上，同一个进程内的多个线程持有的是同一个 PID。线程 A 加了写锁，线程 B 依然可以成功加上写锁，POSIX 记录锁在同进程多线程间完全无法起到互斥保护作用。

注：关于 lockf(3)：
C 标准库提供的 lockf(fd, cmd, len) 底层在 Linux 上纯粹是对 fcntl(F_SETLK) 的一层薄封装，它完全具备 POSIX 锁的所有陷阱。

查看所有网卡
ip link show
查看网卡及其 IP：
ip addr show


只查看 IPv6：
ip -6 addr show


查看 Wi-Fi 的全局 IPv6
ip -6 addr show dev wlp18s0 scope global

ip -6 addr show

查看 IPv6 路由表
ip -6 route

查看 Tina 域名解析
dig AAAA tina.rance-21.com +short

查看 DNS 详细信息：
dig AAAA tina.rance-21.com

查看定时器状态
systemctl status tina-ddns.timer
正常应该看到：
Active: active (waiting)

查看下次执行时间
systemctl list-timers --all | grep tina-ddns
查看服务状态
systemctl status tina-ddns.service

手动触发一次 DDNS
sudo systemctl start tina-ddns.service
journalctl -u tina-ddns.service -n 10 --no-pager

监听流量
sudo tcpdump -ni wlp18s0 -nn 'ip6 and tcp port 3490'

systemd-inhibit --what=idle:sleep:handle-lid-switch --who="MyServer" --why="Running backend server" ./tina
