export module ide.server;

import std;
import ide.protocol;

// ============================================================================
// Workspace 模块服务端业务处理函数
// 职责：在 Worker 线程中执行具体的工作区与文件操作，并将结果写入响应 Frame。
// 目前暂留空实现，待确定具体存储与文件操作细节后再补齐。
// ============================================================================

// 处理 0x0011 (workspace_list_projects)：列出服务端已登记的项目列表。
export void handle_workspace_list_projects(Frame & /* frame */) {
}

// 处理 0x0013 (workspace_list_dir)：枚举指定目录下的一级文件与子目录。
export void handle_workspace_list_dir(Frame & /* frame */) {
}

// 处理 0x0014 (workspace_read_file)：按字节偏移与长度上限分段读取 UTF-8 文本文件。
export void handle_workspace_read_file(Frame & /* frame */) {
}

// 处理 0x0015 (workspace_write_file)：将完整的 UTF-8 文本内容保存到目标文件。
export void handle_workspace_write_file(Frame & /* frame */) {
}

// Workspace 模块请求统一分发入口（对应 Method 掩码分组 0x0010）。
// 流程：先调用 ide.protocol 中的解码函数解析请求 payload，再交由对应的处理函数执行。
export void handle_workspace_request(Frame &frame) {
    switch (frame.method) {
    case Method::workspace_list_projects:
        decode_workspace_list_projects(frame.payload);
        handle_workspace_list_projects(frame);
        return;

    case Method::workspace_list_dir:
        decode_workspace_list_dir(frame.payload);
        handle_workspace_list_dir(frame);
        return;

    case Method::workspace_read_file:
        decode_workspace_read_file(frame.payload);
        handle_workspace_read_file(frame);
        return;

    case Method::workspace_write_file:
        decode_workspace_write_file(frame.payload);
        handle_workspace_write_file(frame);
        return;

    default:
        frame.status = Status::unsupported;
        frame.payload = "unsupported method";
        return;
    }
}
