# Tina 远程开发工作台：总体架构

**阶段：** Phase 0（架构设计）  
**版本：** 0.1  
**状态：** 设计基线，尚未进入业务实现

## 1. 项目定位

Tina 是一个运行在 Fedora Linux 主机上的远程开发工作台，由 Android 设备提供操作界面。

它不是完整复刻 VS Code，而是让用户在 Android 上远程完成常见开发操作：

- 浏览 Fedora 上的 Git 项目和目录树。
- 查看、编辑并保存远端文件。
- 使用 Fedora Shell 终端。
- 查看 Git 状态和代码差异。
- 启动、管理并使用远端 AI CLI Agent，例如 Codex 和 Antigravity。
- 网络断开后重新连接，并尽可能恢复已有会话。

**核心原则：Fedora 负责执行，Android 负责交互。**

源代码、编译器、Git、终端进程和 Agent 都运行在 Fedora 上。Android 通过协议向 Server 发送请求，并接收文件内容、终端输出和 Agent 事件。

## 2. 设计原则与明确边界

### 2.1 技术方向

- Server：Fedora Linux + C++23。
- Client：Android + Tauri 2。
- Client 原生层：Rust，负责连接、协议和与 WebView 的桥接。
- Client 界面：WebView，负责工作区、编辑器、终端、Git 和 Agent 的展示。
- 通信：基于现有 TCP/RPC 能力扩展，支持请求、响应和服务端事件。
- 元数据：SQLite。
- 项目文件：Linux 原生文件系统。
- 缓存：有容量上限的内存缓存。

### 2.2 不做什么

- 不做多机分布式系统，不引入 Raft、分布式事务或服务发现。
- 不自研数据库，SQLite 只承担适合它的元数据持久化工作。
- 不自研文本编辑器和终端模拟器，接入成熟组件。
- 不在 Tina 中实现 AI 模型推理；Agent 作为 Fedora 上的外部 CLI 进程运行。
- 不为了“看起来高级”提前引入复杂线程模型、无锁结构或自定义内存分配器。

### 2.3 性能优先级

1. **Performance：** 网络运行时、事件循环、并发、内存分配、数据复制、背压和端到端延迟。
2. **Storage：** 文件访问、元数据持久化、缓存命中率、恢复能力。
3. **产品功能与界面：** 在不破坏前两项的前提下逐步完善。

性能优化必须由基准测试和 Profiling 结果驱动。KV 服务继续保留，主要作为可控的 RPC/并发 workload 和性能测试对象，不再作为产品的核心功能。

## 3. 总体结构

    Android
    ┌─────────────────────────────────────────┐
    │ WebView                                 │
    │ 工作区 │ 文件编辑 │ 终端 │ Git │ Agent  │
    ├─────────────────────────────────────────┤
    │ Tauri Rust Backend                      │
    │ Connection Manager / Protocol Codec    │
    │ Pending Requests / Event Forwarding    │
    └───────────────────┬─────────────────────┘
                        │ TCP
                        │ Tina Protocol
    ┌───────────────────▼─────────────────────┐
    │ Fedora：C++23 Tina Server               │
    │                                         │
    │ Socket / Epoll / EventLoop              │
    │              │                          │
    │         Frame Decoder                    │
    │              │                          │
    │          RPC Router                      │
    │     ┌────────┼────────┬────────┐         │
    │     ▼        ▼        ▼        ▼         │
    │ Workspace Terminal   Git     Agent      │
    │     │        │        │        │         │
    │     └────────┴────────┴────────┘         │
    │              │                          │
    │       WorkerPool / Process Manager      │
    │              │                          │
    │ Filesystem / PTY / Git CLI / Agent CLI  │
    │              │                          │
    │         SQLite / Memory Cache            │
    └─────────────────────────────────────────┘

Android 的 WebView 不直接建立 TCP 连接。所有网络请求都经过 Rust Backend，由 Rust 层统一管理连接和请求状态。

Server 不知道界面如何绘制。它只提供稳定的 Workspace、Terminal、Git、Agent 等服务接口，并通过 Event 向客户端推送异步数据。

## 4. 现有 Tina 基础设施的复用

新版 Tina 是现有 C++ Linux Server 项目的功能扩展，不是另起炉灶，也不需要先把整个仓库重构一遍。

