export module ide.client_git;

import std;
import rpc.client;
export import ide.client_protocol;

export namespace client {

// ============================================================================
// Git 提交历史图（DAG）数据结构与拓扑轨道（Lane）布局算法
// ============================================================================

// Git 图中的单个提交节点（包含原始提交信息、父子邻接下标与可视化泳道编号）。
struct GitGraphNode {
    GitCommit commit;                          // 提交原始元数据（hash, parents, refs, author, timestamp, subject）
    std::uint32_t index{};                     // 在拓扑序数组 nodes 中的下标（0 .. N-1）
    std::uint32_t lane{};                      // 分支泳道/列号（0 为主干，>= 1 为并行分叉轨道）
    bool is_head{false};                       // 该提交是否包含 HEAD 指向（refs 含 "HEAD"）
    std::vector<std::uint32_t> parent_indices; // 父节点在 nodes 中的下标列表
    std::vector<std::uint32_t> child_indices;  // 子节点在 nodes 中的下标列表
};

// Git 图中连接子提交与父提交的有向边（用于 UI 绘制主干直线与分叉/合并贝塞尔曲线）。
struct GitGraphEdge {
    std::uint32_t from_index{}; // 起点（子提交）下标
    std::uint32_t to_index{};   // 终点（父提交）下标
    std::uint32_t from_lane{};  // 起点所在泳道号
    std::uint32_t to_lane{};    // 终点所在泳道号
    bool is_merge{false};       // 是否为 Merge 提交的第二（或更多）父分支连线
};

// 客户端解码还原出的完整 Git 提交图。
struct GitGraph {
    std::vector<GitGraphNode> nodes; // 按拓扑序排列的提交节点列表（最多 100 个）
    std::vector<GitGraphEdge> edges; // 节点之间的父子连线列表
    std::uint32_t max_lanes{0};      // 图中同时并行的最大分支泳道数

    [[nodiscard]] const GitGraphNode *find_node(std::string_view hash) const noexcept {
        for (const auto &node : nodes) {
            if (node.commit.hash == hash)
                return &node;
        }
        return nullptr;
    }
};

// 根据服务端返回的拓扑序提交列表构建 Git 提交历史图（DAG 邻接关系 + 分支泳道分配）：
// 优化细节：
//   - 不加 inline，防止包含多趟循环与哈希表查找的大函数展开污染 L1 I-Cache；
//   - graph.nodes 一次性 resize 到最终大小后不再扩容，内部所有 commit.hash 与 parents 字符串地址恒定，
//     因此活跃泳道表 `active_lanes` 直接存储 `std::string_view` 视图，全程零字符串拷贝与零堆内存分配。
[[nodiscard]] GitGraph build_git_graph(std::vector<GitCommit> commits) {
    GitGraph graph;
    const auto count = static_cast<std::uint32_t>(commits.size());
    graph.nodes.resize(count);

    std::unordered_map<std::string_view, std::uint32_t> hash_to_idx;
    hash_to_idx.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        auto &node = graph.nodes[i];
        node.commit = std::move(commits[i]);
        node.index = i;
        node.is_head = (node.commit.refs.find("HEAD") != std::string::npos);
        hash_to_idx.emplace(node.commit.hash, i);
    }

    // 活跃泳道表（零拷贝 string_view）：active_lanes[lane] 指向该泳道下一个期望遇到的父 commit hash；空视图表示空闲。
    std::vector<std::string_view> active_lanes;
    active_lanes.reserve(8);

    auto allocate_lane = [&](std::string_view target_hash) -> std::uint32_t {
        for (std::uint32_t l = 0; l < active_lanes.size(); ++l) {
            if (active_lanes[l].empty()) {
                active_lanes[l] = target_hash;
                return l;
            }
        }
        active_lanes.push_back(target_hash);
        return static_cast<std::uint32_t>(active_lanes.size() - 1);
    };

    for (std::uint32_t i = 0; i < count; ++i) {
        auto &node = graph.nodes[i];
        const std::string_view cur_hash = node.commit.hash;

        // 1. 查找当前 commit 是否已经在某条活跃泳道的追踪目标中。
        std::optional<std::uint32_t> assigned_lane;
        for (std::uint32_t l = 0; l < active_lanes.size(); ++l) {
            if (active_lanes[l] == cur_hash) {
                if (!assigned_lane) {
                    assigned_lane = l;
                } else {
                    // 多条子分支汇入同一个父节点，释放多余泳道。
                    active_lanes[l] = {};
                }
            }
        }

        if (!assigned_lane)
            assigned_lane = allocate_lane(cur_hash);

        node.lane = *assigned_lane;
        graph.max_lanes = std::max(graph.max_lanes, node.lane + 1);

        // 2. 更新当前泳道与父提交泳道状态。
        if (node.commit.parents.empty()) {
            active_lanes[node.lane] = {};
        } else {
            const std::string_view first_parent = node.commit.parents[0];
            bool first_already_tracked = false;
            for (std::uint32_t l = 0; l < active_lanes.size(); ++l) {
                if (l != node.lane && active_lanes[l] == first_parent) {
                    first_already_tracked = true;
                    break;
                }
            }
            active_lanes[node.lane] = first_already_tracked ? std::string_view{} : first_parent;

            for (std::size_t p_idx = 1; p_idx < node.commit.parents.size(); ++p_idx) {
                const std::string_view merge_parent = node.commit.parents[p_idx];
                bool tracked = false;
                for (std::string_view tracked_hash : active_lanes) {
                    if (tracked_hash == merge_parent) {
                        tracked = true;
                        break;
                    }
                }
                if (!tracked) {
                    std::uint32_t new_lane = allocate_lane(merge_parent);
                    graph.max_lanes = std::max(graph.max_lanes, new_lane + 1);
                }
            }
        }

        // 3. 建立当前窗口内（<= 100 条）的父子索引关系。
        for (const auto &parent_hash : node.commit.parents) {
            auto it = hash_to_idx.find(parent_hash);
            if (it != hash_to_idx.end()) {
                const std::uint32_t parent_idx = it->second;
                node.parent_indices.push_back(parent_idx);
                graph.nodes[parent_idx].child_indices.push_back(i);
            }
        }
    }

