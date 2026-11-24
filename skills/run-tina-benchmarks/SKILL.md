---
name: run-tina-benchmarks
description: 构建 Tina 的 Clang RelWithDebInfo 版本，用 tina_bench 实测 ide_mixed 和 fs_mixed。在 Tina 项目中跑混合负载、检查重构后的 QPS 回退或做前后对照时使用。
---

# Tina 混合压测

从 Tina 仓库根目录运行本技能的 `scripts/run.sh`。例如，技能源码随项目保存时执行：

```bash
bash skills/run-tina-benchmarks/scripts/run.sh
```

脚本用 `/usr/bin/clang++`、Ninja、`RelWithDebInfo` 配置 `build-release`，构建 `tina` 与 `tina_bench`。两个程序的输出位置都是源码根目录。刚用另一个构建目录生成过这两个程序时，运行脚本前先用 `cmake --build build-release --target clean` 清除旧产物，确保本轮链接正确版本。

两组测试依次执行，各自启动新的服务端，使用同一份 `~/projects/linux`、4 个连接和每连接 64 个在途请求。客户端沿用项目压测脚本的 CPU 集合 `6,10,12,14,24-31`。服务端沿用默认绑核。需要复现另一次运行时，可通过 `TINA_BENCH_PORT`、`TINA_CLIENT_CPUS`、`TINA_FS_ROOT` 和 `TINA_FS_SCRATCH_ROOT` 指定对应值。最后一项是 IDE 可写临时目录的父目录，默认位于 `~/projects`，须在被测源码树外。

| 负载 | 预热 | 正式测量 | 单次读上限 |
| --- | ---: | ---: | ---: |
| ide_mixed | 10 秒 | 50 秒 | 65536 字节 |
| fs_mixed | 5 秒 | 25 秒 | 4096 字节 |

每轮报告、运行参数与服务端日志保存在脚本打印的独立目录里。比较代码版本时，在同一台机器、相同数据集和绑核下交替运行两个版本。核对预热和正式测量的错误数、成功率及 IDE 临时目录清理结果，再汇报平均 QPS、P99、RX 吞吐和各操作延迟。若本地 TCP 创建失败，保留错误并注明本轮缺少 TCP 结果。
