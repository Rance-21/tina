# Tina 远程开发工作台：通信协议

**阶段：** Phase 0（协议设计）  
**版本：** 第一版个人使用协议

## 1. 设计原则

这是 Tina 自己的 Android 客户端与 Fedora Server 之间使用的协议，不是面向第三方开发者的通用 RPC 标准。

因此第一版遵循：

- 不设计多版本兼容机制。
- 不设置 Payload Type。
- 不支持二进制 Payload。
- 所有 Payload 都是 UTF-8 文本。
- 客户端与服务端共用一套帧格式，由 Method 决定文本负载的含义。
- 只保留工作台与 KV 使用的 Method。

客户端和 Server 由同一个人维护，协议升级时两端一起更新。

## 2. 帧格式

所有整数均使用大端序（Big-endian）。不能直接发送 C++ 结构体的内存布局，必须显式编码每个字段。

| 字段 | 大小 | 说明 |
|---|---:|---|
| Length | 4 字节 | 后续帧的总字节数，不包含自身 |
| Kind | 1 字节 | REQUEST / RESPONSE / EVENT |
| Request ID | 8 字节 | 请求关联编号 |
| Method | 2 字节 | 操作编号 |
| Status | 2 字节 | 响应状态码 |
| Payload | 可变 | 按 Method 解释的 UTF-8 文本 |

固定帧头总计 **17 字节**。其中 Length 自身占 4 字节，因此 Length 的值为 13 + Payload 字节数。

### 2.1 Kind

| 值 | 类型 | 含义 |
|---:|---|---|
| 0x01 | REQUEST | Android 请求 Server 执行操作 |
| 0x02 | RESPONSE | Server 返回请求结果 |
| 0x03 | EVENT | Server 主动向 Android 推送事件 |

规则：

- REQUEST 的 Request ID 必须非零，Status 必须为 0。
- RESPONSE 的 Request ID 必须与原请求相同。
- EVENT 的 Request ID 固定为 0，Status 必须为 0。
- 一个 Request 只能对应一个 Response。
- Response 可以乱序返回，客户端按 Request ID 匹配。
- Terminal 和 Agent 的持续输出使用 EVENT。

### 2.2 Status

Status 只在 RESPONSE 中有意义。第一版保留统一状态码：

| 值 | 名称 | 含义 |
|---:|---|---|
| 0 | OK | 成功 |
| 1 | INVALID_ARGUMENT | 参数不合法 |
| 2 | UNAUTHENTICATED | 尚未认证 |
| 3 | PERMISSION_DENIED | 无权限 |
| 4 | NOT_FOUND | 目标不存在 |
| 5 | ALREADY_EXISTS | 目标已存在 |
| 6 | CONFLICT | 状态冲突 |
| 7 | RESOURCE_EXHAUSTED | 资源或队列达到上限 |
| 8 | UNSUPPORTED | 不支持的操作 |
| 9 | CANCELED | 操作已取消 |
| 10 | INTERNAL | 内部错误 |
| 11 | UNAVAILABLE | 服务不可用 |
| 12 | TIMEOUT | 操作超时 |

失败响应直接携带错误文本，客户端按 Status 判断结果。

## 3. Payload 编码

帧头后紧跟原始文本，长度按字节计算。收包时先按 Length 切出完整帧，再按 Kind 和 Method 处理。当前已实现的方法使用下表的文本格式。

| Method | 请求 Payload | 成功响应 Payload |
|---|---|---|
| system.ping | 零字节 | `PONG` |
| kv.set | 键、一个换行符、值（只按第一个换行切分） | `OK` |
| kv.get | 键 | 原始值 |
| kv.del | 键 | `OK` |

**范围限制：** 第一版只支持 UTF-8 文本文件和文本输出，不支持图片、压缩包等二进制文件。文件内容过大时通过 workspace.readFile 的范围参数分段读取。

## 4. Method 编号

Method 是 16 位无符号整数。下面统一写成 4 位十六进制；例如 0x0011 与 0x011 是同一个数值。

### 4.1 系统与 KV

| 编号 | Method | 类型 | 用途 |
|---:|---|---|---|
| 0x0001 | system.ping | Request | 连通性检查 |
| 0x0002 | kv.set | Request | 写入键值 |
| 0x0003 | kv.get | Request | 读取值 |
| 0x0004 | kv.del | Request | 删除键 |

### 4.2 Workspace

| 编号 | Method | 类型 | 用途 |
|---:|---|---|---|
| 0x0011 | workspace.listProjects | Request | 列出已登记项目 |
| 0x0012 | workspace.open | Request | 打开项目 |
| 0x0013 | workspace.listDir | Request | 枚举指定目录 |
| 0x0014 | workspace.readFile | Request | 读取 UTF-8 文本文件或指定范围 |
| 0x0015 | workspace.writeFile | Request | 保存 UTF-8 文本文件 |

