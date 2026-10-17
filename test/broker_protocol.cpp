import std;
import broker.protocol;

namespace bp = broker::protocol;

void check(bool passed, std::string_view name) {
    if (!passed)
        throw std::runtime_error("broker protocol test failed: " + std::string(name));
}

template <typename Fn> void check_rejected(Fn &&fn, std::string_view name) {
    try {
        fn();
    } catch (const std::invalid_argument &) {
        return;
    }

    throw std::runtime_error("broker protocol must reject: " + std::string(name));
}

int main() {
    // 二进制内容，包括 NUL。
    const auto raw = bp::encode_publish({"events", 0, std::string("a\0b", 3)});

    const auto pub = bp::decode_publish(raw);

    check(pub && pub->topic == "events" && pub->partition == 0 &&
              pub->message == std::string("a\0b", 3),
          "publish round trip");

    check(bp::decode_publish(bp::encode_publish({"events", 0, ""})).has_value(), "empty message");

    // 截断、额外数据、非法名称。
    check(!bp::decode_publish(raw.substr(0, raw.size() - 1)), "truncated publish");

    check(!bp::decode_publish(raw + "x"), "trailing publish data");

    check_rejected(
        [] {
            (void)bp::encode_publish({"../secret", 0, "x"});
        },
        "invalid topic");

    // 消息大小边界。
    check_rejected(
        [] {
            (void)bp::encode_publish({"events", 0, std::string(bp::max_message_size + 1, 'x')});
        },
        "oversized message");

    const auto boundary = bp::encode_publish({"events", 0, std::string(bp::max_message_size, 'x')});

    check(bp::decode_publish(boundary).has_value(), "max sized message");

    // 网络字节序验证。
    const auto raw_fetch = bp::encode_fetch({"events", 2, 0x0102030405060708ULL, 1024});

    check(static_cast<unsigned char>(raw_fetch[12]) == 0x01 &&
              static_cast<unsigned char>(raw_fetch[19]) == 0x08,
          "network byte order");

    const auto fetch = bp::decode_fetch(raw_fetch);

    check(fetch && fetch->partition == 2 && fetch->offset == 0x0102030405060708ULL &&
              fetch->max_bytes == 1024,
          "fetch round trip");

    check(!bp::decode_fetch(raw_fetch.substr(0, raw_fetch.size() - 1)), "truncated fetch");

    check_rejected(
        [] {
            (void)bp::encode_fetch({"events", 0, 0, 0});
        },
        "zero fetch budget");

    // Consumer Offset。
    const auto commit =
        bp::decode_commit_offset(bp::encode_commit_offset({"group_1", "events", 0, 3}));

    check(commit && commit->consumer == "group_1" && commit->next_offset == 3,
          "commit offset round trip");

    // 第一条消息的 Offset 可以是 0。
    const auto published = bp::decode_publish_result(bp::encode_publish_result({0}));

    check(published && published->offset == 0, "first publish offset is zero");

    // 多条 FETCH 结果。
    const auto raw_result = bp::encode_fetch_result({2, {{0, "hello"}, {1, ""}}});

    const auto result = bp::decode_fetch_result(raw_result);

    check(result && result->next_offset == 2 && result->records.size() == 2 &&
              result->records[0].message == "hello" && result->records[1].message.empty(),
          "fetch result round trip");

    check(!bp::decode_fetch_result(raw_result.substr(0, raw_result.size() - 1)),
          "truncated fetch result");

    check_rejected(
        [] {
            (void)bp::encode_fetch_result({3, {{0, "x"}, {1, "y"}}});
        },
        "nonmatching next offset");

    check_rejected(
        [] {
            (void)bp::encode_fetch_result({3, {{0, "x"}, {2, "y"}}});
        },
        "noncontiguous offsets");

    std::cout << "broker protocol: all tests passed\n";
}