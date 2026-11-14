export module ide.client;

import std;
import rpc.client;
export import ide.client_protocol;

export namespace client {

// 客户端文件树节点类型（与协议 DirEntryType 编码一一对应）。
enum class NodeType : std::uint8_t {
    file = 0,
    directory = 1,
    symlink = 2,

    File = 0,
    Directory = 1,
    Symlink = 2,
};

// 目录节点的加载状态：
//   Unloaded : 从未请求过这个目录
//   Loading  : 已发出 RPC，正在等待响应
//   Loaded   : 已经获得当前缓存的子目录列表
//   Stale    : Server 通知目录可能发生变化，需要刷新
enum class LoadState : std::uint8_t {
    Unloaded = 0,
    Loading = 1,
    Loaded = 2,
    Stale = 3,
};

// 客户端维护的工作区文件树节点。
// 由于 workspace_list_dir 每次只列出一级目录（深度为 1），未展开的子目录 children 为空。
struct FileNode {
    std::string name;
    std::string path;

    NodeType type{NodeType::directory};
    LoadState state{LoadState::Unloaded};

    std::vector<std::unique_ptr<FileNode>> children;
};

// workspace_write_file 客户端调用结果：
//   - conflict == false：保存成功，revision 为写入后的 newRevision；
//   - conflict == true ：服务端检测到文件版本冲突（Status::conflict），revision 为服务端最新
//                        currentRevision，客户端据此重新发起 read_file。
struct WriteFileResult {
    bool conflict{false};
    std::uint64_t revision{};
};

// 创建指定项目根节点的辅助函数（如 name="tina", path="tina"）。
[[nodiscard]] std::unique_ptr<FileNode> make_project_root(std::string_view project_name) {
    return std::make_unique<FileNode>(FileNode{
        .name = std::string(project_name),
        .path = std::string(project_name),
        .type = NodeType::directory,
        .state = LoadState::Unloaded,
        .children = {},
    });
}

// 在文件树中按相对路径（如 "tina" 或 "tina/src/ide"）查找节点。
[[nodiscard]] FileNode *find_node(FileNode &root, std::string_view target_path) noexcept {
    if (root.path == target_path)
        return &root;

    if (!root.path.empty()) {
        if (!target_path.starts_with(root.path) || target_path.size() <= root.path.size() ||
            target_path[root.path.size()] != '/')
            return nullptr;

        std::string_view rest = target_path.substr(root.path.size() + 1);
        FileNode *current = &root;
        while (!rest.empty()) {
            const std::size_t slash = rest.find('/');
            const std::string_view part =
                (slash == std::string_view::npos) ? rest : rest.substr(0, slash);

            FileNode *next = nullptr;
            for (const auto &child : current->children) {
                if (child && child->name == part) {
                    next = child.get();
                    break;
                }
            }
            if (next == nullptr)
                return nullptr;

            current = next;
            if (slash == std::string_view::npos)
                break;
            rest.remove_prefix(slash + 1);
        }
        return current;
    }

    for (const auto &child : root.children) {
        if (!child)
            continue;
        if (auto *found = find_node(*child, target_path))
            return found;
    }
    return nullptr;
}

[[nodiscard]] const FileNode *find_node(const FileNode &root,
                                        std::string_view target_path) noexcept {
    return find_node(const_cast<FileNode &>(root), target_path);
}

// 将目标目录节点标记为 Stale（需要刷新）。
bool mark_stale(FileNode &root, std::string_view dir_path) noexcept {
    auto *node = find_node(root, dir_path);
    if (node == nullptr || node->type != NodeType::directory)
        return false;

    node->state = LoadState::Stale;
    return true;
}

// 用服务端返回的 ListDirResponse（来自 RPC 响应或 inotify Event）增量更新目标目录节点的直接子项：
//   - 若旧子项中存在同名且同类型的目录节点，则复用原节点指针以保留已展开的子树状态与缓存；
//   - 新增子项初始 state 设为 LoadState::Unloaded，children 为空；
//   - 目标目录节点自身的 state 更新为 LoadState::Loaded。
FileNode *apply_list_dir(FileNode &root, const ListDirResponse &response) {
    FileNode *dir_node = find_node(root, response.path);
    if (dir_node == nullptr || dir_node->type != NodeType::directory)
        return nullptr;

    std::unordered_map<std::string_view, std::unique_ptr<FileNode>> existing;
    existing.reserve(dir_node->children.size());
    for (auto &child : dir_node->children) {
        if (child)
            existing.emplace(child->name, std::move(child));
    }

    std::vector<std::unique_ptr<FileNode>> updated;
    updated.reserve(response.entries.size());

    for (const auto &entry : response.entries) {
        const auto node_type = static_cast<NodeType>(std::to_underlying(entry.type));
        std::string child_path =
            dir_node->path.empty() ? entry.name : (dir_node->path + "/" + entry.name);

        auto it = existing.find(entry.name);
        if (it != existing.end() && it->second && it->second->type == node_type) {
            auto reused = std::move(it->second);
            reused->path = std::move(child_path);
            updated.push_back(std::move(reused));
        } else {
            updated.push_back(std::make_unique<FileNode>(FileNode{
                .name = entry.name,
                .path = std::move(child_path),
                .type = node_type,
                .state = LoadState::Unloaded,
                .children = {},
            }));
        }
    }

    dir_node->children = std::move(updated);
    dir_node->state = LoadState::Loaded;
    return dir_node;
}

// 关闭目标目录节点：清空其已缓存的一级子项列表，并将节点状态重置为 LoadState::Unloaded。
FileNode *apply_close_dir(FileNode &root, std::string_view dir_path) noexcept {
    FileNode *dir_node = find_node(root, dir_path);
    if (dir_node == nullptr || dir_node->type != NodeType::directory)
        return nullptr;

    dir_node->children.clear();
    dir_node->state = LoadState::Unloaded;
    return dir_node;
}

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
        auto frame =
            rpc_.async_call(Method::workspace_write_file,
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