| 现有模块 | 在新版 Tina 中的职责 |
|---|---|
| net.socket | TCP Socket 创建、连接和收发 |
| net.epoll | Linux I/O 多路复用 |
| net.event_loop | I/O 事件循环、跨线程任务投递 |
| net.event_loop_thread_pool | I/O EventLoop 线程组 |
| net.buffer | 网络输入/输出缓冲区 |
| runtime.worker_pool | 执行可能阻塞的业务任务 |
| runtime.log | 统一日志 |
| runtime.config | Server 配置 |
| rpc.protocol | 现有 RPC 帧和消息定义的演进基础 |
| rpc.connection | 连接状态和收发管理 |
| rpc.server / rpc.client | 现有 Server/Client 通信基础 |
| src/file/protocol.cppm | 已有分块文件传输协议，可评估后复用 |

具体文件名以仓库当前分支为准。Phase 0 只确定职责边界，不要求立即移动或重命名已有模块。

KV 服务继续保留，作为测试负载；已有文件服务协议作为文件传输能力的基础。不要为了新版工作台删除已有实验成果。

## 5. Server 模块边界

### 5.1 RPC Runtime

只负责通用通信机制：

- TCP 连接的建立和关闭。
- Epoll 事件分发。
- 输入缓冲区和输出缓冲区。
- 帧解析与编码。
- Request / Response / Event 的分发。
- 请求超时、连接状态和输出背压。
- 将业务任务投递到 WorkerPool，并把结果送回所属 EventLoop。

Runtime 不直接操作 Workspace、Git 或 Agent 的业务数据。

### 5.2 RPC Router

Router 根据 Method 将请求分发到对应 Service。

它负责：
- Method 注册和查找。
- 参数格式检查。
- 统一错误状态。
- 将响应交回 RPC Runtime。

新增业务服务时，不应该把越来越多的业务 switch 塞进 Server 主循环。

### 5.3 Workspace Service

负责远端项目和文件访问：

- 列出已登记的 Workspace。
- 打开 Workspace。
- 枚举目录。
- 读取文件或指定范围。
- 写入文件。
- 处理较大文件的分块传输。

项目文件仍然存储在 Linux 文件系统中，SQLite 不保存整份源代码。

路径必须相对于已登记的 Workspace 根目录解析。必须检查路径穿越和符号链接逃逸，不能仅靠字符串拼接判断路径是否合法。

### 5.4 Terminal Service

负责交互式 Shell：

- 创建 PTY（伪终端）会话。
- 启动用户指定的 Shell。
- 转发客户端输入。
- 转发终端输出。
- 调整终端窗口大小。
- 关闭会话并回收进程。

必须使用 PTY，而不是简单地把子进程的 stdin/stdout 管道当作完整终端。交互式 Shell 依赖终端设备语义。

PTY 文件描述符应纳入现有 I/O 事件循环管理，避免每个终端都创建一个阻塞线程。

### 5.5 Git Service

第一版优先调用系统 Git CLI，不重新实现 Git 对象模型。

提供常用能力：
- 查询分支和工作区状态。
- 列出变更文件。
- 获取 diff。

耗时的 Git 操作不能阻塞网络 EventLoop，应通过 WorkerPool 执行，并限制输出大小。

### 5.6 Agent Service

Agent 是由 Server 管理的外部 CLI 进程。

职责：
- 管理 Agent Profile。
- 启动和停止 Agent 进程。
- 为交互式 Agent 分配 PTY。
- 转发输入和输出。
- 维护 Agent 会话状态。
- 支持断线后重新连接到仍然存活的会话。

Agent Profile 至少需要描述名称、可执行文件、启动参数、工作目录和环境变量。不要把某一家 Agent 的命令行参数写死在 RPC Runtime 中。

Agent 进程与普通 Terminal 进程共用底层 Process/PTY 管理能力，但业务生命周期分开管理。

### 5.7 Storage Service

数据归属明确分为三类：

| 数据 | 存储位置 |
|---|---|
| 项目源代码和普通文件 | Linux 文件系统 |
| Workspace、设备、Agent Profile 等元数据 | SQLite |
| 热点目录信息和可复用元数据 | 有界内存缓存 |

SQLite 不替代文件系统。缓存必须有容量上限，并明确失效策略；不能让打开过的文件和目录无限留在内存中。

