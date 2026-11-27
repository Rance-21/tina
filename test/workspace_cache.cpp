#include <stdlib.h>

import std;
import ide.protocol;
import ide.server_protocol;
import ide.server;
import net.buffer;
import net.event_loop;
import service.file;
import service.file_watcher;
import service.workspace_cache;

namespace {

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "CHECK failed at line " << __LINE__ << ": " #cond << '\n';                \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

std::string encode_list_dir_req(std::string_view path) {
    std::string out;
    append_be(out, static_cast<std::uint16_t>(path.size()));
    out.append(path);
    return out;
}

std::string encode_read_file_req(std::string_view path, std::uint64_t offset,
                                 std::uint32_t max_bytes) {
    std::string out;
    append_be(out, static_cast<std::uint16_t>(path.size()));
    out.append(path);
    append_be(out, offset);
    append_be(out, max_bytes);
    return out;
}

int test_lru_and_invalidation() {
    // 配置较小的分片容量以确定性验证 LRU 淘汰与内存上限控制。
    service::WorkspaceCacheConfig cfg{
        .max_dir_entries = 64, // 每个分片 4 条，避免无关测试键发生容量淘汰
        .max_dir_bytes = 64 * 1024,
        .max_file_entries = 64, // 每个分片 4 条
        .max_file_bytes = 64 * 1024,
        .max_per_file_bytes = 64,
    };
    service::WorkspaceCache cache(cfg);

    // 1. 验证 epoch 并发保护：读盘期间发生 invalidate，旧读不能回填缓存。
    const auto stale_epoch = cache.dir_epoch("demo");
    cache.invalidate_dir("demo");
    auto stale_dir = std::make_shared<const std::string>("stale-encoded");
    cache.put_dir("demo", stale_dir, stale_epoch);
    CHECK(cache.get_dir("demo") == nullptr);

    // 使用最新 epoch 写入应成功命中。
    const auto fresh_epoch = cache.dir_epoch("demo");
    auto fresh_dir = std::make_shared<const std::string>("fresh-encoded");
    cache.put_dir("demo", fresh_dir, fresh_epoch);
    auto got_dir = cache.get_dir("demo");
    CHECK(got_dir != nullptr && *got_dir == "fresh-encoded");

    // 2. 验证 unwatch 级联淘汰目录自身及其直接子文件，但不误伤子目录或无关目录。
    const auto f1_ep = cache.file_epoch("demo/a.txt");
    cache.put_file("demo/a.txt",
                   std::make_shared<ReadFileResponse>(ReadFileResponse{5, 1, "hello"}), f1_ep);
    const auto f2_ep = cache.file_epoch("demo/sub/b.txt");
    cache.put_file("demo/sub/b.txt",
                   std::make_shared<ReadFileResponse>(ReadFileResponse{5, 2, "world"}), f2_ep);
    const auto d2_ep = cache.dir_epoch("demo2");
    cache.put_dir("demo2", fresh_dir, d2_ep);

    cache.invalidate_dir_and_files("demo");
    CHECK(cache.get_dir("demo") == nullptr);
    CHECK(cache.get_file("demo/a.txt") == nullptr);
    CHECK(cache.get_file("demo/sub/b.txt") != nullptr);
    CHECK(cache.get_dir("demo2") != nullptr);

    // 3. 验证子树淘汰（严格按 '/' 边界匹配，不误伤 demo2）。
    const auto sub_dir_ep = cache.dir_epoch("demo/sub");
    cache.put_dir("demo/sub", fresh_dir, sub_dir_ep);
    cache.invalidate_subtree("demo");
    CHECK(cache.get_dir("demo/sub") == nullptr);
    CHECK(cache.get_file("demo/sub/b.txt") == nullptr);
    CHECK(cache.get_dir("demo2") != nullptr);

    // 4. 验证超限文件不入缓存，且 write_file 超限更新会失效旧缓存。
    const auto small_ep = cache.file_epoch("demo/s.txt");
    cache.put_file("demo/s.txt",
                   std::make_shared<ReadFileResponse>(ReadFileResponse{3, 10, "abc"}), small_ep);
    CHECK(cache.get_file("demo/s.txt") != nullptr);

    cache.put_file_after_write(
        "demo/s.txt",
        std::make_shared<ReadFileResponse>(ReadFileResponse{100, 11, std::string(100, 'x')}));
    CHECK(cache.get_file("demo/s.txt") == nullptr);

    // 5. 验证同一分片内 LRU 淘汰顺序，以及淘汰回调正确触发。
    service::WorkspaceCache lru_cache(service::WorkspaceCacheConfig{
        .max_dir_entries = 32, // 每分片最多 2 条
        .max_dir_bytes = 1024 * 1024,
        .max_file_entries = 32,
        .max_file_bytes = 1024 * 1024,
        .max_per_file_bytes = 1024,
    });
    std::size_t file_evict_count = 0;
    lru_cache.set_file_evict_callback([&](std::string_view) {
        ++file_evict_count;
    });
    // 构造 40 个不同目录键与文件键，必然触发多轮同分片 LRU 淘汰且总条目数 <= 32。
    for (int i = 0; i < 40; ++i) {
        const std::string key = "proj/d_" + std::to_string(i);
        const auto ep = lru_cache.dir_epoch(key);
        lru_cache.put_dir(key, fresh_dir, ep);

        const std::string fkey = "proj/f_" + std::to_string(i);
        const auto fep = lru_cache.file_epoch(fkey);
        lru_cache.put_file(fkey,
                           std::make_shared<ReadFileResponse>(ReadFileResponse{3, 1, "abc"}), fep);
    }
    CHECK(lru_cache.dir_cache_size() <= 32);
    CHECK(lru_cache.file_cache_size() <= 32);
    CHECK(file_evict_count >= 8);

    return 0;
}

int test_file_watcher_independent() {
    const std::filesystem::path tmp_root =
        std::filesystem::temp_directory_path() / "tina_file_watcher_test";
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
    const std::filesystem::path dir = tmp_root / "unlisted";
    std::filesystem::create_directories(dir, ec);
    CHECK(!ec);

    const std::filesystem::path file_path = dir / "solo.txt";
    {
        std::ofstream ofs(file_path, std::ios::binary);
        ofs << "v1";
    }

    service::WorkspaceCache cache(service::WorkspaceCacheConfig{
        .max_dir_entries = 64,
        .max_dir_bytes = 1024 * 1024,
        .max_file_entries = 32,
        .max_file_bytes = 1024 * 1024,
        .max_per_file_bytes = 4096,
    });
    service::FileWatcher watcher(&cache);

    net::EventLoop loop;
    std::jthread loop_thread([&loop] {
        loop.run();
    });

    auto started = std::make_shared<std::latch>(1);
    loop.post_task([&loop, &watcher, started] {
        watcher.start(loop);
        started->count_down();
    });
    started->wait();

    // 写入缓存并独立注册 FileWatcher（未经过 DirWatcher）。
    const auto ep = cache.file_epoch("unlisted/solo.txt");
    watcher.watch_file("unlisted/solo.txt", file_path.string());
    cache.put_file("unlisted/solo.txt",
                   std::make_shared<ReadFileResponse>(ReadFileResponse{2, 1, "v1"}), ep);
    CHECK(cache.get_file("unlisted/solo.txt") != nullptr);
    CHECK(watcher.watched_count() == 1);

    // 原地修改文件内容，等待 FileWatcher 通过 epoll 收到 IN_MODIFY 并失效文件缓存。
    {
        std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
        ofs << "v2_edited";
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (cache.get_file("unlisted/solo.txt") != nullptr &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(cache.get_file("unlisted/solo.txt") == nullptr);

    // 触发 LRU 淘汰时，evict 回调应同步撤销 FileWatcher 中的 watch。
    for (int i = 0; i < 40; ++i) {
        const std::string rel = "unlisted/extra_" + std::to_string(i) + ".txt";
        const std::filesystem::path p = dir / ("extra_" + std::to_string(i) + ".txt");
        {
            std::ofstream ofs(p, std::ios::binary);
            ofs << "x";
        }
        const auto fep = cache.file_epoch(rel);
        watcher.watch_file(rel, p.string());
        cache.put_file(rel, std::make_shared<ReadFileResponse>(ReadFileResponse{1, 1, "x"}), fep);
    }
    CHECK(watcher.watched_count() <= 33);

    auto stopped = std::make_shared<std::latch>(1);
    loop.post_task([&loop, &watcher, stopped] {
        watcher.stop();
        loop.stop();
        stopped->count_down();
    });
    stopped->wait();
    loop_thread.join();

    std::filesystem::remove_all(tmp_root, ec);
    return 0;
}

int test_handlers_skip_filesystem_on_hit() {
    const std::filesystem::path tmp_root =
        std::filesystem::temp_directory_path() / "tina_workspace_cache_test";
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);

    const std::filesystem::path proj_dir = tmp_root / "projects" / "demo";
    std::filesystem::create_directories(proj_dir, ec);
    CHECK(!ec);

    {
        std::ofstream ofs(proj_dir / "note.txt", std::ios::binary);
        ofs << "abcdef";
    }

    const char *old_home = ::getenv("HOME");
    const std::string saved_home = old_home ? old_home : "";
    ::setenv("HOME", tmp_root.c_str(), 1);
    service::file::init_workspace_root();

    auto restore_env = [&]() {
        if (old_home)
            ::setenv("HOME", saved_home.c_str(), 1);
        else
            ::unsetenv("HOME");
        service::file::init_workspace_root();
        std::filesystem::remove_all(tmp_root, ec);
        service::global_workspace_cache().clear();
    };

    service::global_workspace_cache().clear();

    // 1. 首次 list_dir 进入文件系统，并将协议编码后的负载写入缓存。
    Frame list_frame{
        .kind = Kind::request,
        .request_id = 1,
        .method = Method::workspace_list_dir,
        .status = Status::ok,
        .payload = encode_list_dir_req("demo"),
    };
    server::handle_workspace_list_dir(list_frame);
    CHECK(list_frame.status == Status::ok);
    CHECK(list_frame.payload.empty());
    CHECK(list_frame.external_payload.size() >= 6 &&
          list_frame.external_payload.substr(2, 4) == "demo");
    CHECK(list_frame.payload_owner.get() ==
          service::global_workspace_cache().get_dir("demo").get());

    // 2. 首次 read_file 读取前 3 字节，底层整文件写入缓存。
    Frame read_frame1{
        .kind = Kind::request,
        .request_id = 2,
        .method = Method::workspace_read_file,
        .status = Status::ok,
        .payload = encode_read_file_req("demo/note.txt", 0, 3),
    };
    read_frame1.kind = Kind::response;
    server::handle_workspace_read_file(read_frame1);
    CHECK(read_frame1.status == Status::ok);
    CHECK(read_frame1.payload.size() == 16);
    CHECK(read_frame1.payload_size() == 19);
    CHECK(read_frame1.external_payload == "abc");
    CHECK(read_frame1.payload_owner.get() ==
          service::global_workspace_cache().get_file("demo/note.txt").get());

    // 3. 删除磁盘上的目录和文件（不触发任何缓存失效），验证后续访问完全不走 Linux 文件系统。
    std::filesystem::remove_all(proj_dir, ec);
    CHECK(!ec);

    Frame list_hit{
        .kind = Kind::request,
        .request_id = 3,
        .method = Method::workspace_list_dir,
        .status = Status::ok,
        .payload = encode_list_dir_req("demo"),
    };
    server::handle_workspace_list_dir(list_hit);
    CHECK(list_hit.status == Status::ok);
    CHECK(list_hit.payload.empty());
    CHECK(list_hit.external_payload.data() == list_frame.external_payload.data());
    CHECK(list_hit.external_payload == list_frame.external_payload);

    // 命中缓存且使用不同 offset/maxBytes 分段读取仍应返回正确切片 "def"。
    Frame read_hit{
        .kind = Kind::response,
        .request_id = 4,
        .method = Method::workspace_read_file,
        .status = Status::ok,
        .payload = encode_read_file_req("demo/note.txt", 3, 10),
    };
    server::handle_workspace_read_file(read_hit);
    CHECK(read_hit.status == Status::ok);
    CHECK(read_hit.payload.size() == 16);
    CHECK(read_hit.payload_size() == 19);
    CHECK(read_hit.external_payload == "def");
    CHECK(read_hit.external_payload.data() == read_frame1.external_payload.data() + 3);

    Frame read_eof{
        .kind = Kind::response,
        .request_id = 7,
        .method = Method::workspace_read_file,
        .status = Status::ok,
        .payload = encode_read_file_req("demo/note.txt", 100, 10),
    };
    server::handle_workspace_read_file(read_eof);
    CHECK(read_eof.status == Status::ok && read_eof.payload_size() == 16);
    CHECK(!read_eof.payload_owner && read_eof.external_payload.empty());
    CHECK(read_be<std::uint64_t>(read_eof.payload.data()) == 6);

    // 4. 模拟 unwatch 淘汰后，已外借的切片仍然有效，再次访问会回落到 Linux 文件系统并返回 not_found。
    service::global_workspace_cache().invalidate_dir_and_files("demo");
    CHECK(list_hit.external_payload == list_frame.external_payload);
    CHECK(read_frame1.external_payload == "abc");
    CHECK(read_hit.external_payload == "def");

    Frame list_miss{
        .kind = Kind::request,
        .request_id = 5,
        .method = Method::workspace_list_dir,
        .status = Status::ok,
        .payload = encode_list_dir_req("demo"),
    };
    server::handle_workspace_list_dir(list_miss);
    CHECK(list_miss.status == Status::not_found);

    Frame read_miss{
        .kind = Kind::request,
        .request_id = 6,
        .method = Method::workspace_read_file,
        .status = Status::ok,
        .payload = encode_read_file_req("demo/note.txt", 0, 10),
    };
    server::handle_workspace_read_file(read_miss);
    CHECK(read_miss.status == Status::not_found);

    restore_env();
    return 0;
}

} // namespace

int main() {
    if (test_lru_and_invalidation() != 0)
        return 1;
    if (test_file_watcher_independent() != 0)
        return 1;
    if (test_handlers_skip_filesystem_on_hit() != 0)
        return 1;

    std::cout << "workspace_cache test passed\n";
    return 0;
}
