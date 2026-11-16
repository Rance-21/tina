export module ide.client;

export import :tree;

import std;
import rpc.client;
export import ide.client_protocol;

export namespace client {

// workspace_write_file 客户端调用结果：
//   - conflict == false：保存成功，revision 为写入后的 newRevision；
//   - conflict == true ：服务端检测到文件版本冲突（Status::conflict），revision 为服务端最新
//                        currentRevision，客户端据此重新发起 read_file。
struct WriteFileResult {
    bool conflict{false};
    std::uint64_t revision{};
};

// ============================================================================
// Workspace 客户端会话封装（含 RPC 编解码、文件树状态维护与服务端 Event 自动刷新）
// ============================================================================
class WorkspaceClient {
  public:
    using DirEventCallback = std::function<void(const ListDirResponse &)>;

    explicit WorkspaceClient(std::string_view host = "127.0.0.1", std::string_view port = "3490")
        : rpc_(host, port) {
        rpc_.set_event_handler([this](Frame event) {
            handle_event(std::move(event));
        });
    }

    // 注册服务端推送目录变更 Event 时的额外通知回调。
    void set_dir_event_callback(DirEventCallback callback) {
        std::lock_guard lock(mutex_);
        dir_event_callback_ = std::move(callback);
    }

    // 初始化或切换当前维护的项目根文件树节点（如 "tina"）。
    void set_root_project(std::string_view project_name) {
        std::lock_guard lock(mutex_);
        root_ = make_project_root(project_name);
    }

    // 在树互斥锁保护下读取当前客户端文件树快照。
    template <typename Fn> decltype(auto) with_tree(Fn &&fn) const {
        std::lock_guard lock(mutex_);
        return std::forward<Fn>(fn)(root_.get());
    }

    // 0x0201: 列出 ~/projects 下的所有项目名称。
    std::vector<std::string> list_projects() {
        auto frame =
            rpc_.async_call(Method::workspace_list_projects, encode_workspace_list_projects())
                .get();
        if (frame.status != Status::ok)
            throw std::runtime_error("workspace_list_projects failed: " + frame.payload);

        auto projects = decode_workspace_list_projects(frame.payload);
        if (!projects)
            throw std::runtime_error("invalid workspace_list_projects response payload");

        return std::move(*projects);
    }

    // 0x0202: 请求指定目录的一级子项，自动维护 FileNode 的 Loading -> Loaded 状态转换。
    ListDirResponse list_dir(std::string_view path) {
        {
            std::lock_guard lock(mutex_);
            if (!root_) {
                const std::size_t slash = path.find('/');
                const std::string_view project =
                    (slash == std::string_view::npos) ? path : path.substr(0, slash);
                root_ = make_project_root(project);
            }
            if (auto *node = find_node(*root_, path))
                node->state = LoadState::Loading;
        }

        auto frame =
            rpc_.async_call(Method::workspace_list_dir, encode_workspace_list_dir(path)).get();
        if (frame.status != Status::ok) {
            std::lock_guard lock(mutex_);
            if (root_) {
                if (auto *node = find_node(*root_, path))
                    node->state = LoadState::Stale;
            }
            throw std::runtime_error("workspace_list_dir failed: " + frame.payload);
        }

        auto response = decode_workspace_list_dir(frame.payload);
        if (!response)
            throw std::runtime_error("invalid workspace_list_dir response payload");

        {
            std::lock_guard lock(mutex_);
            if (root_)
                (void)apply_list_dir(*root_, *response);
        }
        return std::move(*response);
    }

    // 0x0203: 分段读取 UTF-8 文件内容及其 fileSize 与 revision。
    ReadFileResponse read_file(std::string_view path, std::uint64_t offset = 0,
                               std::uint32_t max_bytes = max_file_read_bytes) {
        auto frame = rpc_.async_call(Method::workspace_read_file,
                                     encode_workspace_read_file(path, offset, max_bytes))
                         .get();
        if (frame.status != Status::ok)
            throw std::runtime_error("workspace_read_file failed: " + frame.payload);

        auto response = decode_workspace_read_file(frame.payload);
        if (!response)
            throw std::runtime_error("invalid workspace_read_file response payload");

        return std::move(*response);
    }

    // 0x0204: 带期望 revision 的原子文件写入。
    // 若服务端返回 Status::conflict，不抛异常，返回 WriteFileResult{.conflict = true, .revision = latest}
    // 供客户端重新调用 read_file。
    WriteFileResult write_file(std::string_view path, std::uint64_t expected_revision,
                               std::string_view content) {
        auto frame = rpc_.async_call(Method::workspace_write_file,
                                     encode_workspace_write_file(path, expected_revision, content))
                         .get();

        if (frame.status == Status::ok || frame.status == Status::conflict) {
            auto rev = decode_workspace_write_file(frame.payload);
            if (!rev)
                throw std::runtime_error("invalid workspace_write_file response payload");

            return WriteFileResult{
                .conflict = (frame.status == Status::conflict),
                .revision = *rev,
            };
        }

        throw std::runtime_error("workspace_write_file failed: " + frame.payload);
    }

    // 0x0205: 关闭指定目录，通知服务端 unwatch 对应的 inotify 监听，并重置本地文件树节点为 Unloaded。
    std::string close_dir(std::string_view path) {
        auto frame =
            rpc_.async_call(Method::workspace_close_dir, encode_workspace_close_dir(path)).get();
        if (frame.status != Status::ok)
            throw std::runtime_error("workspace_close_dir failed: " + frame.payload);

        auto closed_path = decode_workspace_close_dir(frame.payload);
        if (!closed_path)
            throw std::runtime_error("invalid workspace_close_dir response payload");

        const std::string_view target =
            closed_path->empty() ? path : std::string_view(*closed_path);
        {
            std::lock_guard lock(mutex_);
            if (root_)
                (void)apply_close_dir(*root_, target);
        }
        return closed_path->empty() ? std::string(path) : std::move(*closed_path);
    }

  private:
    void handle_event(Frame event) {
        if (event.kind != Kind::event || event.method != Method::workspace_list_dir)
            return;

        auto decoded = decode_workspace_list_dir(event.payload);
        if (!decoded)
            return;

        DirEventCallback callback;
        {
            std::lock_guard lock(mutex_);
            if (root_) {
                if (auto *node = find_node(*root_, decoded->path)) {
                    node->state = LoadState::Stale;
                    (void)apply_list_dir(*root_, *decoded);
                }
            }
            callback = dir_event_callback_;
        }

        if (callback)
            callback(*decoded);
    }

    rpc::RpcClient rpc_;
    mutable std::mutex mutex_;
    std::unique_ptr<FileNode> root_;
    DirEventCallback dir_event_callback_;
};

} // namespace client
