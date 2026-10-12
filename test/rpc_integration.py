import socket
import struct
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HOST = "127.0.0.1"
TIMEOUT = 2.0

def connect(port):
    return socket.create_connection((HOST, port), timeout=TIMEOUT)


# RPC Frame: length(4) + request_id(4) + method(1) + payload
def frame(request_id, method, payload=b""):
    return struct.pack("!IIB", len(payload) + 5,
                       request_id, method) + payload


def recv_exact(sock, count):
    data = bytearray()

    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise AssertionError("unexpected EOF")
        data.extend(chunk)

    return bytes(data)

def recv_response(sock):
    length, = struct.unpack("!I", recv_exact(sock, 4))
    assert 5 <= length <= 1024 * 1024 + 5

    body = recv_exact(sock, length)
    request_id, status = struct.unpack("!IB", body[:5])

    return request_id, status, body[5:]


def expect_no_response(sock):
    sock.settimeout(0.1)
    try:
        data = sock.recv(1)
    except socket.timeout:
        return
    finally:
        sock.settimeout(TIMEOUT)

    raise AssertionError(f"unexpected data or EOF: {data!r}")


def expect_closed(sock):
    try:
        data = sock.recv(1)
    except ConnectionResetError:
        return

    assert data == b"", f"connection not closed: {data!r}"


def test_fragmentation(port):
    with connect(port) as sock:
        request = frame(101, 2, b"frag-key\nhello")

        # 故意将一个请求分成三次发送。
        sock.sendall(request[:2])
        expect_no_response(sock)

        sock.sendall(request[2:11])
        expect_no_response(sock)

        sock.sendall(request[11:])
        assert recv_response(sock) == (101, 0, b"OK")

        # 验证刚才的 SET 确实生效。
        sock.sendall(frame(102, 3, b"frag-key"))
        assert recv_response(sock) == (102, 0, b"hello")


def test_pipelining(port):
    with connect(port) as sock:
        ids = range(201, 217)

        # 不等待响应，一次性连续发送 16 个请求。
        requests = b"".join(frame(i, 1) for i in ids)
        sock.sendall(requests)

        received = {}

        for _ in ids:
            request_id, status, payload = recv_response(sock)
            assert request_id not in received
            received[request_id] = (status, payload)

        # Worker 并发执行，不能假定响应顺序。
        assert received == {
            i: (0, b"PONG") for i in ids
        }


def test_invalid_frames(port):
    for invalid_length in (4, 1024 * 1024 + 6):
        with connect(port) as sock:
            # 只发送非法长度字段。
            sock.sendall(struct.pack("!I", invalid_length))
            expect_closed(sock)

    # 错误连接不能导致整个 Server 崩溃。
    with connect(port) as sock:
        sock.sendall(frame(301, 1))
        assert recv_response(sock) == (301, 0, b"PONG")


def test_half_close(port):
    with connect(port) as sock:
        sock.sendall(frame(401, 1))

        # 客户端不再发送，但仍保留接收方向。
        sock.shutdown(socket.SHUT_WR)

        assert recv_response(sock) == (401, 0, b"PONG")
        expect_closed(sock)


def test_concurrent_clients(port):
    def client(index):
        key = f"concurrent-{index}".encode()
        value = f"value-{index}".encode()

        with connect(port) as sock:
            sock.sendall(frame(501, 2, key + b"\n" + value))
            assert recv_response(sock) == (501, 0, b"OK")

            sock.sendall(frame(502, 3, key))
            assert recv_response(sock) == (502, 0, value)

    with ThreadPoolExecutor(max_workers=16) as pool:
        list(pool.map(client, range(16)))


def find_free_port():
    with socket.socket() as sock:
        sock.bind((HOST, 0))
        return sock.getsockname()[1]


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: rpc_integration.py <tina binary>")

    port = find_free_port()
    server = subprocess.Popen(
        [sys.argv[1], "server", str(port)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    try:
        # 最多等待 3 秒，让 Server 开始监听。
        deadline = time.monotonic() + 3

        while time.monotonic() < deadline:
            if server.poll() is not None:
                raise RuntimeError("server exited during startup")

            try:
                with connect(port):
                    break
            except OSError:
                time.sleep(0.03)
        else:
            raise RuntimeError("server startup timed out")

        tests = [
            ("fragmentation", test_fragmentation),
            ("pipelining", test_pipelining),
            ("invalid frames", test_invalid_frames),
            ("half close", test_half_close),
            ("concurrent clients", test_concurrent_clients),
        ]

        for name, test in tests:
            test(port)
            print(f"PASS: {name}", flush=True)

    finally:
        # 目前 Server 还没有优雅退出机制。
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == "__main__":
    main()