## 6. 并发与线程模型

第一版沿用现有的 Acceptor + I/O EventLoop 线程组 + WorkerPool 思路。

- Acceptor 负责接受新连接，并将连接分配给 I/O EventLoop。
- 一个 Connection 由一个 EventLoop 负责，避免多个线程同时读写同一连接状态。
- 网络 I/O、PTY I/O 尽量由 EventLoop 驱动。
- 文件系统、Git 等可能阻塞的操作交给有界 WorkerPool。
- Worker 完成后通过任务队列和 EventLoop 唤醒机制返回结果。
- 每条连接的待发送数据必须有上限；超过上限时执行背压，而不是无限积累。

不在 Phase 0 引入新的复杂调度器。后续是否调整线程数量、是否拆分更多 EventLoop，必须通过压测决定。

## 7. 连接与会话生命周期

一次正常连接大致经历：

1. Android Rust Backend 建立 TCP 连接。
2. 双方交换协议版本和能力信息。
3. 完成设备认证（认证能力在产品化阶段完善）。
4. 客户端获取 Workspace 和已有会话信息。
5. 客户端发送请求，Server 返回响应。
6. Server 可以主动推送终端输出、Agent 输出和会话状态等 Event。
7. 断线后，客户端重新建立连接并重新同步状态。

连接断开不应默认杀掉所有 Terminal 和 Agent 进程。会话是否保留由 Server 的会话生命周期策略决定。

不能在连接状态不确定时盲目重放非幂等请求。例如文件写入、启动 Agent、执行 Shell 命令都不能简单地因为超时就自动执行第二次。

## 8. 安全边界

- Server 以普通 Linux 用户身份运行，不使用 root。
- 开发阶段只监听明确配置的局域网地址，不开放公网端口。
- Workspace API 只能访问配置允许的目录。
- 必须防止路径穿越和符号链接逃逸。
- 终端和 Agent 运行在 Server 用户权限下；Workspace API 的路径限制不等于 Shell 沙箱。
- 设备认证、凭据保护和传输加密在产品化阶段形成完整方案。

尤其要明确：远程 Terminal 和 Agent 相当于获得了 Server 当前 Linux 用户的执行权限。不能把它描述成一个天然受限的文件浏览器。

## 9. 启动与关闭顺序

建议的启动顺序：

1. 读取配置并初始化日志。
2. 初始化 SQLite 和必要的缓存。
3. 初始化 WorkerPool、EventLoop 线程组和 RPC Router。
4. 初始化 Workspace、Process、PTY 等 Service。
5. 启动 TCP 监听。
6. 接受 Android 客户端连接。

关闭时：

1. 停止接受新连接和新请求。
2. 停止创建新的 Terminal/Agent 会话。
3. 根据策略等待在途任务结束。
4. 刷新需要持久化的元数据。
5. 清理临时资源并关闭连接。
6. 回收 WorkerPool 和 EventLoop 线程。

具体的超时、会话保留和强制终止策略在对应实现阶段确定。

## 10. 仓库组织原则

保留现有根目录 CMake 项目和 src/ 模块布局，不在 Phase 0 为了目录整齐而整体搬迁代码。

后续增量扩展可以采用：

    tina/
    ├── src/
    │   ├── net/          # 已有网络基础设施
    │   ├── rpc/          # 已有 RPC Runtime，逐步扩展
    │   ├── runtime/      # 日志、配置、WorkerPool 等
    │   ├── file/         # 文件传输协议
    │   └── service/
    │       ├── workspace/
    │       ├── terminal/
    │       ├── git/
    │       ├── agent/
    │       └── storage/
    ├── client/           # Tauri Android 客户端
    ├── test/
    ├── benchmark/
    └── docs/
        └── workbench/

这是目标逻辑结构，不要求 Phase 0 一次性创建所有目录。

## 11. Phase 0 完成条件

- 总体架构和模块职责已经明确。
- Android、Rust Backend、C++ Server 的边界已经明确。
- 通信协议的帧格式、消息类型和错误语义已经明确。
- 文件、终端、Git、Agent、Storage 的责任边界已经明确。
- 线程模型、背压、断线和会话生命周期有基本约定。
- 0–8 阶段路线已经明确。
- 本阶段只提交设计文档，不修改现有业务代码。