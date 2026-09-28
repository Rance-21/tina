import std;

import rpc.client;
import rpc.protocol;
import rpc.server;

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cout << "usage:\n"
                     "  ./tina server\n"
                     "  ./tina ping\n"
                     "  ./tina set <key> <value>\n"
                     "  ./tina get <key>\n"
                     "  ./tina del <key>\n";

        return 0;
    }

    try {
        std::string command = argv[1];

        if (command == "server") {
            rpc::run_server("3490");
            return 0;
        }

        rpc::Response response;

        if (command == "ping") {
            response = rpc::call(rpc::Method::ping, "");
        } else if (command == "set" && argc == 4) {
            std::string payload = std::string{argv[2]} + '\n' + argv[3];

            response = rpc::call(rpc::Method::set, std::move(payload));
        } else if (command == "get" && argc == 3) {
            response = rpc::call(rpc::Method::get, argv[2]);
        } else if (command == "del" && argc == 3) {
            response = rpc::call(rpc::Method::del, argv[2]);
        } else {
            std::cerr << "invalid command\n";
            return 1;
        }

        std::cout << response.payload << '\n';
    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
