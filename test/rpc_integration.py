import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

HOST = "127.0.0.1"
TIMEOUT = 2.0

def connect(port):
    return socket.create_connection((HOST, port), timeout=TIMEOUT)


# 大端序的统一帧头：Length、Kind、Request ID、Method、Status。
def frame(request_id, method, payload=b"", *, kind=1, status=0):
    return struct.pack("!IBQHH", 13 + len(payload), kind, request_id, method, status) + payload


def recv_exact(sock, count):
    data = bytearray()

    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise AssertionError("unexpected EOF")
        data.extend(chunk)

    return bytes(data)

def recv_response(sock, expected_method):
    length, = struct.unpack("!I", recv_exact(sock, 4))
    assert 13 <= length <= 1024 * 1024
    kind, request_id, method, status = struct.unpack("!BQHH", recv_exact(sock, 13))
    assert kind == 2 and method == expected_method
    return request_id, status, recv_exact(sock, length - 13)


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
        assert recv_response(sock, 2) == (101, 0, b"OK")

        # 验证刚才的 SET 确实生效。
        sock.sendall(frame(102, 3, b"frag-key"))
        assert recv_response(sock, 3) == (102, 0, b"hello")


def test_pipelining(port):
    with connect(port) as sock:
        ids = range(2**48, 2**48 + 16)

        # 不等待响应，一次性连续发送 16 个请求。
        requests = b"".join(frame(i, 1) for i in ids)
        sock.sendall(requests)

        received = {}

        for _ in ids:
            request_id, status, payload = recv_response(sock, 1)
            assert request_id not in received
            received[request_id] = (status, payload)

        # Worker 并发执行，不能假定响应顺序。
        assert received == {
            i: (0, b"PONG") for i in ids
        }


def test_kv_dispatch(port):
    with connect(port) as sock:
        # 同一条连接先发 ping，再发 KV 请求。业务错误只影响对应请求。
        sock.sendall(frame(600, 1))
        assert recv_response(sock, 1) == (600, 0, b"PONG")
        sock.sendall(frame(607, 1, b"x"))
        assert recv_response(sock, 1) == (607, 1, b"ping requires an empty payload")
        sock.sendall(frame(601, 0x0099))
        assert recv_response(sock, 0x0099) == (601, 8, b"unsupported method")
        sock.sendall(frame(602, 2, b"no-separator"))
        assert recv_response(sock, 2) == (602, 1, b"SET requires key and value")

        sock.sendall(frame(603, 2, b"del-key\nvalue"))
        assert recv_response(sock, 2) == (603, 0, b"OK")
        sock.sendall(frame(604, 4, b"del-key"))
        assert recv_response(sock, 4) == (604, 0, b"OK")
        sock.sendall(frame(605, 3, b"del-key"))
        assert recv_response(sock, 3) == (605, 4, b"NOT_FOUND")
        sock.sendall(frame(606, 4, b"del-key"))
        assert recv_response(sock, 4) == (606, 4, b"NOT_FOUND")


