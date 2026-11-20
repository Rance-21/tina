#!/usr/bin/env python3
"""ide_mixed 端到端回归；--offline 不创建监听 socket。"""

import hashlib
import os
from pathlib import Path
import random
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time


def snapshot(root):
    result = {}
    for path in sorted(root.rglob("*")):
        stat = path.lstat()
        value = os.readlink(path) if path.is_symlink() else (
            hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else "dir"
        )
        result[str(path.relative_to(root))] = (stat.st_mode, stat.st_mtime_ns, stat.st_ino, value)
    return result


def main():
    if len(sys.argv) not in (3, 4) or (len(sys.argv) == 4 and sys.argv[3] != "--offline"):
        raise SystemExit("usage: benchmark_mixed.py <tina> <tina_bench> [--offline]")
    server_binary, bench_binary = (str(Path(arg).resolve()) for arg in sys.argv[1:3])
    offline = "--offline" in sys.argv
    with tempfile.TemporaryDirectory(prefix="tina-mixed-test-") as tmp:
        home = Path(tmp)
        projects = home / "projects"
        source = projects / "linux"
        (source / "kernel").mkdir(parents=True)
        (source / ".git").mkdir()
        (source / ".git" / "sentinel").write_text("do not touch repository metadata\n")
        (source / "untracked.txt").write_text("preserve untracked and edited content\n")
        (source / "kernel" / "space ' name.c").write_text("/* quoting */\n" * 1024)
        rng = random.Random(7131)
        for index in range(8):
            (source / "kernel" / f"file{index}.c").write_bytes(rng.randbytes(4096 * (index + 1)))
        (source / "kernel" / "large.c").write_bytes(rng.randbytes(3 * 1024 * 1024))
        (home / "outside.txt").write_text("outside sentinel")
        (source / "outside-link").symlink_to(home / "outside.txt")
        (source / "loop").symlink_to(source, target_is_directory=True)
        baseline = snapshot(source)
        env = os.environ.copy()
        env["HOME"] = str(home)

        def check_unchanged():
            assert snapshot(source) == baseline, "source tree was modified"
            assert (home / "outside.txt").read_text() == "outside sentinel"
            assert sorted(p.name for p in projects.iterdir()) == ["linux"], "scratch leaked"

        def command(*args, port=0, report="report.txt"):
            return [bench_binary, "--workload", "ide_mixed", "--port", str(port),
                    "--output", str(home / report), *args]

        def reject(args, expected):
            result = subprocess.run(command(*args), env=env, capture_output=True, text=True, timeout=20)
            assert result.returncode != 0, result.stdout
            assert expected in result.stderr, result.stderr
            check_unchanged()

        reject(["--fs-hot-percent", "101"], "fs-hot-percent")
        reject(["--read-bytes", "0"], "read-bytes")
        reject(["--warmup", "-1"], "warmup")
        reject(["--duration", "10"], "warmup")  # 默认暖机是 10s。
        reject(["--host", "192.0.2.1"], "local server")
        reject(["--fs-root", "../"], "within ~/projects")
        reject(["--fs-scratch-root", "linux/kernel"], "outside fs-root")
        reject(["--output", str(source / "untracked.txt")], "report output")
        # 连接失败发生在 mkdtemp 之后，验证异常路径确实清理独占目录。
        reject(["--warmup", "0", "--duration", "1"], "benchmark error:")
        print("PASS argument validation, source protection, setup-failure cleanup", flush=True)
        if offline:
            return

        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        server = subprocess.Popen([server_binary, "server", str(port)], env=env, cwd=home,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 5
            while True:
                if server.poll() is not None:
                    raise AssertionError("server exited during startup")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise AssertionError("server startup timed out")
                    time.sleep(0.03)

            def metric(report, label):
                match = re.search(r"^  " + re.escape(label) + r"\s*:\s*([0-9.]+)", report, re.M)
                assert match, (label, report)
                return float(match[1])

            for hot, warmup, duration, connections, inflight in [(70, 1, 4, 2, 16), (0, 0, 2, 1, 1),
                                                                (100, 0, 2, 1, 64)]:
                result = subprocess.run(command("--fs-hot-percent", str(hot), "--warmup", str(warmup),
                    "--duration", str(duration), "--connections", str(connections), "--inflight", str(inflight),
                    port=port), env=env, capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, result.stdout + result.stderr
                report = (home / "report.txt").read_text()
                assert metric(report, "测量错误数") == 0
                assert metric(report, "正式测量时长") == duration - warmup
                for label in ["目录读取", "文件写入", "终端命令", "已确认目录变更"]:
                    assert metric(report, label) > 0, report
                assert (metric(report, "热文件读") > 0) == (hot > 0), report
                assert (metric(report, "冷文件读") > 0) == (hot < 100), report
                assert metric(report, "目录变更事件") >= metric(report, "已确认目录变更"), report
                assert "已验证 (源码树未改动)" in report
                check_unchanged()
                print(f"PASS hot={hot}% connections={connections} inflight={inflight}", flush=True)

            for sig in (signal.SIGINT, signal.SIGTERM, None):
                log = home / "interrupt.log"
                with log.open("w+") as output:
                    proc = subprocess.Popen(command("--warmup", "0", "--duration", "30", port=port,
                                                    report="interrupted-report.txt"),
                                            env=env, stdout=output, stderr=output, text=True)
                    try:
                        deadline = time.monotonic() + 15
                        while "开始压测" not in log.read_text():
                            assert proc.poll() is None, log.read_text()
                            assert time.monotonic() < deadline, log.read_text()
                            time.sleep(0.02)
                        time.sleep(0.1)
                        if sig is None:
                            server.terminate()
                            server.wait(timeout=5)
                        else:
                            proc.send_signal(sig)
                        assert proc.wait(timeout=20) != 0, log.read_text()
                        if sig is not None:
                            assert "interrupted; scratch cleaned" in log.read_text(), log.read_text()
                        assert not (home / "interrupted-report.txt").exists()
                        check_unchanged()
                    finally:
                        if proc.poll() is None:
                            proc.kill()
                            proc.wait()
                print(f"PASS cleanup after {sig.name if sig else 'server disconnect'}", flush=True)
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()


if __name__ == "__main__":
    main()
