export module rpc.task_classifier;

import std;
import runtime.dispatcher;
import ide.server;
import service.file;
import service.workspace_cache;

export namespace rpc::detail {

// 每帧只决定一次去向。所有缓存命中与 ping 均原地挂载零拷贝响应走 inline_task；
// Normal Worker 处理纯内存轻任务，Blocking Worker 执行磁盘访问或等待子进程。
runtime::TaskClass inline_or_blocking(const FrameView &request, Frame &response,
                                      service::WorkspaceHotCache &hot_cache,
                                      std::string_view &dir_path) {
    using enum runtime::TaskClass;

    auto init_inline_response = [&]() noexcept {
        response.kind = Kind::response;
        response.request_id = request.request_id;
        response.method = request.method;
        response.status = Status::ok;
    };

    switch (request.method) {
    case Method::system_ping:
        init_inline_response();
        response.clear_external_payload();
        handle_ping(request.payload, response);
        return inline_task;

    case Method::workspace_read_file: {
        auto req = server::decode_workspace_read_file_view(request.payload);
        if (!req || service::file::workspace_root_fd() < 0)
            return normal;
        auto file = hot_cache.get_file(service::global_workspace_cache(), req->path);
        if (!file)
            return service::file::is_valid_workspace_rel_path(req->path) ? blocking : normal;

        const auto slice = server::slice_file_content(*file, req->offset, req->max_bytes);
        init_inline_response();
        server::encode_workspace_read_file(response, std::move(file), slice);
        return inline_task;
    }

    case Method::workspace_list_dir: {
        auto path = server::decode_workspace_list_dir_view(request.payload);
        if (!path || service::file::workspace_root_fd() < 0)
            return normal;
        auto cached = hot_cache.get_dir(service::global_workspace_cache(), *path);
        if (!cached)
            return service::file::is_valid_workspace_rel_path(*path) ? blocking : normal;

        dir_path = *path;
        init_inline_response();
        server::encode_workspace_list_dir(response, std::move(cached));
        return inline_task;
    }

    case Method::workspace_list_projects:
    case Method::workspace_write_file:
    case Method::terminal_on_tab:
    case Method::terminal_state:
    case Method::git_status:
    case Method::git_diff:
    case Method::git_log:
        return blocking;

    default:
        // KV、close_dir、terminal_input，以及需返回 unsupported 的未知方法或推送方法。
        return normal;
    }
}

} // namespace rpc::detail
