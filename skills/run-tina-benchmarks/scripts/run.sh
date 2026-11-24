#!/usr/bin/env bash
set -euo pipefail

repo=$(pwd -P)
port=${TINA_BENCH_PORT:-3490}
client_cpus=${TINA_CLIENT_CPUS:-6,10,12,14,24-31}
fs_root=${TINA_FS_ROOT:-linux}
scratch_root=${TINA_FS_SCRATCH_ROOT:-.}

cmake -S . -B build-release -G Ninja \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-release --target tina tina_bench

results=$(mktemp -d "$repo/build-release/mixed-$(date +%Y%m%d-%H%M%S)-XXXXXX")
echo "压测结果目录 $results"
{
    git rev-parse HEAD
    git status --short -- src benchmark/*.cpp benchmark/*.cppm
    echo "port=$port client_cpus=$client_cpus fs_root=$fs_root scratch_root=$scratch_root connections=4 inflight=64"
    /usr/bin/clang++ --version
} > "$results/config.txt"

# 两个程序共享 HOME；切换工作目录，让服务端的相对日志路径也落在本轮目录下。
cd "$results"
server_pid=
stop_server() {
    if [[ -n "$server_pid" ]]; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid"
        server_pid=
    fi
}
trap stop_server EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for workload in ide_mixed fs_mixed; do
    log="$results/$workload.server.log"
    "$repo/tina" server "$port" > "$log" 2>&1 &
    server_pid=$!
    # 只认本进程输出的监听就绪日志，避免误测已占用该端口的其他服务。
    for ((i=0; i<100; i++)); do
        if grep -Fq "tina listening on port $port |" "$log"; then break; fi
        if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
        sleep 0.1
    done
    if ! grep -Fq "tina listening on port $port |" "$log"; then
        cat "$log" >&2
        exit 1
    fi
    if [[ "$workload" == ide_mixed ]]; then
        warmup=10 duration=60 read_bytes=65536
    else
        warmup=5 duration=30 read_bytes=4096
    fi
    taskset -c "$client_cpus" "$repo/tina_bench" \
        --workload "$workload" --host 127.0.0.1 --port "$port" --fs-root "$fs_root" \
        --fs-scratch-root "$scratch_root" --connections 4 --inflight 64 --warmup "$warmup" --duration "$duration" \
        --read-bytes "$read_bytes" --output "$results/$workload.txt"
    stop_server
done
