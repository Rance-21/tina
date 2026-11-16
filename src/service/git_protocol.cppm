export module service.git:protocol;

import std;
import ide.protocol;

export namespace service::git {

// ============================================================================
// 4. Git 输出单遍直编器（从 net::Buffer 的 string_view 直接写入
// Frame::payload，零中间结构体/字符串拷贝）
// ============================================================================

// 单遍解析 `git status --porcelain=v1 -z --untracked-files=all` 的输出，
// 并直接编码追加进响应 `out`（即 Frame::payload）：
//   - 不构造中间 std::vector<GitStatusEntry>，不分配任何临时 std::string；
//   - 每条记录 `<X><Y> <PATH>\0` 直接切片后编码为 `[u8 X][u8 Y][u16 path_len][path bytes]`；
//   - 遇到重命名（R）或复制（C）时，自动跳过紧随其后的 `<ORIG_PATH>\0` 字段。
void encode_status_porcelain_z_into(std::string_view raw, std::string &out) {
    out.clear();
    out.reserve(raw.size());

    std::size_t pos = 0;
    while (pos < raw.size()) {
        const std::size_t nul = raw.find('\0', pos);
        if (nul == std::string_view::npos)
            break;

        std::string_view token = raw.substr(pos, nul - pos);
        pos = nul + 1;

        if (token.size() < 4 || token.starts_with("## ") || token[2] != ' ')
            continue;

        const char x = token[0];
        const char y = token[1];
        std::string_view path = token.substr(3);

        // 若是重命名（R）或复制（C），紧接着的下一个 NUL
        // 字段是原路径（orig_path），将其跳过保持流同步。
        if (x == 'R' || x == 'C' || y == 'R' || y == 'C') {
            const std::size_t orig_nul = raw.find('\0', pos);
            pos = (orig_nul != std::string_view::npos) ? (orig_nul + 1) : raw.size();
        }

        if (!path.empty() && path.size() <= std::numeric_limits<std::uint16_t>::max()) {
            append_be(out, static_cast<std::uint8_t>(x));
            append_be(out, static_cast<std::uint8_t>(y));
            append_be(out, static_cast<std::uint16_t>(path.size()));
            out.append(path);
        }
    }
}

// 单遍解析 `git log --all --topo-order -n 100 -z
// --pretty=format:%h%x1f%p%x1f%D%x1f%an%x1f%ct%x1f%s` 的输出， 并直接编码写入响应 `out`（即
// Frame::payload）：
//   - 全程只用 string_view 在 net::Buffer 上原地切片，直接序列化进 out；
//   - 零 std::vector<GitCommit> 分配、零 std::vector<std::string> parents 分配、零临时 std::string
//   拷贝；
//   - 解析父提交列表时，先在 out 中预留 1 字节 parent_count
//   槽位，扫描完所有父哈希后再原地回填计数。
void encode_log_z_into(std::string_view raw, std::string &out) {
    out.clear();
    out.reserve(raw.size() + 256);

    std::size_t pos = 0;
    std::uint32_t commit_count = 0;

    while (pos < raw.size() && commit_count < git_log_limit) {
        const std::size_t nul = raw.find('\0', pos);
        std::string_view record =
            (nul == std::string_view::npos) ? raw.substr(pos) : raw.substr(pos, nul - pos);
        pos = (nul == std::string_view::npos) ? raw.size() : (nul + 1);

        while (!record.empty() && (record.front() == '\n' || record.front() == '\r'))
            record.remove_prefix(1);
        if (record.empty())
            continue;

        // 按 '\x1f' 切分 6 个字段视图：[0]hash [1]parents [2]refs [3]author [4]timestamp [5]subject
        std::array<std::string_view, 6> fields{};
        std::size_t f_pos = 0;
        bool valid = true;
        for (std::size_t i = 0; i < 5; ++i) {
            const std::size_t sep = record.find('\x1f', f_pos);
            if (sep == std::string_view::npos) {
                valid = false;
                break;
            }
            fields[i] = record.substr(f_pos, sep - f_pos);
            f_pos = sep + 1;
        }
        if (!valid)
            continue;
        fields[5] = record.substr(f_pos);

        const std::string_view hash = fields[0];
        if (hash.empty() || hash.size() > std::numeric_limits<std::uint8_t>::max())
            continue;

        // 1. 写入 [u8 hash_len][hash]
        append_be(out, static_cast<std::uint8_t>(hash.size()));
        out.append(hash);

        // 2. 先在 out 中占位 1 字节 parent_count，随后原地扫描空格分隔的父哈希直接写入
        // out，最后回填计数。
        const std::size_t parent_count_offset = out.size();
        out.push_back('\0');
        std::uint8_t parent_count = 0;

        const std::string_view parents_raw = fields[1];
        std::size_t p_start = 0;
        while (p_start < parents_raw.size()) {
            while (p_start < parents_raw.size() && parents_raw[p_start] == ' ')
                ++p_start;
            if (p_start >= parents_raw.size())
                break;

            const std::size_t p_end = parents_raw.find(' ', p_start);
            const std::string_view phash = (p_end == std::string_view::npos)
                                               ? parents_raw.substr(p_start)
                                               : parents_raw.substr(p_start, p_end - p_start);
            if (!phash.empty() && phash.size() <= std::numeric_limits<std::uint8_t>::max() &&
                parent_count < std::numeric_limits<std::uint8_t>::max()) {
                append_be(out, static_cast<std::uint8_t>(phash.size()));
                out.append(phash);
                ++parent_count;
            }
            if (p_end == std::string_view::npos)
                break;
            p_start = p_end + 1;
        }
        out[parent_count_offset] = static_cast<char>(parent_count);

        // 3. 写入 [u16 refs_len][refs] 与 [u16 author_len][author]（超出 u16 上限时直接截断视图）
        constexpr std::size_t u16_max = std::numeric_limits<std::uint16_t>::max();
        const std::string_view refs = fields[2].substr(0, std::min(fields[2].size(), u16_max));
        const std::string_view author = fields[3].substr(0, std::min(fields[3].size(), u16_max));
        const std::string_view subject = fields[5].substr(0, std::min(fields[5].size(), u16_max));

        append_be(out, static_cast<std::uint16_t>(refs.size()));
        out.append(refs);

        append_be(out, static_cast<std::uint16_t>(author.size()));
        out.append(author);

        // 4. 解析并写入 [u64 timestamp]
        std::uint64_t timestamp = 0;
        (void)std::from_chars(fields[4].data(), fields[4].data() + fields[4].size(), timestamp);
        append_be(out, timestamp);

        // 5. 写入 [u16 subject_len][subject]
        append_be(out, static_cast<std::uint16_t>(subject.size()));
        out.append(subject);

        ++commit_count;
    }
}

}
