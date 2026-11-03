# Tina 远程开发工作台：阶段路线图

**阶段：** Phase 0（总体规划）  
**版本：** 0.1

## 总目标

在现有 C++23/Linux Server 基础上，逐步构建一个 Android 可用的远程开发工作台。

Android 提供交互界面；Fedora 负责实际文件访问、Shell 命令、Git 操作和 Agent 运行。

开发顺序遵循：

**先打通通信，再实现具体功能；先形成可用闭环，再做性能研究；最后完成安全与产品化。**

## Phase 0：总体架构与协议

**目标：** 在写业务代码前，确定系统边界。

工作内容：
- 明确 Android、Rust Backend、C++ Server 的职责。
- 明确现有 RPC Runtime 的复用方式。
- 划分 Workspace、Terminal、Git、Agent、Storage 模块。
- 确定 Request / Response / Event 消息模型。
- 确定帧格式、Request ID、Method、Status 和 Payload。
- 确定文件传输、断线重连、背压和会话生命周期的基本规则。
- 确定 0–8 阶段开发顺序。

交付：
- architecture.md
- protocol.md
- roadmap.md

验收：
- 架构和模块边界明确。
- 协议设计足以指导 Phase 1。
- 本阶段只提交文档，不修改业务代码。

## Phase 1：Android 与 Fedora 建立连接

**目标：** Android 能稳定连接 Fedora Server，并完成一次请求/响应。

Server：
- 沿用现有 TCP、Epoll、EventLoop 和 RPC 基础。
- 演进协议帧，加入消息类型和服务端事件能力。
- 实现最小握手和 ping。
- 处理断开连接和资源回收。

Android：
- 创建 Tauri 2 项目。
- Rust Backend 实现 TCP 连接和协议编解码。
- 管理 Request ID 和 pending 请求。
- 将连接状态和响应交给 WebView。
- 提供最简单的连接状态界面。

验收：
- Android 可以连接 Fedora。
- 可以发送 ping 并收到匹配的响应。
- 多个并发请求不会串响应。
- 网络断开后能明确报告失败，并能重新连接。

## Phase 2：Workspace 文件浏览与编辑

**目标：** Android 能浏览 Fedora 上的项目并修改文件。

Server：
- Workspace 注册和打开。
- 目录枚举。
- 文件读取和写入。
- 大文件范围读取与分块传输。
- Workspace 根目录校验。
- 复用已有 File Service 协议和实现。

Client：
- 项目列表。
- 目录树。
- 文件查看。
- 接入成熟文本编辑器组件。
- 保存和错误提示。

验收：
- 能选择一个 Fedora 项目。
- 能浏览目录并打开文件。
- 能修改并保存文件。
- 大文件不会因为单次 RPC 帧过大而失败。
- 不能通过路径穿越访问 Workspace 以外的文件。

## Phase 3：PTY Terminal

**目标：** Android 可以使用 Fedora 的交互式 Shell。

Server：
- PTY 创建和销毁。
- Shell 进程管理。
- 输入、输出转发。
- 终端尺寸调整。
- 进程退出状态通知。
- PTY 文件描述符接入 I/O 事件循环。

Client：
- 接入成熟终端模拟器组件。
- 支持多终端会话。
- 支持输入、滚动和窗口尺寸变化。
- 正确处理终端控制序列和 UTF-8 输出。

验收：
- 可以运行交互式 Shell 命令。
- 可以运行需要 TTY 的程序。
- 可以调整终端大小。
- 断开 Android 后，按会话策略保留或清理进程。
- 不为每个 Terminal 创建一个阻塞线程。

## Phase 4：Git

**目标：** 在 Android 上查看项目版本控制状态。

Server：
- 识别 Git 仓库。
- 查询 status。
- 查询 diff。
- 对 Git 子进程设置超时和输出上限。
- 通过 WorkerPool 执行可能阻塞的 Git 命令。

Client：
- 显示当前分支。
- 显示修改、新增和删除文件。
- 展示 diff。

验收：
- 能查看仓库状态和文件差异。
- 大型仓库或 Git 命令执行较慢时，不阻塞网络 EventLoop。
- 错误和超时能够反馈到客户端。

## Phase 5：AI Agent

**目标：** 在 Android 上控制 Fedora 中运行的 Agent CLI。

