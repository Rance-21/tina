import std;
import net.buffer;

int main() {
    net::Buffer buffer;

    // 1. 追加数据后，长度应当是 5。
    buffer.append("hello", 5);

    if (buffer.readable_bytes() != 5) {
        std::cerr << "append failed\n";
        return 1;
    }

    // 2. 消费 2 字节，剩余内容应当是 "llo"。
    buffer.retrieve(2);

    std::string_view remaining(buffer.data(), buffer.readable_bytes());

    if (remaining != "llo") {
        std::cerr << "retrieve failed\n";
        return 1;
    }

    // 3. 消费所有数据后，Buffer 应为空。
    buffer.retrieve(3);

    if (!buffer.empty()) {
        std::cerr << "buffer should be empty\n";
        return 1;
    }

    std::cout << "buffer test passed\n";
    return 0;
}
