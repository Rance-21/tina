# Tina 远程开发工作台：通信协议

**阶段：** Phase 0（协议设计）  
**协议版本：** 1  
**状态：** 设计基线，尚未进入业务实现

## 1. 协议目标

Tina Protocol 用于 Android 客户端与 Fedora C++ Server 之间的双向通信。

协议必须同时支持三种通信模式：

1. **请求（Request）：** 客户端要求 Server 执行操作。
2. **响应（Response）：** Server 返回某个请求的结果。
3. **事件（Event）：** Server 主动向客户端推送异步数据。

例如：
- Android 请求列出目录，Server 返回目录项。
- Android 请求创建 Terminal，Server 返回 Session ID。
- Terminal 运行过程中，Server 持续推送终端输出事件。
- Agent 运行过程中，Server 持续推送 Agent 输出和状态事件。

协议不依赖 Tauri、Rust 或 C++ 的内部类型，双方只依赖线上字节格式。

## 2. 传输层

- 使用一条长期保持的 TCP 全双工连接。
- 一条连接可以承载多个并发请求。
- 响应通过 Request ID 与原请求关联，不要求按请求提交顺序返回。
- Event 不依赖某个尚未完成的 Request。
- TCP 是字节流，必须正确处理半包、粘包和部分写入。
- 开发阶段只允许在受控的局域网环境测试；公网部署前必须完成认证和传输加密。

TLS 和设备认证属于后续安全实现工作，本协议预留握手与认证方法，不把安全机制混入普通业务 Method。

## 3. 帧格式

所有整数均使用网络字节序，即大端序（Big-endian）。不能直接发送 C++ 结构体的内存布局。

一帧由固定帧头和 Payload 组成：

| 字段 | 字节数 | 含义 |
|---|---:|---|
| Length | 4 | 后续整帧的字节数，不包含自身这 4 字节 |
| Version | 1 | 协议版本，当前为 1 |
| Kind | 1 | 消息类型：Request / Response / Event |
| Request ID | 8 | 请求关联编号 |
| Method | 2 | 操作编号 |
| Status | 2 | 响应状态码 |
| Payload Type | 1 | Payload 编码类型 |
| Reserved | 1 | 保留字段，当前必须为 0 |
| Payload | 可变 | 消息正文 |

固定帧头总长度为 20 字节，其中 Length 占 4 字节，Length 字段的值包含后续 16 字节固定字段和 Payload。

### 3.1 Kind

| 值 | 类型 | 说明 |
|---:|---|---|
| 0x01 | REQUEST | 客户端请求 |
| 0x02 | RESPONSE | 服务端响应 |
| 0x03 | EVENT | 服务端主动事件 |

REQUEST 和 EVENT 的 Status 必须为 0。只有 RESPONSE 使用 Status 表示操作结果。

### 3.2 Request ID

- 普通 REQUEST 使用非零 64 位 Request ID。
- RESPONSE 必须复用对应 REQUEST 的 Request ID。
- EVENT 的 Request ID 固定为 0。
- 客户端不能在某个请求仍处于 pending 状态时复用其 ID。
- 响应可以乱序到达，客户端必须按 Request ID 找到对应请求。
- Request ID 不代表执行顺序，也不代表事务顺序。

### 3.3 Payload Type

| 值 | 类型 | 用途 |
|---:|---|---|
| 0 | EMPTY | 无正文 |
| 1 | JSON | UTF-8 编码的 JSON |
| 2 | BINARY | 原始二进制数据 |

控制类消息优先使用 JSON。文件内容、文件块、Terminal 输出和 Agent 输出使用 BINARY，避免对大块数据做 Base64 编码。

## 4. Payload 约定

### 4.1 JSON

JSON 用于传递参数、状态和元数据。

字段名称统一使用 lowerCamelCase。

示例：

    {
      "workspaceId": "workspace-001",
      "path": "src/main.cpp"
    }

### 4.2 二进制 Payload

为了让二进制数据也能携带元信息，统一采用：

    metadataLength : u32，大端序
    metadata       : UTF-8 JSON
    data           : 原始字节

metadataLength 表示后续 JSON 元数据的字节数，剩余字节全部属于 data。

例如文件读取响应的 metadata 可以描述文件总大小、当前偏移量和是否到达文件末尾；data 则直接保存文件字节。

Terminal 和 Agent 输出也可以在 metadata 中携带 Session ID、流类型等信息，data 保存原始输出字节。

## 5. Method 编号

Method 使用 16 位编号。编号一经分配，不得再用于其他含义。

### 5.1 系统与握手

| 编号 | Method | 方向 | 用途 |
|---:|---|---|---|
| 0x0001 | system.hello | Request | 交换协议版本和能力 |
| 0x0002 | system.ping | Request | 连通性检查 |

认证相关 Method 在安全实现阶段补充；未完成认证前，开发环境不得暴露到公网。

### 5.2 Workspace

| 编号 | Method | 用途 |
|---:|---|---|
| 0x0101 | workspace.listProjects | 列出已登记项目 |
| 0x0102 | workspace.open | 打开项目 |
| 0x0103 | workspace.listDir | 枚举目录 |
| 0x0104 | workspace.readFile | 读取文件或文件范围 |
| 0x0105 | workspace.writeFile | 写入文件 |

大文件读取和写入使用范围请求或分块传输，不能要求一次把整个大文件装入一个 RPC 帧。

