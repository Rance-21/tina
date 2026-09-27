import std;

import tina.rpc.client;
import tina.rpc.protocol;
import tina.rpc.server;

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cout
            << "usage:\n"
            << "  ./tina server\n"
            << "  ./tina ping\n"
            << "  ./tina set <key> <value>\n"
            << "  ./tina get <key>\n"
            << "  ./tina del <key>\n";

        return 0;
    }

    try
    {
        std::string command = argv[1];

        if (command == "server")
        {
            tina::rpc::run_server("3490");
            return 0;
        }

        tina::rpc::Response response;

        if (command == "ping")
        {
            response = tina::rpc::call(
                tina::rpc::Method::ping,
                ""
            );
        }
        else if (command == "set" && argc == 4)
        {
            std::string payload =
                std::string{argv[2]} + '\n' + argv[3];

            response = tina::rpc::call(
                tina::rpc::Method::set,
                std::move(payload)
            );
        }
        else if (command == "get" && argc == 3)
        {
            response = tina::rpc::call(
                tina::rpc::Method::get,
                argv[2]
            );
        }
        else if (command == "del" && argc == 3)
        {
            response = tina::rpc::call(
                tina::rpc::Method::del,
                argv[2]
            );
        }
        else
        {
            std::cerr << "invalid command\n";
            return 1;
        }

        std::cout << response.payload << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