Server：
- Agent Profile 配置。
- 启动、停止和监控 Agent 进程。
- 通过 PTY 提供交互式输入输出。
- 推送 Agent 输出和状态事件。
- 支持断线后重新连接到仍存活的会话。

Client：
- Agent 列表。
- 创建和选择 Agent 会话。
- 输入消息。
- 流式展示输出。
- 展示运行、退出和错误状态。

初始集成对象：
- Codex CLI。
- Antigravity CLI。

Agent 是外部进程。Tina 不实现模型推理，也不把某家 Agent 的专有协议塞进通用 RPC Runtime。

验收：
- 能启动配置好的 Agent。
- 能通过 Android 发送输入并接收持续输出。
- 多个会话可以独立管理。
- Agent 退出和异常状态能够正确反馈。

## Phase 6：Storage、缓存与恢复

**目标：** 管理工作台自身的元数据，并提升重复访问效率和恢复能力。

SQLite：
- Workspace 元数据。
- 设备和应用配置。
- Agent Profile。
- 必要的会话元数据。

Filesystem：
- 项目文件仍由 Linux 文件系统管理。
- 文件内容不复制到 SQLite。
- 临时传输数据按生命周期清理。

Memory Cache：
- 目录元数据缓存。
- 必要的文件元数据缓存。
- 容量上限和失效策略。
- 避免缓存与外部文件修改长期不一致。

恢复：
- Server 重启后恢复必要的配置和元数据。
- Android 重连后重新获取服务端状态。
- 对仍然存活的 Terminal/Agent 会话提供重新连接能力。

验收：
- 元数据可以在重启后恢复。
- 文件内容仍以文件系统为准。
- 缓存内存有明确上限。
- 重连不会错误地重复执行非幂等操作。

## Phase 7：性能工程

**目标：** 对整个系统建立可重复的性能基线，并根据真实瓶颈优化。

重点测量：
- RPC QPS 和 P50/P95/P99 延迟。
- 并发连接数。
- 文件读取和传输吞吐。
- Terminal 输出延迟。
- Agent 输出转发延迟。
- CPU 使用率和内存占用。
- EventLoop 延迟。
- 队列深度、背压和锁竞争。
- 缓存命中率及文件访问开销。

工具：
- perf stat
- perf record
- perf report
- 必要时使用 Flame Graph、Ftrace 或 BPF 工具。

保留 KV 服务作为稳定的 RPC/并发测试负载。它可以帮助隔离网络运行时和调度开销，不必为了工作台功能而删除。

优化方向由 Profiling 结果决定，例如减少不必要的数据复制、降低分配频率、调整队列和锁粒度、改善 I/O 批处理。

验收：
- 有可复现的 Benchmark 脚本和结果。
- 每项优化有优化前后数据。
- 能解释主要瓶颈及其原因。
- 不以“使用了某种高级技术”代替性能证据。

## Phase 8：安全、可靠性与产品化

**目标：** 把个人开发原型变成可以长期使用的工具。

工作内容：
- 设备配对和身份认证。
- 传输加密与凭据保护。
- Server 配置和权限边界检查。
- systemd 服务管理。
- 启动失败、崩溃和重启恢复。
- 连接重试与会话恢复。
- 资源上限、日志和错误处理。
- Android 安装包和基本设置界面。
- 端到端测试与使用文档。

安全要求：
- Server 不以 root 运行。
- 默认不暴露公网。
- Workspace API 不能越过授权目录。
- Terminal 和 Agent 权限必须明确告知用户。
- 认证和加密未完成前，不把服务开放到不可信网络。

验收：
- 可以按配置启动和停止 Server。
- Android 能安全配对和重新连接。
- 常见断线、重启和异常场景有明确恢复行为。
- 资源和权限边界清晰。
- 有基本安装、配置和使用文档。

## 全程开发原则

1. 保留现有 C++23/Linux RPC Runtime，优先复用已有实现。
2. 先做最小端到端闭环，不先搭建大量空模块。
3. 网络、并发、协议、进程生命周期等核心机制必须理解清楚。
4. UI、成熟组件接入和重复性 API 工作可以交给 Agent 辅助。
5. 不自研数据库、编辑器和终端模拟器。
6. 不做分布式系统。
7. Performance 优先，Storage 次之。
8. 不提前优化；所有性能结论以 Benchmark 和 Profiling 为依据。
9. 每个阶段都有可运行的验收结果。
10. 不为了新版工作台破坏已有 KV、文件传输和 RPC 实验成果。