### 5.3 Terminal

| 编号 | Method | 用途 |
|---:|---|---|
| 0x0201 | terminal.create | 创建 PTY 会话 |
| 0x0202 | terminal.input | 发送键盘输入 |
| 0x0203 | terminal.resize | 更新终端行列数 |
| 0x0204 | terminal.close | 关闭会话 |
| 0x0281 | terminal.output | 终端输出事件 |
| 0x0282 | terminal.exit | 进程退出事件 |

Terminal 输出事件携带 Session ID，客户端据此找到对应终端实例。

### 5.4 Git

| 编号 | Method | 用途 |
|---:|---|---|
| 0x0301 | git.status | 查询仓库状态 |
| 0x0302 | git.diff | 获取文件差异 |

第一版只覆盖最常用的状态和 diff 查询。其他 Git 操作可以通过 Terminal 执行，不需要在 RPC 层重复实现全部 Git 命令。

### 5.5 Agent

| 编号 | Method | 用途 |
|---:|---|---|
| 0x0401 | agent.list | 列出 Agent 会话 |
| 0x0402 | agent.start | 启动 Agent |
| 0x0403 | agent.input | 向 Agent 发送输入 |
| 0x0404 | agent.cancel | 请求取消 Agent |
| 0x0481 | agent.output | Agent 输出事件 |
| 0x0482 | agent.state | Agent 状态变化事件 |

Agent 输出可以是流式的。普通 RPC 响应只负责确认启动、输入提交或取消请求，不应等待整个 Agent 任务结束。

### 5.6 文件传输

已有 src/file/protocol.cppm 定义了分块文件传输相关结构。新版协议应优先评估并复用已有实现，不要重新设计一套互不兼容的文件分块协议。

文件传输至少需要表达：
- 上传会话 ID。
- 文件大小和文件名等元数据。
- Chunk 序号和校验值。
- 文件读取偏移量和长度。
- 上传提交结果。

文件传输 Method 的最终编号在接入现有 File Service 时统一登记。

## 6. 状态码

Status 使用 16 位整数。

| 值 | 名称 | 含义 |
|---:|---|---|
| 0 | OK | 成功 |
| 1 | INVALID_ARGUMENT | 参数不合法 |
| 2 | UNAUTHENTICATED | 尚未认证 |
| 3 | PERMISSION_DENIED | 无权限 |
| 4 | NOT_FOUND | 目标不存在 |
| 5 | ALREADY_EXISTS | 目标已存在 |
| 6 | CONFLICT | 状态冲突 |
| 7 | RESOURCE_EXHAUSTED | 队列或资源达到上限 |
| 8 | UNSUPPORTED | 不支持的操作 |
| 9 | CANCELED | 操作已取消 |
| 10 | INTERNAL | 内部错误 |
| 11 | UNAVAILABLE | 服务不可用 |
| 12 | TIMEOUT | 操作超时 |

失败响应的 JSON Payload 示例：

    {
      "code": "NOT_FOUND",
      "message": "Workspace does not exist"
    }

错误消息用于诊断，客户端逻辑应主要依据 Status 和稳定的错误 code，不应解析自然语言 message。

## 7. 数据大小与背压

初始限制：

| 项目 | 初始上限 |
|---|---:|
| 单帧 Length | 1 MiB |
| 默认文件块 | 256 KiB |
| Terminal 输出事件块 | 64 KiB |
| Request ID | u64 |

这些是初始设计值，后续可通过基准测试调整。

必须满足：

- 收到不完整帧时继续等待，不提前分发。
- 一次读取包含多帧时，必须逐帧解析。
- Length 超限时，在按 Length 分配大块内存前拒绝该帧。
- 非法 Version、Kind、Reserved 或 Payload Type 必须被拒绝。
- 非法 JSON 或不符合 Method 要求的 Payload 必须返回错误。
- Socket 只写出部分数据时，保留剩余数据并等待后续可写事件。
- 每条连接的发送队列和各类事件队列必须有界。
- 慢客户端不能导致 Server 内存无限增长。

队列达到上限时应有明确的背压策略。对于 Terminal/Agent 输出，不能无上限地把子进程输出积压在内存里。

## 8. 连接断开与请求语义

- 普通请求超时后，客户端将其标记为失败，并清理对应 pending 状态。
- 连接断开时，所有无法确认结果的 pending 请求都必须明确失败。
- 不确定 Server 是否已经执行的非幂等请求不能自动重放。
- 重连后客户端重新查询 Workspace 和会话状态。
- Terminal/Agent 会话是否保留由 Server 的会话管理策略决定，不由 TCP 连接的存活状态直接决定。

## 9. 版本兼容

协议当前版本为 1。

- system.hello 用于交换版本和能力。
- 新增 Method 可以使用尚未分配的编号，不改变现有帧结构。
- 未知 Method 返回 UNSUPPORTED。
- 破坏帧结构的修改必须提升协议版本。
- Reserved 字段在版本 1 中必须为 0。

## 10. 实现边界

Phase 0 只确定协议，不实现新业务接口。

进入 Phase 1 后，先实现最小闭环：
1. TCP 建立连接。
2. 交换版本信息。
3. 发送 ping 请求。
4. 返回对应 Request ID 的响应。
5. 验证断线和重连。

随后再按 Workspace、Terminal、Git、Agent 的阶段逐步增加 Method。