export module rpc.task_classifier;

import std;
import runtime.dispatcher;
import ide.server;
import service.file;
import service.workspace_cache;

export namespace rpc::detail {

// 只有需要 Worker 编码的缓存命中才填入快照。file 保活不可变内容，slice 可直接跨线程
// 引用其中的字节；directory 保活目录项。目录名另存在登记订阅所需的同一份字符串里。
struct WorkspaceSnapshot {
    service::WorkspaceCache::FileHandle file;
    std::string_view slice;
    service::WorkspaceCache::DirHandle directory;
};

// 每帧只决定一次去向。返回 inline_task 时响应已写入 response；否则按需产出快照，
// Normal Worker 消费快照或处理轻任务，Blocking Worker 可以执行磁盘访问或等待子进程。
runtime::TaskClass inline_or_blocking(const FrameView &request, Frame &response,
                                      std::size_t inline_threshold, WorkspaceSnapshot &snapshot,
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
        if (!req || !service::file::is_valid_workspace_rel_path(req->path) ||
            service::file::workspace_root_fd() < 0)
            return normal;
        auto file = service::global_workspace_cache().get_file(req->path);

        if (!file)
            return blocking;

        const auto slice = server::slice_file_content(*file, req->offset, req->max_bytes);
        // 阈值为零时，连 EOF 的空切片也交给 Worker。正阈值按正文大小判断，不含元数据头。
        if (inline_threshold && slice.size() <= inline_threshold) {
            init_inline_response();
            server::encode_workspace_read_file(response, std::move(file), slice);
            return inline_task;
        }

        snapshot.file = std::move(file);
        snapshot.slice = slice;
        return normal;
    }

    case Method::workspace_list_dir: {
        auto path = server::decode_workspace_list_dir_view(request.payload);
        if (!path || !service::file::is_valid_workspace_rel_path(*path) ||
            service::file::workspace_root_fd() < 0)
            return normal;

        auto cached = service::global_workspace_cache().get_dir(*path);
        if (!cached)
            return blocking;

        dir_path = *path;
        if (inline_threshold && cached->size() <= inline_threshold) {
            init_inline_response();
            server::encode_workspace_list_dir(response, std::move(cached));
            return inline_task;
        }

        snapshot.directory = std::move(cached);
        return normal;
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
