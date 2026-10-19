export module broker.client;

import std;
import rpc.client;
import rpc.protocol;

export import broker.protocol;

export namespace broker {

class BrokerClient {
  public:
    explicit BrokerClient(std::string_view host = "127.0.0.1", std::string_view port = "3490")
        : rpc_(host, port) {
    }

    BrokerClient(const BrokerClient &) = delete;
    BrokerClient &operator=(const BrokerClient &) = delete;

    // 发布消息，返回服务器分配的 Offset。
    // 底层使用异步 RPC，这里等待 Future 完成。
    [[nodiscard]]
    protocol::PublishResult
    publish(const protocol::Publish &request,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {

        auto response = call(rpc::Method::publish, protocol::encode_publish(request), timeout);

        auto result = protocol::decode_publish_result(response.payload);

        if (!result)
            throw std::runtime_error("invalid PUBLISH response");

        return std::move(*result);
    }

    // 从指定 Topic / Partition / Offset 开始拉取。
    // FETCH 不会改变服务器记录的 Consumer Offset，由 commit offset 传递 client 端完成
    [[nodiscard]]
    protocol::FetchResult
    fetch(const protocol::Fetch &request,
          std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {

        auto response = call(rpc::Method::fetch, protocol::encode_fetch(request), timeout);

        // 服务器必须遵守客户端给出的响应大小预算。
        if (response.payload.size() > request.max_bytes)
            throw std::runtime_error("FETCH exceeded byte budget");

        auto result = protocol::decode_fetch_result(response.payload);

        if (!result)
            throw std::runtime_error("invalid FETCH response");

        // 防止错误的服务端结果让客户端跳过消息。
        if (result->records.empty()) {
            if (result->next_offset != request.offset)
                throw std::runtime_error("invalid empty FETCH offset");
        } else if (result->records.front().offset != request.offset) {
            throw std::runtime_error("unexpected first FETCH offset");
        }

        return std::move(*result);
    }

    // 提交 Consumer 的下一次读取位置。
    // 这只是元数据操作，不会删除消息。
    void commit_offset(const protocol::CommitOffset &request,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {

        auto response =
            call(rpc::Method::commit_offset, protocol::encode_commit_offset(request), timeout);

        // 现有协议规定 COMMIT_OFFSET 成功响应为空。
        if (!response.payload.empty())
            throw std::runtime_error("invalid COMMIT_OFFSET response");
    }

  private:
    static std::string_view status_name(rpc::Status status) {
        switch (status) {
        case rpc::Status::ok:
            return "OK";
        case rpc::Status::not_found:
            return "NOT_FOUND";
        case rpc::Status::bad_request:
            return "BAD_REQUEST";
        case rpc::Status::conflict:
            return "CONFLICT";
        case rpc::Status::incomplete:
            return "INCOMPLETE";
        case rpc::Status::io_error:
            return "IO_ERROR";
        case rpc::Status::resource_exhausted:
            return "RESOURCE_EXHAUSTED";
        }

        return "UNKNOWN_STATUS";
    }

    // RPC 超时、断连等异常由原有 RpcClient 直接传播。目前是非异步的
    rpc::Response call(rpc::Method method, std::string payload, std::chrono::milliseconds timeout) {
        auto future = rpc_.async_call(method, std::move(payload), timeout);
        rpc::Response response = future.get();

        if (response.status != rpc::Status::ok) {
            throw std::runtime_error("Broker RPC failed [" +
                                     std::string(status_name(response.status)) +
                                     "]: " + response.payload);
        }

        return response;
    }

    rpc::RpcClient rpc_;
};

} // namespace broker