def test_workspace_and_inotify(port, projects_root):
    proj_dir = os.path.join(projects_root, "demo")
    os.makedirs(os.path.join(proj_dir, "sub"), exist_ok=True)
    file_path = os.path.join(proj_dir, "a.txt")
    with open(file_path, "wb") as f:
        f.write(b"hello")

    with connect(port) as sock:
        # 1. workspace_list_projects (0x0011)
        sock.sendall(frame(701, 0x0011))
        req_id, status, payload = recv_response(sock, 0x0011)
        assert req_id == 701 and status == 0
        assert payload == struct.pack("!H", 4) + b"demo"

        # 2. workspace_list_dir (0x0013)
        dir_req = struct.pack("!H", 4) + b"demo"
        sock.sendall(frame(702, 0x0013, dir_req))
        req_id, status, payload = recv_response(sock, 0x0013)
        assert req_id == 702 and status == 0
        expected_dir_payload = (
            struct.pack("!H", 4) + b"demo"
            + struct.pack("!BH", 0, 5) + b"a.txt"
            + struct.pack("!BH", 1, 3) + b"sub"
        )
        assert payload == expected_dir_payload

        # 3. workspace_read_file (0x0014)
        rel_file = b"demo/a.txt"
        read_req = struct.pack("!H", len(rel_file)) + rel_file + struct.pack("!QI", 0, 1024)
        sock.sendall(frame(703, 0x0014, read_req))
        req_id, status, payload = recv_response(sock, 0x0014)
        assert req_id == 703 and status == 0
        file_size, rev1 = struct.unpack("!QQ", payload[:16])
        assert file_size == 5 and payload[16:] == b"hello"

        # 4. workspace_write_file (0x0015) 成功写入 + 版本冲突检测
        write_req = struct.pack("!H", len(rel_file)) + rel_file + struct.pack("!Q", rev1) + b"world!"
        sock.sendall(frame(704, 0x0015, write_req))
        req_id, status, payload = recv_response(sock, 0x0015)
        assert req_id == 704 and status == 0
        rev2, = struct.unpack("!Q", payload)
        assert rev2 != rev1

        # 用旧的 rev1 再次写，应当返回 Status::conflict (6) 以及当前的 rev2
        sock.sendall(frame(705, 0x0015, write_req))
        req_id, status, payload = recv_response(sock, 0x0015)
        assert req_id == 705 and status == 6
        conflict_rev, = struct.unpack("!Q", payload)
        assert conflict_rev == rev2

        # 5. 在已 list_dir 的目录下新建文件，验证服务端通过 inotify 主动推送 Kind::event (kind=3)
        with open(os.path.join(proj_dir, "b.txt"), "wb") as f:
            f.write(b"new")

        length, = struct.unpack("!I", recv_exact(sock, 4))
        kind, event_req_id, method, status = struct.unpack("!BQHH", recv_exact(sock, 13))
        event_payload = recv_exact(sock, length - 13)
        assert kind == 3 and event_req_id == 0 and method == 0x0013 and status == 0
        assert (struct.pack("!BH", 0, 5) + b"b.txt") in event_payload


def test_invalid_frames(port):
    for packet in (struct.pack("!I", 12), struct.pack("!I", 1024 * 1024 + 1),
                   frame(0, 1), frame(1, 1, status=1), frame(1, 1, kind=2)):
        with connect(port) as sock:
            sock.sendall(packet)
            expect_closed(sock)

    # 错误连接不能导致整个 Server 崩溃。
    with connect(port) as sock:
        sock.sendall(frame(301, 1))
        assert recv_response(sock, 1) == (301, 0, b"PONG")


def test_half_close(port):
    with connect(port) as sock:
        sock.sendall(frame(401, 1))

        # 客户端不再发送，但仍保留接收方向。
        sock.shutdown(socket.SHUT_WR)

        assert recv_response(sock, 1) == (401, 0, b"PONG")
        expect_closed(sock)


def test_concurrent_clients(port):
    def client(index):
        key = f"concurrent-{index}".encode()
        value = f"value-{index}".encode()

        with connect(port) as sock:
            sock.sendall(frame(501, 2, key + b"\n" + value))
            assert recv_response(sock, 2) == (501, 0, b"OK")

            sock.sendall(frame(502, 3, key))
            assert recv_response(sock, 3) == (502, 0, value)

    with ThreadPoolExecutor(max_workers=16) as pool:
        list(pool.map(client, range(16)))


def find_free_port():
    with socket.socket() as sock:
        sock.bind((HOST, 0))
        return sock.getsockname()[1]


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: rpc_integration.py <tina binary>")

    with tempfile.TemporaryDirectory() as tmp_home:
        projects_root = os.path.join(tmp_home, "projects")
        os.makedirs(projects_root, exist_ok=True)
        env = os.environ.copy()
        env["HOME"] = tmp_home

        port = find_free_port()
        server = subprocess.Popen(
            [sys.argv[1], "server", str(port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=env,
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
                ("KV dispatch", test_kv_dispatch),
                ("workspace & inotify", lambda p: test_workspace_and_inotify(p, projects_root)),
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
