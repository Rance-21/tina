export module ide.server;

export import :directory;
export import :read;
export import :write;
export import ide.server_protocol;

import std;

export namespace server {

// Workspace 模块请求统一分发入口（对应 MethodGroup::workspace 0x0200）。
// watched_dir 用于回传 list_dir 需新增监听或 close_dir 需取消监听的目录相对路径（若额外提供了
// unwatched_dir 则优先写入 unwatched_dir）。
void handle_workspace_request(Frame &frame, std::string *watched_dir = nullptr,
                              std::string *unwatched_dir = nullptr) {
    switch (frame.method) {
    case Method::workspace_list_projects:
        handle_workspace_list_projects(frame);
        return;

    case Method::workspace_list_dir:
        handle_workspace_list_dir(frame, watched_dir);
        return;

    case Method::workspace_read_file:
        handle_workspace_read_file(frame);
        return;

    case Method::workspace_write_file:
        handle_workspace_write_file(frame);
        return;

    case Method::workspace_close_dir:
        handle_workspace_close_dir(frame, unwatched_dir != nullptr ? unwatched_dir : watched_dir);
        return;

    default:
        frame.status = Status::unsupported;
        frame.payload = "unsupported method";
        return;
    }
}

} // namespace server
