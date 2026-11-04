import std;
import rpc.protocol;

namespace wb = rpc::workbench;
using wb::Kind;
using wb::Method;
using wb::Status;
using std::to_underlying;

static_assert(std::is_same_v<std::underlying_type_t<Kind>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<Method>, std::uint16_t>);
static_assert(std::is_same_v<std::underlying_type_t<Status>, std::uint16_t>);
static_assert(std::is_same_v<decltype(wb::Frame::length), std::uint32_t>);
static_assert(std::is_same_v<decltype(wb::Frame::request_id), std::uint64_t>);
static_assert(to_underlying(Kind::request) == 1);
static_assert(to_underlying(Kind::response) == 2);
static_assert(to_underlying(Kind::event) == 3);

static_assert(to_underlying(Method::system_ping) == 0x0001);
static_assert(to_underlying(Method::workspace_list_projects) == 0x0011);
static_assert(to_underlying(Method::workspace_open) == 0x0012);
static_assert(to_underlying(Method::workspace_list_dir) == 0x0013);
static_assert(to_underlying(Method::workspace_read_file) == 0x0014);
static_assert(to_underlying(Method::workspace_write_file) == 0x0015);
static_assert(to_underlying(Method::terminal_input) == 0x0021);
static_assert(to_underlying(Method::terminal_resize) == 0x0022);
static_assert(to_underlying(Method::terminal_output) == 0x0281);
static_assert(to_underlying(Method::terminal_state) == 0x0282);
static_assert(to_underlying(Method::git_status) == 0x0031);
static_assert(to_underlying(Method::git_diff) == 0x0032);
static_assert(to_underlying(Method::agent_list) == 0x0401);
static_assert(to_underlying(Method::agent_start) == 0x0402);
static_assert(to_underlying(Method::agent_input) == 0x0403);
static_assert(to_underlying(Method::agent_cancel) == 0x0404);
static_assert(to_underlying(Method::agent_close) == 0x0405);
static_assert(to_underlying(Method::agent_output) == 0x0481);
static_assert(to_underlying(Method::agent_state) == 0x0482);

static_assert(to_underlying(Status::ok) == 0);
static_assert(to_underlying(Status::invalid_argument) == 1);
static_assert(to_underlying(Status::unauthenticated) == 2);
static_assert(to_underlying(Status::permission_denied) == 3);
static_assert(to_underlying(Status::not_found) == 4);
static_assert(to_underlying(Status::already_exists) == 5);
static_assert(to_underlying(Status::conflict) == 6);
static_assert(to_underlying(Status::resource_exhausted) == 7);
static_assert(to_underlying(Status::unsupported) == 8);
static_assert(to_underlying(Status::canceled) == 9);
static_assert(to_underlying(Status::internal) == 10);
static_assert(to_underlying(Status::unavailable) == 11);
static_assert(to_underlying(Status::timeout) == 12);

static_assert(wb::min_frame_length == 13);
static_assert(wb::frame_header_size == 17);
static_assert(wb::max_frame_length == 1024 * 1024);
static_assert(wb::max_file_read_bytes == 256 * 1024);
static_assert(wb::max_terminal_output_bytes == 64 * 1024);

int main() {
    // 空 ping、其 JSON 响应，以及使用空 JSON 对象的项目列表请求。
    constexpr std::uint64_t id = 0x1'0000'0001;
    const wb::Frame ping{13, Kind::request, id, Method::system_ping, Status::ok, ""};
    const wb::Frame pong{24, Kind::response, ping.request_id, ping.method, Status::ok,
                         R"({"ok":true})"};
    const wb::Frame list{15, Kind::request, id + 1, Method::workspace_list_projects, Status::ok,
                         "{}"};
    const wb::Frame event{32, Kind::event, 0, Method::terminal_state, Status::ok,
                          R"({"state":"running"})"};
    for (const auto *frame : {&ping, &pong, &list, &event}) {
        if (frame->length != wb::min_frame_length + frame->payload.size())
            throw std::runtime_error("Length must exclude its own four bytes");
    }
    if (pong.request_id != id || event.request_id != 0)
        throw std::runtime_error("invalid correlation id");

    // 用同一目录中的目录项和文件项检查可选 size，用多字节文本检查文件偏移。
    const wb::workspace::ListDirResponse dir{.entries = {
        {.name = "rpc", .path = "src/rpc", .type = "directory"},
        {.name = "main.cpp", .path = "src/main.cpp", .type = "file", .size = 2048},
    }};
    const wb::workspace::ReadFileRequest read{.workspaceId = "tina", .path = "src/main.cpp"};
    const wb::workspace::ReadFileResponse chunk{
        .offset = 0, .nextOffset = 3, .totalBytes = 3, .eof = true, .content = "好"};
    if (dir.entries[0].size || dir.entries[1].size != 2048 ||
        read.maxBytes != wb::max_file_read_bytes ||
        chunk.nextOffset - chunk.offset != chunk.content.size())
        throw std::runtime_error("workspace payload mismatch");

    const wb::terminal::OutputEvent output{.sequence = 1024, .text = "Build finished\n"};
    const wb::terminal::OutputEvent snapshot{.sequence = 1024, .text = output.text, .snapshot = true};
    const wb::git::DiffRequest diff{
        .workspaceId = "tina", .path = "src/main.cpp", .scope = wb::git::DiffScope::worktree};
    const wb::agent::StateEvent state{.sessionId = "agent-001", .state = wb::agent::State::busy};
    if (output.snapshot || snapshot.snapshot != true || diff.scope != wb::git::DiffScope::worktree ||
        state.state != wb::agent::State::busy)
        throw std::runtime_error("workbench payload mismatch");
}
