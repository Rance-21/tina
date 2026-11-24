export module rpc.task_classifier;

import std;
import runtime.dispatcher;
import ide.protocol;
import ide.server;

export namespace rpc::detail {

// 分类与缓存探测阶段传递到提交阶段的轻量上下文（复用 I/O 线程栈上空间，避免二次查缓存锁）。
struct PreparedDispatch {
    runtime::TaskClass task_class{runtime::TaskClass::blocking};
    std::shared_ptr<const ReadFileResponse> cached_file;
    std::size_t file_slice_offset{0};
    std::size_t file_slice_size{0};

    std::shared_ptr<const std::vector<DirEntry>> cached_dir;
    std::string cached_dir_path;

    // 【作用】将上下文重置回默认初始态，供同一连接连续拆包或槽位对象池回收复用。
    // 【原理】释放缓存快照的 shared_ptr 引用，对 cached_dir_path 调用 clear() 保留底层字符串容量避免重复分配，
    //         并将默认分类重置为保守的 blocking。
    void reset() noexcept {
        task_class = runtime::TaskClass::blocking;
        cached_file.reset();
        file_slice_offset = 0;
        file_slice_size = 0;
        cached_dir.reset();
        cached_dir_path.clear();
    }

    // 【作用】在 Worker 线程消费完预取快照后立即释放 shared_ptr 引用，避免延长大文件/目录缓存块生命周期，
    //         同时让回写阶段的 I/O 线程无需再触碰 prepared 所在的尾部缓存行。
    void clear_snapshots() noexcept {
        cached_file.reset();
        cached_dir.reset();
        cached_dir_path.clear();
    }
};

// 【作用】在 I/O 线程上对刚拆出的单帧 RPC 请求执行“内联直出判定 + 三级任务分类”（Inline / Normal / Blocking）。
// 【原理】按“实际执行路径”而非单纯按 Method 名称分流：
//   1. system_ping：纯内存响应，原地改写成 PONG 响应帧，返回 true 直接在 I/O 线程走 writev 发出；
//   2. workspace_read_file：探测内存缓存：
//      - 命中且切片 <= 32 KiB：原地编码响应，返回 true 走 Inline；
//      - 命中但切片 > 32 KiB：把不可变 shared_ptr 快照存入 out 并标记 Normal，交 Normal Worker 准备响应；
//      - 参数非法：不碰磁盘，标记 Normal 快速回错误帧；
//      - 缓存未命中：标记 Blocking，交 Blocking Worker 走磁盘读盘路径；
//   3. workspace_list_dir：同理探测目录缓存，<= 32 KiB 命中走 Inline（同时通过 inline_watched_dir
//      回传路径以便本 I/O 线程注册 inotify），> 32 KiB 命中走 Normal，未命中走 Blocking；
//   4. kv_* / workspace_close_dir / terminal_input：纯内存或仅向 EventLoop 投递写入缓冲，无阻塞等待，归入 Normal；
//   5. write_file / list_projects / terminal_on_tab / terminal_state / git_*：涉及磁盘、fsync、PTY 等待或 fork，归入 Blocking。
//   返回值：true 表示已在当前 I/O 线程就地完成响应；false 表示需交给 WorkerPool（分类与快照在 out 中）。
[[nodiscard]] inline bool
try_inline_or_classify(Frame &request, std::size_t inline_threshold, PreparedDispatch &out,
                       std::string &inline_watched_dir) {
    out.reset();
    inline_watched_dir.clear();

    if (is_event_only_method(request.method)) {
        out.task_class = runtime::TaskClass::normal;
        return false;
    }

    switch (request.method) {
    case Method::system_ping:
        request.kind = Kind::response;
        request.status = Status::ok;
        handle_ping(request);
        return true;

    case Method::workspace_read_file: {
        auto probe = server::probe_cached_read_file(request, inline_threshold);
        switch (probe.outcome) {
        case server::CachedReadProbe::Outcome::inline_handled:
            return true;
        case server::CachedReadProbe::Outcome::normal_cached:
            out.task_class = runtime::TaskClass::normal;
            out.cached_file = std::move(probe.cached);
            out.file_slice_offset = probe.slice_offset;
            out.file_slice_size = probe.slice_size;
            return false;
        case server::CachedReadProbe::Outcome::invalid_request:
            out.task_class = runtime::TaskClass::normal;
            return false;
        case server::CachedReadProbe::Outcome::blocking_miss:
            return false;
        }
    }

    case Method::workspace_list_dir: {
        auto probe =
            server::probe_cached_list_dir(request, inline_threshold, inline_watched_dir);
        switch (probe.outcome) {
        case server::CachedListDirProbe::Outcome::inline_handled:
            return true;
        case server::CachedListDirProbe::Outcome::normal_cached:
            out.task_class = runtime::TaskClass::normal;
            out.cached_dir = std::move(probe.cached);
            out.cached_dir_path = std::move(probe.dir_path);
            return false;
        case server::CachedListDirProbe::Outcome::invalid_request:
            out.task_class = runtime::TaskClass::normal;
            return false;
        case server::CachedListDirProbe::Outcome::blocking_miss:
            return false;
        }
    }

    case Method::kv_set:
    case Method::kv_get:
    case Method::kv_del:
    case Method::workspace_close_dir:
    case Method::terminal_input:
        // terminal_input 仅投递到所属 EventLoop 写入 PTY 缓冲并立即返回 OK，不阻塞等待命令完成；
        // 按实际执行路径归入 Normal，避免被慢磁盘/Git 任务拖累。
        out.task_class = runtime::TaskClass::normal;
        return false;

    case Method::workspace_list_projects:
    case Method::workspace_write_file:
    case Method::terminal_on_tab:
    case Method::terminal_state:
    case Method::git_status:
    case Method::git_diff:
    case Method::git_log:
        return false;

    default:
        out.task_class = runtime::TaskClass::normal;
        return false;
    }
}

} // namespace rpc::detail
