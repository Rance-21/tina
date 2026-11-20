#!/usr/bin/env bash
# ============================================================================
# Tina WorkerPool V2 参数穷举脚本（k = 1..7）
# ============================================================================
# 说明（对应 docs/workerpool_design.txt 第 7 节）：
#   1. 固定配置：
#      - Server 端：./tina server 3490 --normal-workers <k>
#        （内部绑核：EventLoop -> CPUs 0,2,4,8；Workers/Acceptor/Logger -> CPUs 16-23）
#      - 压测端：taskset -c 6,10,12,14,24-31 ./tina_bench --workload ide_mixed --port 3490 --connections 4 --inflight 64
#        （隔离到剩余 4P+8E 核心，默认 10s warmup + 50s measured）
#   2. 交错顺序遍历 k=1..7（默认 "1 7 2 6 3 5 4"），减小连续升温导致的单调热漂移；
#      可用 --perf 挂载 perf stat 采集服务端硬件/调度计数器（task-clock/cycles/instructions/
#      context-switches/cpu-migrations），无需在 Server 热路径内嵌入计时指标。
#   3. 进程安全与可审计：
#      - 启动前检查端口未被占用，绝不杀死系统中其他进程；
#      - 使用 trap 保证异常退出/中断时仅终止本脚本启动的 SERVER_PID；
#      - 完整保留原始输出、退出状态码与汇总 CSV，不隐藏任何失败或拒绝。
#
# 用法示例：
#   ./scripts/sweep_normal_workers.sh
#   ./scripts/sweep_normal_workers.sh --perf
#   ./scripts/sweep_normal_workers.sh --rounds 3 --k-list "2 3 4"

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BIN="${ROOT_DIR}/tina"
BENCH_BIN="${ROOT_DIR}/tina_bench"
PORT="3490"
CLIENT_CPUS="6,10,12,14,24-31"
K_LIST="1 7 2 6 3 5 4"
ROUNDS=1
USE_PERF=0
PERF_EVENTS="task-clock,cycles,instructions,context-switches,cpu-migrations,cache-misses"
EXTRA_BENCH_ARGS=()

# 【作用】打印参数穷举脚本的全部命令行选项与默认值说明。
# 【原理】通过 heredoc 将选项列表输出到标准输出。
usage() {
    cat <<'EOF'
用法: ./scripts/sweep_normal_workers.sh [选项]

选项:
  --server-bin <path>     tina 服务端可执行文件路径 (默认: ./tina)
  --bench-bin <path>      tina_bench 可执行文件路径 (默认: ./tina_bench)
  --port <port>           测试端口 (默认: 3490)
  --client-cpus <list>    tina_bench 绑定的剩余核心集合 (默认: 6,10,12,14,24-31)
  --k-list "<k...>"       要遍历的 normal_workers 列表 (默认: "1 7 2 6 3 5 4")
  --rounds <n>            完整轮次重复数 (默认: 1)
  --perf                  使用 perf stat 采集服务端执行期的 CPU 与调度指标
  --perf-events <list>    自定义 perf stat -e 事件列表
  --out-dir <dir>         原始输出保存目录 (默认: benchmark/sweep_<timestamp>)
  --bench-arg <arg>       透传给 tina_bench 的额外参数（可重复指定）
  -h, --help              显示此帮助信息
EOF
}

OUT_DIR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --server-bin) SERVER_BIN="$2"; shift 2 ;;
        --bench-bin) BENCH_BIN="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --client-cpus) CLIENT_CPUS="$2"; shift 2 ;;
        --k-list) K_LIST="$2"; shift 2 ;;
        --rounds) ROUNDS="$2"; shift 2 ;;
        --perf) USE_PERF=1; shift ;;
        --perf-events) PERF_EVENTS="$2"; USE_PERF=1; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --bench-arg) EXTRA_BENCH_ARGS+=("$2"); shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *)
            echo "错误: 未知参数 $1" >&2
            usage
            exit 1
            ;;
    esac
done

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
if [[ -z "$OUT_DIR" ]]; then
    OUT_DIR="${ROOT_DIR}/benchmark/sweep_${TIMESTAMP}"
fi

if [[ ! -x "$SERVER_BIN" ]]; then
    echo "错误: 找不到可执行服务端 $SERVER_BIN（请先执行 Release/RelWithDebInfo 构建）" >&2
    exit 1
fi
if [[ ! -x "$BENCH_BIN" ]]; then
    echo "错误: 找不到可执行压测端 $BENCH_BIN（请先执行 Release/RelWithDebInfo 构建）" >&2
    exit 1
fi
if [[ ! -d "${HOME}/projects/linux" ]]; then
    echo "错误: ide_mixed 需要 ~/projects/linux 只读源码树，当前不存在。" >&2
    exit 1
