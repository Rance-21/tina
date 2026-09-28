export module rpc.connection;

import kv.service;
import rpc.protocol;

export namespace rpc {

// 处理一条已经建立好的 TCP 连接。
// v0.1 是 blocking：没有完整请求时会停在 recv()。
void serve_connection(int client_fd, kv::Service &service) {
    Request request;

    while (recv_request(client_fd, request)) {
        Response response = service.handle(request);
        send_response(client_fd, response);
    }
}

} // namespace rpc
