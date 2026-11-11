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


def test_dual_stack(port):
    # 先检查本机是否启用了 IPv6；未启用时仍由后续用例验证 IPv4 回退。
    try:
        with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as probe:
            probe.bind(("::1", 0))
    except OSError:
        print("SKIP: IPv6 loopback unavailable", flush=True)
        return

    # 同一个服务端端口必须同时接受 IPv4 和 IPv6，避免只有 AAAA 的域名无法连接。
    for host in (HOST, "::1"):
        with socket.create_connection((host, port), timeout=TIMEOUT) as sock:
            sock.sendall(frame(100, 1))
            assert recv_response(sock, 1) == (100, 0, b"PONG")


def test_fragmentation(port):
    with connect(port) as sock:
        request = frame(101, 0x0101, b"frag-key\nhello")

        # 故意将一个请求分成三次发送。
        sock.sendall(request[:2])
        expect_no_response(sock)

        sock.sendall(request[2:11])
        expect_no_response(sock)

        sock.sendall(request[11:])
        assert recv_response(sock, 0x0101) == (101, 0, b"OK")

        # 验证刚才的 SET 确实生效。
        sock.sendall(frame(102, 0x0102, b"frag-key"))
        assert recv_response(sock, 0x0102) == (102, 0, b"hello")


def test_pipelining(port):
    with connect(port) as sock:
        ids = range(2**48, 2**48 + 16)

        # 不等待响应，一次性连续发送 16 个请求。
        requests = b"".join(frame(i, 0x0001) for i in ids)
        sock.sendall(requests)

        received = {}

        for _ in ids:
            request_id, status, payload = recv_response(sock, 0x0001)
            assert request_id not in received
            received[request_id] = (status, payload)

        # Worker 并发执行，不能假定响应顺序。
        assert received == {
            i: (0, b"PONG") for i in ids
        }


def test_kv_dispatch(port):
    with connect(port) as sock:
        # 同一条连接先发 ping，再发 KV 请求。业务错误只影响对应请求。
        sock.sendall(frame(600, 0x0001))
        assert recv_response(sock, 0x0001) == (600, 0, b"PONG")
        sock.sendall(frame(607, 0x0001, b"x"))
        assert recv_response(sock, 0x0001) == (607, 1, b"ping requires an empty payload")
        sock.sendall(frame(601, 0x0099))
        assert recv_response(sock, 0x0099) == (601, 8, b"unsupported method")
        sock.sendall(frame(608, 0x0403))  # git_log (0x0403) 尚未接入处理函数，返回 unsupported
        assert recv_response(sock, 0x0403) == (608, 8, b"unsupported method")
        sock.sendall(frame(602, 0x0101, b"no-separator"))
        assert recv_response(sock, 0x0101) == (602, 1, b"SET requires key and value")

        sock.sendall(frame(603, 0x0101, b"del-key\nvalue"))
        assert recv_response(sock, 0x0101) == (603, 0, b"OK")
        sock.sendall(frame(604, 0x0103, b"del-key"))
        assert recv_response(sock, 0x0103) == (604, 0, b"OK")
        sock.sendall(frame(605, 0x0102, b"del-key"))
        assert recv_response(sock, 0x0102) == (605, 4, b"NOT_FOUND")
        sock.sendall(frame(606, 0x0103, b"del-key"))
        assert recv_response(sock, 0x0103) == (606, 4, b"NOT_FOUND")


def test_workspace_and_inotify(port, projects_root):
    proj_dir = os.path.join(projects_root, "demo")
    os.makedirs(os.path.join(proj_dir, "sub"), exist_ok=True)
    file_path = os.path.join(proj_dir, "a.txt")
    with open(file_path, "wb") as f:
        f.write(b"hello")

    with connect(port) as sock:
        # 1. workspace_list_projects (0x0201)
        sock.sendall(frame(701, 0x0201))
        req_id, status, payload = recv_response(sock, 0x0201)
        assert req_id == 701 and status == 0
        assert payload == struct.pack("!H", 4) + b"demo"

        # 2. workspace_list_dir (0x0202)
        dir_req = struct.pack("!H", 4) + b"demo"
        sock.sendall(frame(702, 0x0202, dir_req))
        req_id, status, payload = recv_response(sock, 0x0202)
        assert req_id == 702 and status == 0
        expected_dir_payload = (
            struct.pack("!H", 4) + b"demo"
            + struct.pack("!BH", 0, 5) + b"a.txt"
            + struct.pack("!BH", 1, 3) + b"sub"
        )
        assert payload == expected_dir_payload

        # 3. workspace_read_file (0x0203)
        rel_file = b"demo/a.txt"
        read_req = struct.pack("!H", len(rel_file)) + rel_file + struct.pack("!QI", 0, 1024)
        sock.sendall(frame(703, 0x0203, read_req))
        req_id, status, payload = recv_response(sock, 0x0203)
        assert req_id == 703 and status == 0
        file_size, rev1 = struct.unpack("!QQ", payload[:16])
        assert file_size == 5 and payload[16:] == b"hello"

        # 4. workspace_write_file (0x0204) 成功写入 + 版本冲突检测
        write_req = struct.pack("!H", len(rel_file)) + rel_file + struct.pack("!Q", rev1) + b"world!"
        sock.sendall(frame(704, 0x0204, write_req))
        req_id, status, payload = recv_response(sock, 0x0204)
        assert req_id == 704 and status == 0
        rev2, = struct.unpack("!Q", payload)
        assert rev2 != rev1

        # 用旧的 rev1 再次写，应当返回 Status::conflict (6) 以及当前的 rev2
        sock.sendall(frame(705, 0x0204, write_req))
        req_id, status, payload = recv_response(sock, 0x0204)
        assert req_id == 705 and status == 6
        conflict_rev, = struct.unpack("!Q", payload)
        assert conflict_rev == rev2

        # 5. 在已 list_dir 的目录下新建文件，验证服务端通过 inotify 主动推送 Kind::event (kind=3)
        with open(os.path.join(proj_dir, "b.txt"), "wb") as f:
            f.write(b"new")

        length, = struct.unpack("!I", recv_exact(sock, 4))
        kind, event_req_id, method, status = struct.unpack("!BQHH", recv_exact(sock, 13))
        event_payload = recv_exact(sock, length - 13)
        assert kind == 3 and event_req_id == 0 and method == 0x0202 and status == 0
        assert (struct.pack("!BH", 0, 5) + b"b.txt") in event_payload


