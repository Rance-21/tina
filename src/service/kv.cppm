export module service.kv;

import std;
import rpc.protocol;

export namespace service {

class KvService {
  public:
    // 原帧保留请求编号和方法，直接填写响应状态和文本负载。
    void handle(Frame &frame) {
        switch (frame.method) {
        case Method::kv_set: {
            // 只用第一个换行分隔键和值，值中可以包含后续换行。
            const auto separator = frame.payload.find('\n');
            if (separator == std::string::npos) {
                frame.status = Status::invalid_argument;
                frame.payload = "SET requires key and value";
                return;
            }
            data_[frame.payload.substr(0, separator)] = frame.payload.substr(separator + 1);
            frame.payload = "OK";
            return;
        }
        case Method::kv_get: {
            auto it = data_.find(frame.payload);
            if (it != data_.end()) {
                frame.payload = it->second;
                return;
            }
            break;
        }
        case Method::kv_del:
            if (data_.erase(frame.payload)) {
                frame.payload = "OK";
                return;
            }
            break;
        default:
            frame.status = Status::unsupported;
            frame.payload = "unsupported method";
            return;
        }
        frame.status = Status::not_found;
        frame.payload = "NOT_FOUND";
    }

  private:
    std::unordered_map<std::string, std::string> data_;
};

} // namespace service
