# Tina 远程开发工作台：总体架构

**阶段：** Phase 0（架构设计）  
**版本：** 0.2  
**目标：** 个人使用的轻量级 Android 远程 IDE

## 1. 项目定位

Tina 是一个运行在 Fedora Linux 主机上的远程开发工作台，Android 平板/手机提供操作界面。

第一版只需要做好几件事：

- 浏览 Fedora 上的项目和目录树。
- 查看、编辑、保存 UTF-8 文本文件。
- 在 IDE 内使用一个长期存在的命令行。
- 查看 Git 状态和代码差异。
- 在 Fedora 上启动和管理 AI CLI Agent，并在 Android 上查看输出、发送输入。

不追求完整复刻 VS Code，不做插件市场、复杂多工作区管理或多机分布式系统。

**核心原则：Fedora 负责执行，Android 负责交互。**

源代码、编译器、Git、Shell 和 Agent 都运行在 Fedora 上。Android 通过 TCP 连接 Server，发送请求并接收结果和事件。

## 2. 技术结构

| 部分 | 技术 | 职责 |
|---|---|---|
| Server | Fedora + C++23 | 文件、终端、Git、Agent 的实际执行 |
| 网络运行时 | 现有 TCP / epoll / EventLoop / RPC | 连接、帧解析、请求分发和事件推送 |
| Client 原生层 | Tauri 2 + Rust | 管理 TCP 连接、协议编解码和前端桥接 |
| Client 界面 | WebView | 展示目录树、编辑器、终端、Git 和 Agent |
| 文件编辑器 | CodeMirror 6 | 编辑文本文件 |
| 终端显示 | xterm.js | 展示终端输出和处理终端输入 |
| 元数据 | 配置文件；必要时使用 SQLite | 保存项目和 Agent 配置 |
| 项目文件 | Fedora 原生文件系统 | 文件内容的唯一事实来源 |

不自研数据库、编辑器和终端模拟器。Rust Backend 负责网络，不让 WebView 直接管理 TCP。

## 3. 总体结构

    Android
    ┌───────────────────────────────────────┐
    │ WebView                               │
    │ 项目树 │ 编辑器 │ 终端 │ Git │ Agent  │
    ├───────────────────────────────────────┤
    │ Tauri Rust Backend                    │
    │ TCP Connection / Frame Codec          │
    │ Request Tracking / Event Forwarding   │
    └──────────────────┬────────────────────┘
                       │ TCP
                       │ Tina Protocol
    ┌──────────────────▼────────────────────┐
    │ Fedora：C++23 Tina Server             │
    │                                       │
    │ TCP / Epoll / EventLoop               │
    │             │                         │
    │         Frame Decoder                 │
    │             │                         │
    │          RPC Router                   │
    │    ┌────────┼──────────┐              │
    │    ▼        ▼          ▼              │
    │ Workspace  Terminal   Git / Agent     │
    │    │        │          │              │
    │    └────────┴──────────┘              │
    │             │                         │
    │       WorkerPool / PTY                │
    │             │                         │
    │ Filesystem / Git CLI / Agent CLI      │
    └───────────────────────────────────────┘

Server 不关心 Android 界面如何绘制，只提供稳定的 Method 和数据。前端根据返回的数据构建目录树、编辑器内容、Git 差异和 Agent 页面。

## 4. 复用现有 Tina 代码

新版 Tina 是现有 C++ Linux Server 的扩展，不另起炉灶，也不要求先重构整个仓库。

| 现有模块 | 复用职责 |
|---|---|
| net.socket | TCP Socket |
| net.epoll | Linux I/O 多路复用 |
| net.event_loop | I/O 事件循环和跨线程任务投递 |
| net.event_loop_thread_pool | I/O EventLoop 线程组 |
| net.buffer | 网络收发缓冲区 |
| runtime.worker_pool | 执行可能阻塞的任务 |
| runtime.log | 统一日志 |
| runtime.config | Server 配置 |
| rpc.protocol | 现有帧协议的演进基础 |
| rpc.connection | 连接状态和收发 |
| rpc.server / rpc.client | 现有 RPC 基础 |
| src/file/protocol.cppm | 现有分块文件协议，后续按需复用 |

实际文件名以仓库为准。Phase 0 只确定职责，不移动旧文件、不重命名模块。

KV 服务继续保留，作为 RPC 和并发性能测试负载。它不是新版 IDE 的核心功能。

## 5. 功能模块

### 5.1 Workspace

负责：

- 列出已登记项目。
- 打开项目。
- 按目录枚举文件和子目录。
- 读取 UTF-8 文本文件。
- 保存 UTF-8 文本文件。

目录树由 workspace.listDir 提供数据，客户端递归或按需展开目录。第一版不支持图片、压缩包等二进制文件的查看和编辑。

所有文件操作必须限制在已登记的 Workspace 根目录内，防止路径穿越和符号链接逃逸。

### 5.2 Terminal：一个长期存在的命令行

第一版只保留**一个默认 Terminal**，不做多终端标签页和复杂会话管理。

