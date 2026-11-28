export module ide.server;

export import :directory;
export import :read;
export import :write;
export import ide.server_protocol;

import std;

export namespace server {

// Workspace 模块请求统一分发入口（对应 MethodGroup::workspace 0x0200）。
void handle_workspace_request(Frame &frame) {
    switch (frame.method) {
    case Method::workspace_list_projects:
        handle_workspace_list_projects(frame);
        return;

    case Method::workspace_list_dir:
        handle_workspace_list_dir(frame);
        return;

    case Method::workspace_read_file:
        handle_workspace_read_file(frame);
        return;

    case Method::workspace_write_file:
        handle_workspace_write_file(frame);
        return;

    default:
        frame.status = Status::unsupported;
        frame.payload = "unsupported method";
        return;
    }
}

} // namespace server