    // 4. 所有节点的泳道确定后，生成连线列表 GitGraphEdge。
    graph.edges.reserve(count);
    for (const auto &node : graph.nodes) {
        for (std::size_t k = 0; k < node.parent_indices.size(); ++k) {
            const std::uint32_t parent_idx = node.parent_indices[k];
            graph.edges.push_back(GitGraphEdge{
                .from_index = node.index,
                .to_index = parent_idx,
                .from_lane = node.lane,
                .to_lane = graph.nodes[parent_idx].lane,
                .is_merge = (k > 0),
            });
        }
    }

    return graph;
}

// 直接从 0x0403 (git_log) 响应的二进制 Payload 解码并生成完整 GitGraph。
[[nodiscard]] std::optional<GitGraph> decode_git_graph(std::string_view payload) {
    auto commits = decode_git_log_response(payload);
    if (!commits)
        return std::nullopt;
    return build_git_graph(std::move(*commits));
}

// ============================================================================
// Git 客户端会话封装（支持手动刷新 status / diff / log 图，全程 move 避免冗余深拷贝）
// ============================================================================
class GitClient {
  public:
    explicit GitClient(std::string_view host = "127.0.0.1", std::string_view port = "3490",
                       std::string_view default_repo_path = {})
        : rpc_(host, port), repo_path_(default_repo_path) {
    }

    void set_repo_path(std::string_view repo_path) {
        std::lock_guard lock(mutex_);
        repo_path_.assign(repo_path);
    }

    [[nodiscard]] std::string repo_path() const {
        std::lock_guard lock(mutex_);
        return repo_path_;
    }

    // 0x0401 (git_status)：查询指定仓库的文件状态列表，直接 move 返回解码后的 vector。
    std::vector<GitStatusEntry> status(std::string_view path) {
        auto frame = rpc_.async_call(Method::git_status, encode_git_status_request(path)).get();
        if (frame.status != Status::ok)
            throw std::runtime_error("git_status failed: " + frame.payload);

        auto entries = decode_git_status_response(frame.payload);
        if (!entries)
            throw std::runtime_error("invalid git_status response payload");

        return std::move(*entries);
    }

    std::vector<GitStatusEntry> status() {
        return status(repo_path());
    }

    // 0x0402 (git_diff)：对比两个分支（同名分支对比 worktree 与 HEAD），直接 move 返回 frame.payload，零拷贝。
    std::string diff(std::string_view path, std::string_view base_branch,
                     std::string_view target_branch) {
        auto frame = rpc_.async_call(Method::git_diff,
                                     encode_git_diff_request(path, base_branch, target_branch))
                         .get();
        if (frame.status != Status::ok)
            throw std::runtime_error("git_diff failed: " + frame.payload);

        return std::move(frame.payload);
    }

    std::string diff(std::string_view base_branch, std::string_view target_branch) {
        return diff(repo_path(), base_branch, target_branch);
    }

    // 0x0403 (git_log)：拉取最多 100 条提交历史并解码构建 GitGraph，直接 move 返回。
    GitGraph log(std::string_view path) {
        auto frame = rpc_.async_call(Method::git_log, encode_git_log_request(path)).get();
        if (frame.status != Status::ok)
            throw std::runtime_error("git_log failed: " + frame.payload);

        auto graph = decode_git_graph(frame.payload);
        if (!graph)
            throw std::runtime_error("invalid git_log response payload");

        return std::move(*graph);
    }

    GitGraph log() {
        return log(repo_path());
    }

    // 手动一键并发刷新当前仓库的 status 与 git log 图缓存（全程 std::move 零深拷贝写入缓存）。
    void refresh() {
        const std::string current_repo = repo_path();
        auto st_future =
            rpc_.async_call(Method::git_status, encode_git_status_request(current_repo));
        auto log_future =
            rpc_.async_call(Method::git_log, encode_git_log_request(current_repo));

        auto st_frame = st_future.get();
        auto log_frame = log_future.get();

        if (st_frame.status != Status::ok)
            throw std::runtime_error("git_status refresh failed: " + st_frame.payload);
        if (log_frame.status != Status::ok)
            throw std::runtime_error("git_log refresh failed: " + log_frame.payload);

        auto entries = decode_git_status_response(st_frame.payload);
        auto graph = decode_git_graph(log_frame.payload);
        if (!entries || !graph)
            throw std::runtime_error("invalid git refresh response payload");

        std::lock_guard lock(mutex_);
        last_status_ = std::move(*entries);
        last_graph_ = std::move(*graph);
    }

    template <typename Fn> decltype(auto) with_cached_status(Fn &&fn) const {
        std::lock_guard lock(mutex_);
        return std::forward<Fn>(fn)(last_status_);
    }

    template <typename Fn> decltype(auto) with_cached_graph(Fn &&fn) const {
        std::lock_guard lock(mutex_);
        return std::forward<Fn>(fn)(last_graph_);
    }

  private:
    rpc::RpcClient rpc_;
    mutable std::mutex mutex_;
    std::string repo_path_;
    std::vector<GitStatusEntry> last_status_;
    GitGraph last_graph_;
};

} // namespace client
