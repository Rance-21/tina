import std;
import rpc.protocol;
import service.kv;

// 检查响应是否符合预期，否则抛出异常。
void expect(std::string_view name, const rpc::Response &response, std::uint32_t id,
            rpc::Status status, std::string_view payload) {
    if (response.request_id != id || response.status != status || response.payload != payload) {
        throw std::runtime_error("FAILED: " + std::string{name});
    }
}

int main() {
    try {
        service::KvService kv;

        // 1. PING
        expect("ping", kv.handle({1, rpc::Method::ping, ""}), 1, rpc::Status::ok, "PONG");

        // 2. GET 不存在的 key
        expect("get missing", kv.handle({2, rpc::Method::get, "name"}), 2, rpc::Status::not_found,
               "NOT_FOUND");

        // 3. SET
        expect("set", kv.handle({3, rpc::Method::set, "name\nAlice"}), 3, rpc::Status::ok, "OK");

        // 4. GET 已有的 key
        expect("get", kv.handle({4, rpc::Method::get, "name"}), 4, rpc::Status::ok, "Alice");

        // 5. SET 覆盖原来的值
        expect("overwrite", kv.handle({5, rpc::Method::set, "name\nBob"}), 5, rpc::Status::ok,
               "OK");

        expect("get overwritten", kv.handle({6, rpc::Method::get, "name"}), 6, rpc::Status::ok,
               "Bob");

        // 6. DEL
        expect("delete", kv.handle({7, rpc::Method::del, "name"}), 7, rpc::Status::ok, "OK");

        expect("get deleted", kv.handle({8, rpc::Method::get, "name"}), 8, rpc::Status::not_found,
               "NOT_FOUND");

        // 7. 非法 SET 请求：缺少换行分隔符
        expect("invalid set", kv.handle({9, rpc::Method::set, "invalid"}), 9,
               rpc::Status::bad_request, "SET requires key and value");

        // 8. 未知方法
        expect("unknown method", kv.handle({10, static_cast<rpc::Method>(99), ""}), 10,
               rpc::Status::bad_request, "unknown method");

        std::cout << "KV tests passed\n";
        return 0;

    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
