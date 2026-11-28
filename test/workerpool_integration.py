#!/usr/bin/env python3
"""WorkerPool V2 端到端集成回归：大文件 Normal offload、目录内联缓存+inotify、读写顺序与 CLI 参数验证。"""

import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

HOST = "127.0.0.1"
TIMEOUT = 3.0


def frame(request_id, method, payload=b"", *, kind=1, status=0):
    """【作用】按 Tina 二进制 RPC 协议打包一帧请求/响应消息。
    【原理】采用大端格式打包 4 字节体长（13 + payload 长度）+ 13 字节帧头（kind/req_id/method/status）再拼接 payload。
    """
    return struct.pack("!IBQHH", 13 + len(payload), kind, request_id, method, status) + payload


def recv_exact(sock, count):
    """【作用】从 TCP 流中精确读取指定字节数，处理半包/分片接收。
    【原理】循环调用 sock.recv 直到凑齐 count 字节；若对端提前关闭（返回空）则抛出断言失败。
    """
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise AssertionError("unexpected EOF")
        data.extend(chunk)
    return bytes(data)


def recv_frame(sock):
    """【作用】从 socket 解析并返回一个完整的 Tina RPC 协议帧。
    【原理】先读 4 字节总长度并校验范围，再读 13 字节固定帧头解包，最后按剩余长度读取 payload。
    """
    length, = struct.unpack("!I", recv_exact(sock, 4))
    assert 13 <= length <= 1024 * 1024
    kind, req_id, method, status = struct.unpack("!BQHH", recv_exact(sock, 13))
    payload = recv_exact(sock, length - 13)
    return kind, req_id, method, status, payload


def recv_response(sock, expected_method):
    """【作用】接收一帧响应并校验其类型为 Response(kind==2) 且方法号与预期一致。
    【原理】调用 recv_frame 解帧后断言 kind 与 method，返回 (request_id, status, payload)。
    """
    kind, req_id, method, status, payload = recv_frame(sock)
    assert kind == 2 and method == expected_method, (kind, method, expected_method)
    return req_id, status, payload


def find_free_port():
    """【作用】向操作系统申请一个当前未被占用的本地临时 TCP 端口。
    【原理】创建 socket 绑定到 127.0.0.1:0 由内核分配空闲端口，读取端口号后立即关闭释放。
    """
    with socket.socket() as sock:
        sock.bind((HOST, 0))
        return sock.getsockname()[1]


def start_server(tina_bin, env, port, extra_args):
    """【作用】拉起 tina 服务端进程并等待其开始监听指定端口。
    【原理】用 subprocess.Popen 后台启动服务端，随后 5 秒内轮询检查进程存活与 TCP 连通性，
    连上即返回 Popen 对象，超时或提前退出则清理进程并抛错。
    """
    proc = subprocess.Popen(
        [tina_bin, "server", str(port), *extra_args],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
    )
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            out = proc.stdout.read() if proc.stdout else ""
            raise AssertionError(f"server exited early ({proc.returncode}): {out}")
        try:
            with socket.create_connection((HOST, port), timeout=0.2):
                return proc
        except OSError:
            time.sleep(0.03)
    proc.kill()
    proc.wait()
    raise AssertionError("server startup timed out")


