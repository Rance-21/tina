export module ide.server;

export import :directory;
export import :read;
export import :write;
export import ide.server_protocol;

import std;

export namespace server {

// Workspace 模块请求统一分发入口（对应 MethodGroup::workspace 0x0200）。
// list_dir 和 close_dir 都把目录路径交给同一个回包节点，I/O 线程按原 method 登记或取消监听。
void handle_workspace_request(Frame &frame, std::string &watched_dir) {
    switch (frame.method) {
    case Method::workspace_list_projects:
        handle_workspace_list_projects(frame);
        return;

    case Method::workspace_list_dir:
        handle_workspace_list_dir(frame, &watched_dir);
        return;

    case Method::workspace_read_file:
        handle_workspace_read_file(frame);
        return;

    case Method::workspace_write_file:
        handle_workspace_write_file(frame);
        return;

    case Method::workspace_close_dir:
        handle_workspace_close_dir(frame, watched_dir);
        return;

    default:
        frame.status = Status::unsupported;
        frame.payload = "unsupported method";
        return;
    }
}

} // namespace server
