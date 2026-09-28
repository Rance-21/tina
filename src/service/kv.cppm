export module service.kv;

import std;
import rpc.protocol;

export namespace service {

class KvService {
  public:
    rpc::Response handle(const rpc::Request &request) {
        switch (request.method) {
        case rpc::Method::ping:
            return {request.request_id, rpc::Status::ok, "PONG"};

        case rpc::Method::set:
            return handle_set(request);

        case rpc::Method::get:
            return handle_get(request);

        case rpc::Method::del:
            return handle_del(request);
        }

        return {request.request_id, rpc::Status::bad_request, "unknown method"};
    }

  private:
    rpc::Response handle_set(const rpc::Request &request) {
        // SET 的 payload 暂时定义为：
        //
        // key\nvalue

        std::size_t separator = request.payload.find('\n');

        if (separator == std::string::npos) {
            return {request.request_id, rpc::Status::bad_request, "SET requires key and value"};
        }

        std::string key = request.payload.substr(0, separator);

        std::string value = request.payload.substr(separator + 1);

        data_[std::move(key)] = std::move(value);

        return {request.request_id, rpc::Status::ok, "OK"};
    }

    rpc::Response handle_get(const rpc::Request &request) {
        auto it = data_.find(request.payload);

        if (it == data_.end()) {
            return {request.request_id, rpc::Status::not_found, "NOT_FOUND"};
        }

        return {request.request_id, rpc::Status::ok, it->second};
    }

    rpc::Response handle_del(const rpc::Request &request) {
        std::size_t count = data_.erase(request.payload);

        if (count == 0) {
            return {request.request_id, rpc::Status::not_found, "NOT_FOUND"};
        }

        return {request.request_id, rpc::Status::ok, "OK"};
    }

    std::unordered_map<std::string, std::string> data_;
};

} // namespace service