def test_cli_validation(tina_bin, env):
    """验证服务端命令行参数校验会拒绝非法队列容量及已移除参数。"""
    for bad_args in (
        ["--queue-capacity", "0"],
        ["--queue-capacity", "3"],
        ["--queue-capacity", "abc"],
        ["--normal-workers", "2"],
    ):
        res = subprocess.run(
            [tina_bin, "server", "0", *bad_args],
            env=env,
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert res.returncode != 0, (bad_args, res.stdout, res.stderr)
    print("PASS: CLI argument validation", flush=True)


def test_v2_paths(tina_bin, env, projects_root):
    """端到端回归同构 WorkerPool + 零拷贝内联的全部核心路径与语义一致性。"""
    proj = Path(projects_root) / "v2demo"
    proj.mkdir(parents=True, exist_ok=True)
    # 1. 准备 80 KiB 大文件，验证首次 Worker 读盘与后续零拷贝内联命中。
    large_data = (b"0123456789ABCDEF" * 5120)  # 81920 bytes
    large_file = proj / "big80k.bin"
    large_file.write_bytes(large_data)

    # 2. 准备小文件与读写一致性文件。
    small_file = proj / "small.txt"
    small_file.write_bytes(b"v1-small")

    port = find_free_port()
    proc = start_server(tina_bin, env, port, [])
    try:
        with socket.create_connection((HOST, port), timeout=TIMEOUT) as sock:
            # Inline ping
            sock.sendall(frame(1, 0x0001))
            assert recv_response(sock, 0x0001) == (1, 0, b"PONG")

            # Blocking cache miss for 80 KiB file
            rel_big = b"v2demo/big80k.bin"
            read_big = struct.pack("!H", len(rel_big)) + rel_big + struct.pack("!QI", 0, 81920)
            sock.sendall(frame(2, 0x0203, read_big))
            rid, st, pl = recv_response(sock, 0x0203)
            assert rid == 2 and st == 0 and pl[16:] == large_data

            # Normal Worker cache hit (> 32 KiB slice)
            sock.sendall(frame(3, 0x0203, read_big))
            rid, st, pl = recv_response(sock, 0x0203)
            assert rid == 3 and st == 0 and pl[16:] == large_data

            # Inline cache hit (<= 32 KiB slice of big file)
            read_slice = struct.pack("!H", len(rel_big)) + rel_big + struct.pack("!QI", 4096, 16384)
            sock.sendall(frame(4, 0x0203, read_slice))
            rid, st, pl = recv_response(sock, 0x0203)
            assert rid == 4 and st == 0 and pl[16:] == large_data[4096 : 4096 + 16384]

            # Directory list miss -> hit (Inline) -> external file creation triggers inotify Event
            dir_req = struct.pack("!H", 6) + b"v2demo"
            sock.sendall(frame(5, 0x0202, dir_req))
            rid, st, _ = recv_response(sock, 0x0202)
            assert rid == 5 and st == 0

            # Second list_dir hits cache Inline and keeps inotify watch active
            sock.sendall(frame(6, 0x0202, dir_req))
            rid, st, _ = recv_response(sock, 0x0202)
            assert rid == 6 and st == 0

            (proj / "added_after_inline.txt").write_bytes(b"ok")
            kind, ev_rid, ev_method, ev_st, ev_pl = recv_frame(sock)
            assert (kind, ev_rid, ev_method, ev_st) == (3, 0, 0x0202, 0)
            assert b"added_after_inline.txt" in ev_pl

            # Read-after-write ordering: seed small.txt into cache, then pipeline write + read
            rel_small = b"v2demo/small.txt"
            read_small = struct.pack("!H", len(rel_small)) + rel_small + struct.pack("!QI", 0, 1024)
            sock.sendall(frame(7, 0x0203, read_small))
            rid, st, pl = recv_response(sock, 0x0203)
            assert rid == 7 and st == 0 and pl[16:] == b"v1-small"
            _, rev1 = struct.unpack("!QQ", pl[:16])

            # Write then sequential read: must observe v2-updated (never stale cache)
            write_req = (
                struct.pack("!H", len(rel_small))
                + rel_small
                + struct.pack("!Q", rev1)
                + b"v2-updated"
            )
            sock.sendall(frame(8, 0x0204, write_req))
            rid, st, pl = recv_response(sock, 0x0204)
            assert rid == 8 and st == 0
            rev2, = struct.unpack("!Q", pl)
            assert rev2 != rev1

            sock.sendall(frame(9, 0x0203, read_small))
            rid, st, pl = recv_response(sock, 0x0203)
            assert rid == 9 and st == 0 and pl[16:] == b"v2-updated"
            _, rev_read = struct.unpack("!QQ", pl[:16])
            assert rev_read == rev2

            # Pipelined write + read in one sendall: read must NOT overtake write via inline path
            write_req2 = (
                struct.pack("!H", len(rel_small))
                + rel_small
                + struct.pack("!Q", rev2)
                + b"v3-pipelined"
            )
            sock.sendall(frame(10, 0x0204, write_req2) + frame(11, 0x0203, read_small))
            seen = {}
            for _ in range(2):
                kind, rid, method, st, pl = recv_frame(sock)
                assert kind == 2 and st == 0
                seen[rid] = (method, pl)
            assert 10 in seen and 11 in seen

        # Graceful SIGTERM shutdown must complete cleanly with rc == 0.
        proc.terminate()
        stdout_data, _ = proc.communicate(timeout=5)
        assert proc.returncode == 0, stdout_data
        assert "shutdown: completed" in stdout_data, stdout_data
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

    print("PASS: WorkerPool V2 normal/inline/inotify/consistency/stats", flush=True)


def main():
    """【作用】集成测试脚本入口，在隔离的临时 HOME/projects 目录中运行全部回归用例。
    【原理】解析命令行传入的 tina 可执行文件路径，创建 TemporaryDirectory 覆盖 HOME 环境变量，
    依次执行 CLI 校验和 V2 端到端路径测试，退出时自动清理临时目录。
    """
    if len(sys.argv) != 2:
        raise SystemExit("usage: workerpool_integration.py <tina binary>")
    tina_bin = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="tina-wpv2-") as tmp:
        projects = Path(tmp) / "projects"
        projects.mkdir()
        env = os.environ.copy()
        env["HOME"] = tmp
        test_cli_validation(tina_bin, env)
        test_v2_paths(tina_bin, env, projects)


if __name__ == "__main__":
    main()
