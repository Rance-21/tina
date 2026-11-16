export module ide.client:tree;

import std;
import ide.protocol;

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

}
