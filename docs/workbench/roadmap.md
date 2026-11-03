# Tina 远程开发工作台：开发路线图

**阶段：** Phase 0（总体规划）  
**目标：** 个人使用的轻量级 Android 远程 IDE

## 总体原则

- Fedora 负责真正执行，Android 负责交互。
- 保留现有 C++23/Linux RPC Runtime。
- 只实现实际需要的功能，不追求完整复刻 VS Code。
- 协议使用简单的 UTF-8 文本和 Method-specific JSON。
- Performance 是首要工程目标，Storage 次之。
- 每个阶段都必须有可运行的验收结果。

## Phase 0：架构和协议

工作内容：
- 明确 Android、Rust Backend、C++ Server 的职责。
- 确定固定帧结构和 Request / Response / Event。
- 确定 Method 编号及 Payload JSON 结构。
- 确定 Workspace、单一 Terminal、Git、Agent 的功能边界。
- 确定现有代码的复用方式。

交付：
- architecture.md
- protocol.md
- roadmap.md

验收：
- 架构和协议足以指导 Phase 1。
- 本阶段只提交文档，不修改业务代码。

## Phase 1：打通 Android 与 Fedora

Server：
- 复用现有 TCP、Epoll、EventLoop 和 RPC 基础。
- 按新版帧结构解析和编码。
- 实现 system.ping。
- 处理 Request ID 和 Response。
- 处理断开连接和资源回收。

Client：
- 创建 Tauri 2 项目。
- Rust Backend 实现 TCP 连接和帧编解码。
- 管理并发请求和响应匹配。
- 将连接状态交给 WebView 展示。

验收：
- Android 可以连接 Fedora。
- 可以发送 ping 并收到响应。
- 多个并发请求不会串响应。
- 断线后能重新连接。

## Phase 2：Workspace 文件浏览和编辑

Server：
- 项目列表和打开项目。
- 目录枚举。
- UTF-8 文本文件读取和保存。
- 文件范围读取。
- Workspace 根目录校验。

Client：
- 项目选择。
- 目录树。
- 文件查看和编辑。
- 保存操作。
- 接入 CodeMirror 6。

验收：
- 能打开 Fedora 上的 Tina 项目。
- 能浏览目录树。
- 能打开、修改并保存 C++ 源文件。
- 不能通过路径穿越访问 Workspace 以外的文件。

## Phase 3：单一持久 Terminal

Server：
- Server 启动时创建一个 PTY 和默认 Shell。
- 接收终端输入。
- 推送终端输出。
- 接收终端窗口大小变化。
- 保存固定容量的最近输出。
- 客户端断线不关闭 Shell。
- Shell 意外退出后按策略重启。

Client：
- 接入 xterm.js。
- IDE 中始终保留终端面板。
- 支持键盘输入、滚动和尺寸调整。
- 重连后恢复最近输出。

验收：
- 可以在 Android 上使用 Fedora Shell。
- 切换界面和网络断开不会关闭 Shell。
- 可以正常运行 CMake、Git 等命令。
- 终端输出缓存有容量上限。

## Phase 4：Git 状态和代码差异

Server：
- 调用系统 Git CLI。
- 查询分支和文件状态。
- 获取指定文件的 Unified Diff。
- 处理未跟踪文件。
- 通过 WorkerPool 执行可能阻塞的命令。

Client：
- 将 Git 状态装饰到目录树。
- 展示修改、新增、删除和未跟踪文件。
- 使用差异视图展示新增行和删除行。

验收：
- 目录树能够显示 Git 状态。
- 选中文件可以查看代码差异。
- Git 操作不会阻塞网络 EventLoop。

## Phase 5：Agent

Server：
- 配置 Agent Profile。
- 启动外部 CLI 进程。
- 为每个 Agent 分配独立 PTY。
- 接收 Agent 输入。
- 推送 Agent 输出和状态。
- 支持 cancel 和 close。
- 处理进程退出和资源回收。

Client：
- Agent 会话列表。
- 启动 Agent。
- 输入和流式输出。
- 中断当前任务。
- 结束 Agent 会话。

初始对象：
- Codex CLI。
- Antigravity CLI。

验收：
- 能从 Android 启动 Fedora 上的 Agent。
- 能发送输入并持续接收输出。
- cancel 不等于 close。
- 多个 Agent 会话互不干扰。

## Phase 6：配置持久化和性能测试

Storage：
- 保存 Workspace 配置。
- 保存 Agent Profile。
- 保存必要的应用设置。
- 文件内容仍由 Linux 文件系统管理。
- 配置简单时优先使用配置文件，确实需要查询时再引入 SQLite。

Performance：
- 测量 RPC 延迟和吞吐。
- 测量文件读取和保存耗时。
- 测量 Terminal 输出延迟。
- 测量 Agent 输出转发延迟。
- 观察 CPU、内存、队列长度和锁竞争。
- 使用 perf 等工具定位真实瓶颈。

验收：
- 配置可以在 Server 重启后恢复。
- 有可重复的 Benchmark。
- 每项优化都有优化前后数据。

## Phase 7：稳定性和日常使用

- Server 启停和日志。
- 网络断开与重连。
- Terminal 和 Agent 进程清理。
- 错误提示和资源限制。
- Android 安装包。
- 基本配置界面和使用说明。
- 端到端测试。

验收：
- 可以日常使用 Android 连接 Fedora。
- 文件、终端、Git 和 Agent 的核心流程稳定。
- Server 不会因单个 Agent 或 Git 操作异常而整体退出。

## 始终遵守的约束

1. 不做分布式系统。
2. 不自研数据库、文本编辑器和终端模拟器。
3. 不做多终端管理，第一版只保留一个持久 Terminal。
4. 不支持二进制文件，第一版只处理 UTF-8 文本。
5. 不增加没有实际用途的协议字段和 Method。
6. 不为性能而提前引入复杂无锁结构。
7. 优先复用已有 Tina 代码。
8. UI 和重复性 API 接入可以交给 Agent 辅助；网络、并发、协议和进程生命周期需要理解清楚。