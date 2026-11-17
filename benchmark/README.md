# Tina benchmark

启动服务端后运行新的 Remote IDE 混合负载（客户端和服务端需在同一台机器、使用相同 HOME）：

```sh
cmake --build build-release --target tina_bench -j 4
./tina server 3490
# 另一个终端：
./tina_bench --workload ide_mixed --port 3490 --connections 4 --inflight 64
```

默认扫描 `~/projects/linux`，**10 秒暖机 + 50 秒测量**，报表写到
`benchmark/result_ide_mixed.txt`。准备路径池、预填充热缓存、打开终端和最后排空/清理不计时。
`--duration` 沿用原有语义，表示包含暖机的总时间；等价参数为 `--warmup 10 --duration 60`。
旧 workload 和默认 `ping` 行为保持原样。

| 操作 | 提交占比 | 内容 |
| --- | ---: | --- |
| File read | 45% | 从 Linux 源码热集和超出 Tina 单文件缓存上限的文件读取 |
| Dir read | 25% | Linux 目录与已订阅 inotify 的临时目录 |
| File write | 15% | 每连接独占一个 4 KiB 文件，使用最新 revision 保存 |
| Terminal | 15% | 真实 PTY 命令，一半 `wc -c` 读源码，一半交替 `touch` / `rm` 临时目录中的标记文件 |

每条连接按固定的 20 操作周期提交，连接之间并发。每连接最多一个未完成的写入和一条未完成的终端命令；
遇到这两类依赖就暂停补充流水线，保留配比，不用额外读取填满空槽。`inflight` 是上限，实际并发可能较低。
测量窗口边缘的完成占比会略有偏差。

文件读取默认 70% 选择热集、30% 选择不可缓存文件，可用 `--fs-hot-percent 0..100` 调整。
热集从源码抽取最多 64 个非空、最多 256 KiB 的文件，准备阶段预先读取；整个热集最多 16 MiB。
冷集选择严格大于当前 `WorkspaceCacheConfig::max_per_file_bytes`（目前 2 MiB）的源码文件，
轮换偏移，保证走 `open/fstat/pread` 路径。缺少对应文件时明确报错，不静默退化成热读。
默认每次读取最多 64 KiB，`--read-bytes` 可调整到协议上限。

这里的“冷”指 **不经过 Tina 文件内容缓存**。不执行 `drop_caches`，不驱逐源码的 Linux 页缓存；
`pread` 仍可能命中页缓存，因此不能把冷读数当作 SSD I/O 次数。报表记录的是目标冷热配比，
不是服务端实际缓存命中率。服务端若被修改为另一套缓存配置，需要同步重编译 benchmark。

所有写入和删除都限定在 `~/projects/.tina-bench-XXXXXX/` 独占临时工作区内，Linux 源码只读，
不会执行 `git reset/clean`，也不会覆盖原有未提交内容。可用 `--fs-scratch-root tina` 将临时工作区放在
`~/projects/tina/` 下；必须在源码树之外。报告也禁止写入源码树内。
普通退出、异常、SIGINT 和 SIGTERM 都会清理临时目录；强制 SIGKILL/断电无法执行清理，但留下的也只有独立临时目录，
不会修改源码内容。读取可能更新文件系统访问时间，不承诺保留 atime。

终端命令收到 RPC ACK 后仍占用一个在途位置，直到收到退出码标记和 terminal_state 完成事件。
`touch/rm` 还必须收到含有对应目录条目状态的 inotify 推送才算成功，因此终端延迟包含实际执行时间与变更通知。
结束后排空所有在途操作、校验 RPC 和本地文件内容、关闭终端，再清理目录；排空期间完成的操作不计入 QPS/延迟。
请求超时、命令失败、版本冲突、校验失败都令测试失败，不输出成功报告。

报表保留原有五节，额外追加混合负载的操作计数、目录事件数、变更确认数和清理结果。
目录事件不算 RPC 完成量，但其字节数计入 RX；终端输出/完成事件计入对应命令的接收字节数。
总 QPS 混合了文件 RPC 和端到端终端操作，不能直接与只统计 RPC ACK 的压测比较。

短回归（包含源码保护、并发、冷热边界、信号中断清理）：

```sh
python3 test/benchmark_mixed.py ./tina ./tina_bench
# 无法创建 TCP socket 的受限环境，只跑参数校验和失败清理：
python3 test/benchmark_mixed.py ./tina ./tina_bench --offline
```