- Server 启动时创建一个 PTY（伪终端）和默认 Shell。
- IDE 始终显示这个终端面板，不提供关闭终端的按钮。
- 切换页面、Android 进入后台或 TCP 断开，都不关闭 Shell。
- Android 重连后，Server 提供最近一段终端输出，恢复显示。
- 终端尺寸变化通过 terminal.resize 通知 Server。
- 用户在终端中执行 exit 导致 Shell 退出时，Server 可以自动重启默认 Shell。
- Server 自身退出时，Shell 随之结束。

不提供 terminal.create、terminal.close 等多会话管理接口。终端输出缓冲区有固定上限，不能无限占用内存。

PTY 文件描述符接入现有 EventLoop，不为终端创建阻塞线程。

### 5.3 Git

第一版调用 Fedora 上的系统 Git CLI，不重新实现 Git。

- git.status 返回分支信息和文件状态。
- 客户端将 Git 状态叠加到 Workspace 目录树：修改、新增、删除、未跟踪等文件可以显示不同标记。
- git.diff 返回指定文件的统一差异文本（Unified Diff）。
- 客户端使用差异视图展示新增行、删除行和上下文行。

**目录树不是由 git.status 生成的：** 目录树来自 workspace.listDir，Git 状态负责给树节点添加状态标记。

**代码差异视图不是 Git 提交历史图：** 第一版支持文件级代码 Diff，不做分支/提交关系图。如果以后确实需要提交历史图，再增加 git.log。

对于未跟踪文件，Git 默认 diff 不会显示其内容；Server 应将其作为新文件差异返回，或由客户端明确显示为未跟踪文件。

耗时 Git 命令交给 WorkerPool，不能阻塞网络 EventLoop。

### 5.4 Agent

Agent 是 Fedora 上运行的外部 CLI 进程，例如 Codex CLI、Antigravity CLI。

职责：

- 列出已有 Agent 会话。
- 按 Profile 启动 Agent。
- 向 Agent 发送文本输入。
- 中断当前正在执行的任务。
- 结束 Agent 会话并回收进程。
- 将输出和状态变化推送给 Android。

Agent 使用独立 PTY。它与默认 Terminal 共用底层 Process/PTY 管理能力，但会话彼此独立。

agent.cancel 和 agent.close 语义不同：
- cancel：中断当前任务，尽可能保留 Agent 进程和会话。
- close：终止 Agent 进程，结束该会话并释放资源。

Agent 不需要在 Server 启动时自动全部运行，由用户在 IDE 中启动即可。

## 6. 并发模型

第一版继续使用现有的 Acceptor + I/O EventLoop 线程组 + WorkerPool。

- 一个 Connection 由一个 EventLoop 负责。
- 网络 I/O 和 PTY I/O 尽量由 EventLoop 驱动。
- 文件系统和 Git 等可能阻塞的操作交给有界 WorkerPool。
- Agent/Terminal 输出通过任务队列送回对应 EventLoop。
- 每条连接的待发送队列和终端输出缓存都有上限。
- 慢客户端不能让 Server 无限积累数据。

不提前引入复杂调度器、work-stealing 或自定义无锁结构。线程数和队列大小由后续压测决定。

## 7. 通信协议边界

- TCP 长连接。
- 固定帧头：Length、Kind、Request ID、Method、Status。
- 不包含 Version 和 Payload Type。
- Payload 统一为 UTF-8 文本，结构化内容使用 JSON。
- 由 Method 决定 JSON 字段结构。
- Request / Response / Event 三种消息类型。
- 终端和 Agent 的输出使用 Event 推送，不等待一个长时间 RPC 完成。

完整帧格式和 Method 编号见 protocol.md。

## 8. Storage 与性能

Storage 保持简单：

- 项目源代码和普通文件保留在 Linux 文件系统。
- Workspace 路径、Agent Profile 等少量配置优先使用配置文件。
- 确实需要持久化查询时再引入 SQLite。
- 不把项目文件内容存进数据库。
- 缓存必须有容量上限和失效规则。

性能是首要工程目标，重点观察 RPC 延迟、文件读取、终端输出、Agent 输出、CPU、内存、队列长度和锁竞争。所有优化由 Benchmark 和 Profiling 结果驱动。

## 9. 安全边界

- Server 以普通 Linux 用户身份运行，不使用 root。
- 开发阶段只监听明确配置的局域网地址，不做公网端口转发。
- Workspace API 必须限制在登记目录内。
- Terminal 和 Agent 拥有 Server 用户的权限；Workspace 路径限制不是 Shell 沙箱。
- 如果以后需要在不可信网络使用，必须先补齐身份认证和传输加密。

## 10. Phase 0 验收条件

- Server、Rust Backend、WebView 的职责明确。
- 帧结构和 Method 编号明确。
- 文件、单一持久终端、Git 和 Agent 的边界明确。
- 现有 C++ RPC Runtime 的复用方式明确。
- 后续开发顺序明确。
- 本阶段只提交设计文档，不修改业务代码。