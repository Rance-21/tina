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
    std::shared_ptr<const ReadFileResponse> file;
    std::string_view slice;
    std::shared_ptr<const std::vector<DirEntry>> directory;
};

// 每帧只决定一次去向。返回 inline_task 时响应正文已写进 frame；否则保留请求负载，
// Normal Worker 消费快照或处理轻任务，Blocking Worker 可以执行磁盘访问或等待子进程。
runtime::TaskClass inline_or_blocking(Frame &frame, std::size_t inline_threshold,
                                      WorkspaceSnapshot &snapshot, std::string &dir_path) {
    using enum runtime::TaskClass;
    switch (frame.method) {
    case Method::system_ping:
        handle_ping(frame);
        return inline_task;

    case Method::workspace_read_file: {
        auto req = server::decode_workspace_read_file_view(frame.payload);
        if (!req || !service::file::is_valid_workspace_rel_path(req->path) ||
            !service::file::has_projects_root_env())
            return normal;
        auto file = service::global_workspace_cache().get_file(req->path);
        if (!file)
            return blocking;
        const auto slice = server::slice_file_content(*file, req->offset, req->max_bytes);
        // 阈值为零时，连 EOF 的空切片也交给 Worker。正阈值按正文大小判断，不含元数据头。
        if (inline_threshold && slice.size() <= inline_threshold) {
            server::encode_workspace_read_file(frame, std::move(file), slice);
            return inline_task;
        }
        snapshot.file = std::move(file);
        snapshot.slice = slice;
        return normal;
    }

    case Method::workspace_list_dir: {
        auto path = server::decode_workspace_list_dir_view(frame.payload);
        if (!path || !service::file::is_valid_workspace_rel_path(*path) ||
            !service::file::has_projects_root_env())
            return normal;
        auto entries = service::global_workspace_cache().get_dir(*path);
        if (!entries)
            return blocking;
        // 路径视图指向即将被编码器改写的 payload。先保存一次，回包后仍可用来订阅。
        dir_path.assign(*path);
        if (server::try_encode_list_dir(dir_path, *entries, frame.payload, inline_threshold))
            return inline_task;
        snapshot.directory = std::move(entries);
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