fi
if [[ "$USE_PERF" -eq 1 ]] && ! command -v perf >/dev/null 2>&1; then
    echo "错误: 未找到 perf 命令，无法启用 --perf。" >&2
    exit 1
fi

# 【作用】探测目标 TCP 端口是否已被占用（返回 0 表示已占用，返回 1 表示空闲）。
# 【原理】用内联 Python 分别尝试非阻塞连接 127.0.0.1:PORT 和 [::1]:PORT，任一连上即认为端口被占用。
port_in_use() {
    python3 - "$PORT" <<'PY'
import socket, sys
port = int(sys.argv[1])
for family, addr in ((socket.AF_INET, ("127.0.0.1", port)), (socket.AF_INET6, ("::1", port))):
    try:
        with socket.socket(family, socket.SOCK_STREAM) as s:
            s.settimeout(0.2)
            if s.connect_ex(addr) == 0:
                sys.exit(0)
    except OSError:
        pass
sys.exit(1)
PY
}

if port_in_use; then
    echo "错误: 端口 $PORT 已被其他进程占用；为避免干扰现有服务，脚本拒绝继续。" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

SERVER_PID=""
PERF_PID=""
# 【作用】安全终止单个由本脚本启动的子进程（先优雅 SIGTERM，超时再强杀 SIGKILL）。
# 【原理】若进程仍存活，先发 SIGTERM 并轮询最多 10 秒等待退出，超时未退才补发 SIGKILL，最后 wait 回收僵尸进程。
stop_child_pid() {
    local pid="${1:-}"
    if [[ -z "$pid" ]]; then
        return 0
    fi
    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        local deadline=$((SECONDS + 10))
        while kill -0 "$pid" 2>/dev/null; do
            if (( SECONDS >= deadline )); then
                kill -KILL "$pid" 2>/dev/null || true
                break
            fi
            sleep 0.1
        done
    fi
    wait "$pid" 2>/dev/null || true
}

# 【作用】脚本异常中断或正常退出时的统一清理回调，防止残留后台服务端或 perf 进程。
# 【原理】通过 trap 注册到 EXIT/INT/TERM 信号，依次调用 stop_child_pid 关闭 PERF_PID 和 SERVER_PID。
cleanup() {
    stop_child_pid "$PERF_PID"
    PERF_PID=""
    stop_child_pid "$SERVER_PID"
    SERVER_PID=""
}
trap cleanup EXIT INT TERM

