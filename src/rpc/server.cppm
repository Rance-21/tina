export module tina.rpc.server;

import std;
import tina.kv.service;
import tina.net.socket;
import tina.rpc.connection;

export namespace tina::rpc
{

void run_server(std::string_view port)
{
    int listen_fd = net::listen_tcp(port);
    kv::Service service;

    std::cout << "tina: listening on port " << port << '\n';

    while (true)
    {
        int client_fd = -1;

        try
        {
            // v0.1：这里是 blocking accept()。
            client_fd = net::accept_client(listen_fd);
            std::cout << "tina: client connected\n";

            serve_connection(client_fd, service);

            std::cout << "tina: client disconnected\n";
        }
        catch (const std::exception& error)
        {
            std::cerr << "connection error: " << error.what() << '\n';
        }

        net::close_socket(client_fd);
    }
}

}