def test_terminal(port):
    with connect(port) as sock:
        # 1. terminal_state (0x0303) 启动 Shell：payload = [u32 len=0][""]
        start_req = struct.pack("!I", 0)
        sock.sendall(frame(801, 0x0303, start_req))
        req_id, status, payload = recv_response(sock, 0x0303)
        assert req_id == 801 and status == 0
        data_len, term_id = struct.unpack("!II", payload)
        assert data_len == 4 and term_id >= 1

        # 2. terminal_on_tab (0x0302) Tab 补全：payload = [u32 len][u32 terminal_id][data]
        tab_cmd = b"ls de"
        tab_req = struct.pack("!II", len(tab_cmd), term_id) + tab_cmd
        sock.sendall(frame(802, 0x0302, tab_req))
        req_id, status, payload = recv_response(sock, 0x0302)
        assert req_id == 802 and status == 0
        res_len, res_tid = struct.unpack("!II", payload[:8])
        completed = payload[8:]
        assert res_len == len(completed) and res_tid == term_id
        assert completed.startswith(b"ls demo")

        # 3. terminal_input (0x0301) 执行命令，接收 0x0381 输出事件与 0x0303 完成事件
        cmd = b"echo HELLO_TINA_TERM"
        input_req = struct.pack("!II", len(cmd), term_id) + cmd
        sock.sendall(frame(803, 0x0301, input_req))

        got_input_resp = False
        got_done_event = False
        outputs = bytearray()

        while not (got_input_resp and got_done_event):
            length, = struct.unpack("!I", recv_exact(sock, 4))
            kind, r_id, method, status = struct.unpack("!BQHH", recv_exact(sock, 13))
            body = recv_exact(sock, length - 13)
            if kind == 2 and method == 0x0301:
                assert r_id == 803 and status == 0
                got_input_resp = True
            elif kind == 3 and method == 0x0381:
                out_len, out_tid = struct.unpack("!II", body[:8])
                assert out_tid == term_id and out_len == len(body) - 8
                outputs.extend(body[8:])
            elif kind == 3 and method == 0x0303:
                ev_len, ev_tid = struct.unpack("!II", body)
                assert ev_len == 4 and ev_tid == term_id
                got_done_event = True

        assert b"HELLO_TINA_TERM" in outputs

        # 4. terminal_state (0x0303) 关闭 Shell：payload = [u32 len=4][u32 terminal_id]
        close_req = struct.pack("!II", 4, term_id)
        sock.sendall(frame(804, 0x0303, close_req))
        req_id, status, payload = recv_response(sock, 0x0303)
        assert req_id == 804 and status == 0
        assert payload == struct.pack("!II", 4, term_id)


def test_invalid_frames(port):
    for packet in (struct.pack("!I", 12), struct.pack("!I", 1024 * 1024 + 1),
                   frame(0, 1), frame(1, 1, status=1), frame(1, 1, kind=2)):
        with connect(port) as sock:
            sock.sendall(packet)
            expect_closed(sock)

    # 错误连接不能导致整个 Server 崩溃。
    with connect(port) as sock:
        sock.sendall(frame(301, 0x0001))
        assert recv_response(sock, 0x0001) == (301, 0, b"PONG")


def test_half_close(port):
    with connect(port) as sock:
        sock.sendall(frame(401, 0x0001))

        # 客户端不再发送，但仍保留接收方向。
        sock.shutdown(socket.SHUT_WR)

        assert recv_response(sock, 0x0001) == (401, 0, b"PONG")
        expect_closed(sock)


def test_concurrent_clients(port):
    def client(index):
        key = f"concurrent-{index}".encode()
        value = f"value-{index}".encode()

        with connect(port) as sock:
            sock.sendall(frame(501, 0x0101, key + b"\n" + value))
            assert recv_response(sock, 0x0101) == (501, 0, b"OK")

            sock.sendall(frame(502, 0x0102, key))
            assert recv_response(sock, 0x0102) == (502, 0, value)

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
                ("IPv4 / IPv6 dual stack", test_dual_stack),
                ("fragmentation", test_fragmentation),
                ("pipelining", test_pipelining),
                ("KV dispatch", test_kv_dispatch),
                ("workspace & inotify", lambda p: test_workspace_and_inotify(p, projects_root)),
                ("terminal pty & tab", test_terminal),
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