# 【作用】等待刚拉起的 tina 服务端进入就绪监听状态。
# 【原理】在 10 秒超时窗口内每 50ms 检查一次进程存活（kill -0）与端口连通性（port_in_use），就绪即返回 0。
wait_server_ready() {
    local pid="$1"
    local deadline=$((SECONDS + 10))
    while (( SECONDS < deadline )); do
        if ! kill -0 "$pid" 2>/dev/null; then
            return 1
        fi
        if port_in_use; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

# 记录实验环境基线信息，保证实验条件完整可复现。
{
    echo "timestamp=$TIMESTAMP"
    echo "hostname=$(hostname)"
    echo "kernel=$(uname -r)"
    echo "git_head=$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "server_bin=$SERVER_BIN"
    echo "bench_bin=$BENCH_BIN"
    echo "port=$PORT"
    echo "client_cpus=$CLIENT_CPUS"
    echo "use_perf=$USE_PERF"
    echo "online_cpus=$(cat /sys/devices/system/cpu/online 2>/dev/null || echo unknown)"
    echo "cpu0_governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    echo "cpu16_governor=$(cat /sys/devices/system/cpu/cpu16/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    for sib in 1 3 5 9; do
        echo "cpu${sib}_online=$(cat "/sys/devices/system/cpu/cpu${sib}/online" 2>/dev/null || echo unknown)"
    done
} | tee "${OUT_DIR}/env_meta.txt"

SUMMARY_CSV="${OUT_DIR}/summary.csv"
echo "round,normal_workers,blocking_workers,server_exit,bench_exit,qps,rx_mib_s,errors,p50_us,p95_us,p99_us,hot_inline_p99_us,hot_normal_p99_us,cold_p99_us,report_file" >"$SUMMARY_CSV"

# 【作用】针对单个 (round, k) 组合执行完整的一轮基准测试并把关键指标追加写入 summary.csv。
# 【原理】依次完成：检查端口空闲 -> 后台启动 --normal-workers k 服务端并等就绪 -> 可选挂载 perf stat
#         -> 用 taskset 绑定剩余核心运行 tina_bench -> 优雅停止 perf 与服务端并收集退出码
#         -> 用内联 Python 从压测报告提取 QPS/吞吐/错误数/分位延迟及分类 P99 追加到 CSV。
run_one_k() {
    local round="$1"
    local k_val="$2"
    local b_val=$((8 - k_val))

    if port_in_use; then
        echo "错误: 启动 k=${k_val} 前发现端口 $PORT 仍被占用" >&2
        exit 1
    fi

    local tag="r${round}_k${k_val}"
    local server_log="${OUT_DIR}/${tag}_server.log"
    local bench_log="${OUT_DIR}/${tag}_bench.log"
    local report_file="${OUT_DIR}/${tag}_report.txt"
    local perf_file="${OUT_DIR}/${tag}_perf.txt"

    local server_cmd=("$SERVER_BIN" "server" "$PORT" "--normal-workers" "$k_val")

    echo "===================================================================="
    echo "[Sweep] Round ${round} | k=${k_val} (Normal=${k_val}, Blocking=${b_val}) | Cmd: ${server_cmd[*]}"

    (
        cd "$ROOT_DIR"
        exec "${server_cmd[@]}"
    ) >"$server_log" 2>&1 &
    SERVER_PID="$!"

    if ! wait_server_ready "$SERVER_PID"; then
        echo "错误: Server 启动或就绪检查失败 (查看 $server_log)" >&2
        stop_child_pid "$SERVER_PID"
        SERVER_PID=""
        echo "${round},${k_val},${b_val},START_FAIL,SKIP,0,0,0,0,0,0,0,0,0,${report_file}" >>"$SUMMARY_CSV"
        return 1
    fi

    if [[ "$USE_PERF" -eq 1 ]]; then
        perf stat -e "$PERF_EVENTS" -p "$SERVER_PID" -o "$perf_file" &
        PERF_PID="$!"
    fi

    local bench_exit=0
    set +e
    taskset -c "$CLIENT_CPUS" \
        "$BENCH_BIN" --workload ide_mixed --port "$PORT" --connections 4 --inflight 64 \
        --output "$report_file" "${EXTRA_BENCH_ARGS[@]}" \
        >"$bench_log" 2>&1
    bench_exit=$?
    set -e

    if [[ -n "$PERF_PID" ]]; then
        if kill -0 "$PERF_PID" 2>/dev/null; then
            kill -INT "$PERF_PID" 2>/dev/null || true
            wait "$PERF_PID" 2>/dev/null || true
        fi
        PERF_PID=""
    fi

    local server_exit=0
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        set +e
        wait "$SERVER_PID"
        server_exit=$?
        set -e
    else
        set +e
        wait "$SERVER_PID"
        server_exit=$?
        set -e
    fi
    SERVER_PID=""

    echo "[Sweep] k=${k_val} finished: bench_exit=${bench_exit}, server_exit=${server_exit}"

    python3 - "$round" "$k_val" "$b_val" "$server_exit" "$bench_exit" "$report_file" "$SUMMARY_CSV" <<'PY'
import pathlib, re, sys

round_id, k_val, b_val, srv_rc, bench_rc, rpt_path, csv_path = sys.argv[1:]
text = pathlib.Path(rpt_path).read_text() if pathlib.Path(rpt_path).exists() else ""

def grab(pattern, default="NA"):
    m = re.search(pattern, text, re.M)
    return m.group(1) if m else default

qps = grab(r"^\s*平均 QPS\s*:\s*([0-9.]+)")
rx = grab(r"^\s*RX 吞吐\s*:\s*([0-9.]+)")
errs = grab(r"^\s*测量错误数\s*:\s*([0-9.]+)")
p50 = grab(r"^\s*P50\s*:\s*([0-9.]+)")
p95 = grab(r"^\s*P95\s*:\s*([0-9.]+)")
p99 = grab(r"^\s*P99\s*:\s*([0-9.]+)")
hot_in_p99 = grab(r"Inline 热读 \(<=32K\)\s*:.*P99=\s*([0-9.]+)us")
hot_nm_p99 = grab(r"Normal 热读 \(>32K\)\s*:.*P99=\s*([0-9.]+)us")
cold_p99 = grab(r"Blocking 冷读 \(>2M\)\s*:.*P99=\s*([0-9.]+)us")

row = ",".join([
    round_id, k_val, b_val, srv_rc, bench_rc,
    qps, rx, errs, p50, p95, p99, hot_in_p99, hot_nm_p99, cold_p99, rpt_path
])
with open(csv_path, "a", encoding="utf-8") as f:
    f.write(row + "\n")
print(f"  -> QPS={qps} RX={rx} MiB/s Errors={errs} P50={p50}us P95={p95}us P99={p99}us")
PY
}

for round in $(seq 1 "$ROUNDS"); do
    for k in $K_LIST; do
        run_one_k "$round" "$k" || true
        sleep 1
    done
done

echo ""
echo "==================== Sweep 原始结果汇总 (${SUMMARY_CSV}) ===================="
column -t -s',' "$SUMMARY_CSV" || cat "$SUMMARY_CSV"