### 4.3 Terminal

第一版只有一个默认 Terminal，由 Server 管理，不提供创建和关闭接口。

| 编号 | Method | 类型 | 用途 |
|---:|---|---|---|
| 0x0021 | terminal.input | Request | 向默认 Shell 发送输入 |
| 0x0022 | terminal.resize | Request | 设置终端行数和列数 |
| 0x0281 | terminal.output | Event | 推送终端输出 |
| 0x0282 | terminal.state | Event | 推送 Shell 启动、退出和重启状态 |

不提供 terminal.create、terminal.close。Android 断开连接、切换界面或进入后台都不会关闭 Shell。Shell 退出后由 Server 按默认策略重新启动。

### 4.4 Git

| 编号 | Method | 类型 | 用途 |
|---:|---|---|---|
| 0x0031 | git.status | Request | 查询仓库状态 |
| 0x0032 | git.diff | Request | 获取指定文件的代码差异 |

workspace.listDir 负责提供目录树数据；git.status 返回文件状态，客户端将状态标记叠加到目录树上。

git.diff 返回 Unified Diff，客户端将其绘制为逐行增加/删除的代码差异视图。第一版不实现提交历史关系图。

### 4.5 Agent

| 编号 | Method | 类型 | 用途 |
|---:|---|---|---|
| 0x0401 | agent.list | Request | 列出 Agent 会话 |
| 0x0402 | agent.start | Request | 启动 Agent 会话 |
| 0x0403 | agent.input | Request | 向 Agent 发送输入 |
| 0x0404 | agent.cancel | Request | 中断当前 Agent 任务 |
| 0x0405 | agent.close | Request | 终止 Agent 会话并释放进程 |
| 0x0481 | agent.output | Event | 推送 Agent 输出 |
| 0x0482 | agent.state | Event | 推送 Agent 状态变化 |

agent.cancel 和 agent.close 不能混为一谈：

- cancel：发送中断信号，停止当前任务，但尽可能保留 Agent 进程，让用户继续对话。
- close：结束 Agent 进程，释放 PTY 和会话资源。之后不能继续向该会话发送输入。

## 5. 主要 Payload 约定

### 5.1 system.ping

运行 `./tina server`，再在另一个终端运行 `./tina ping`。客户端连接默认端口 3490，发送空负载请求，服务端回显相同的 Request ID 和 Method，返回状态 OK 和文本 `PONG`。

### 5.2 预留方法

Workspace、Terminal、Git、Agent 的编号见第 4 节。它们目前返回 UNSUPPORTED，具体文本格式留到实现相应方法时确定。后续设计应保留以下语义。

- Workspace 以项目 ID 和相对路径定位文件，目录按需逐层展开。读取范围按 UTF-8 字节偏移表示，返回的文本在完整字符边界结束。保存时提交完整文件。
- Terminal 输入文本，调整大小时指定行列数。输出带序号，重连可返回近期输出快照。终端状态事件表示 Shell 启动、退出或重启。
- Git 状态包含分支和每个文件的暂存区、工作区及未跟踪状态。差异使用 Unified Diff，`worktree` 比较工作区与暂存区，`staged` 比较暂存区与 HEAD，未跟踪文件按新增处理。
- Agent 按配置和项目启动，以会话 ID 关联输入、输出和状态。输出带序号。状态包括 starting、running、busy、idle、exited、failed 和 closed。中断任务可保留会话，关闭会话会释放进程。

## 6. 长度限制与 TCP 解析

第一版初始限制：

| 项目 | 上限 |
|---|---:|
| 单帧 Length | 1 MiB |
| 文件读取请求 maxBytes | 256 KiB |
| Terminal 单次输出事件 | 64 KiB |
| Request ID | u64 |

Length 是整个帧的后续字节数。Server 必须先验证 Length，再决定是否分配对应缓冲区。

TCP 是字节流，因此必须处理：

- 半包：数据不完整时继续等待。
- 粘包：一次读取多个帧时逐帧解析。
- 部分写入：未写完的数据保留在输出缓冲区。
- 非法长度：小于固定头部长度或超过上限时拒绝。
- 非法文本负载：返回参数错误或关闭非法连接。
- 慢客户端：输出队列达到上限时执行背压，不能无限积累。

## 7. 第一版明确不支持的内容

- 二进制文件传输。
- 多个独立 Terminal 会话。
- Terminal 创建和关闭接口。
- Git 提交历史关系图。
- 多版本协议协商。
- Payload Type 和 Binary Payload。
- Agent 厂商专有的复杂 RPC 协议。

确实需要时再增加，不提前为这些功能增加通用抽象。
