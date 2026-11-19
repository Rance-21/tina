#!/usr/bin/env bash
# ============================================================================
# 可选 SMT 超线程兄弟核管理脚本（仅操作 I/O P-core 0,2,4,8 对应的 sibling 1,3,5,9）
# ============================================================================
# 说明（对应 docs/workerpool_design.txt 第 1 节）：
#   - Tina 主程序不申请 root 权限；需要严格隔离 P-core 时由本脚本单独控制 SMT。
#   - offline 子命令会先读取各目标 sibling 当前 online 状态并落盘，随后仅将这些 sibling 设为 0；
#   - restore 子命令严格按已记录的原始状态恢复（若原本即为 0 则保持 0，原本为 1 则恢复为 1）。
#
# 用法：
#   sudo ./scripts/smt_ctl.sh status
#   sudo ./scripts/smt_ctl.sh offline [state_file]
#   sudo ./scripts/smt_ctl.sh restore [state_file]

set -euo pipefail

IO_CPUS=(0 2 4 8)
TARGET_SIBLINGS=(1 3 5 9)
DEFAULT_STATE_FILE="/tmp/tina_smt_siblings.state"

# 【作用】打印脚本的命令行用法说明。
# 【原理】使用 heredoc 向标准输出打印 status / offline / restore 三个子命令的调用格式。
usage() {
    cat <<'EOF'
用法:
  ./scripts/smt_ctl.sh status
  sudo ./scripts/smt_ctl.sh offline [state_file]
  sudo ./scripts/smt_ctl.sh restore [state_file]
EOF
}

# 【作用】检查当前是否以 root 身份运行，防止后续写入 sysfs 失败。
# 【原理】通过 id -u 判断有效 UID 是否为 0，非 0 则输出错误提示到 stderr 并以状态 1 退出。
require_root() {
    if [[ "$(id -u)" -ne 0 ]]; then
        echo "错误: 修改 /sys/devices/system/cpu/cpu*/online 需要 root 权限，请使用 sudo 运行。" >&2
        exit 1
    fi
}

# 【作用】在下线操作前核验目标 sibling（1,3,5,9）确实与 I/O P-core（0,2,4,8）一一对应。
# 【原理】读取每个 I/O 核的 thread_siblings_list，若目标 sibling 当前在线，则用内联 Python 解析
#         列表确认二者属于同一物理核，防止误下线无关核心。
verify_siblings_topology() {
    local idx io_cpu expected_sib sib_file sib_list
    for idx in "${!IO_CPUS[@]}"; do
        io_cpu="${IO_CPUS[$idx]}"
        expected_sib="${TARGET_SIBLINGS[$idx]}"
        sib_file="/sys/devices/system/cpu/cpu${io_cpu}/topology/thread_siblings_list"
        if [[ ! -r "$sib_file" ]]; then
            echo "错误: 无法读取拓扑文件 $sib_file" >&2
            exit 1
        fi
        sib_list="$(cat "$sib_file")"
        # 仅当目标 sibling 仍在线时，它才会出现在 thread_siblings_list 中。
        local online_file="/sys/devices/system/cpu/cpu${expected_sib}/online"
        if [[ -r "$online_file" ]] && [[ "$(cat "$online_file")" == "1" ]]; then
            if ! python3 - "$sib_list" "$io_cpu" "$expected_sib" <<'PY'
import sys
raw, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
cpus = set()
for part in raw.strip().split(","):
    if "-" in part:
        lo, hi = map(int, part.split("-", 1))
        cpus.update(range(lo, hi + 1))
    elif part:
        cpus.add(int(part))
if a not in cpus or b not in cpus:
    sys.exit(1)
PY
            then
                echo "错误: CPU $expected_sib 不是 CPU $io_cpu 的超线程 sibling (实际: $sib_list)" >&2
                exit 1
            fi
        fi
    done
}

# 【作用】展示 4 个 I/O P-core 及其超线程兄弟核的当前在线状态与系统在线 CPU 总览。
# 【原理】遍历 IO_CPUS 与 TARGET_SIBLINGS，读取各自 sysfs 下的 online 与 thread_siblings_list 格式化打印。
cmd_status() {
    echo "=== Tina I/O P-core 与 SMT Sibling 状态 ==="
    local idx io_cpu sib online_path state sib_list
    for idx in "${!IO_CPUS[@]}"; do
        io_cpu="${IO_CPUS[$idx]}"
        sib="${TARGET_SIBLINGS[$idx]}"
        online_path="/sys/devices/system/cpu/cpu${sib}/online"
        state="$(cat "$online_path" 2>/dev/null || echo "N/A")"
        sib_list="$(cat "/sys/devices/system/cpu/cpu${io_cpu}/topology/thread_siblings_list" 2>/dev/null || echo "N/A")"
        printf "  IO CPU %2d (siblings=%-6s) | sibling CPU %2d online=%s\n" \
            "$io_cpu" "$sib_list" "$sib" "$state"
    done
    echo "=== 系统在线 CPU 总览 ==="
    cat /sys/devices/system/cpu/online
}

# 【作用】记录目标 sibling 的原始 online 状态并安全将其全部下线（设为 0），实现 P-core 独占。
# 【原理】先校验 root 权限、拓扑配对及状态文件不存在，再把各 sibling 原始状态原子写入 state_file，
#         最后逐个向 /sys/devices/system/cpu/cpuN/online 写入 0。
cmd_offline() {
    local state_file="${1:-$DEFAULT_STATE_FILE}"
    require_root
    verify_siblings_topology

    if [[ -e "$state_file" ]]; then
        echo "错误: 状态文件 $state_file 已存在，请先 restore 避免覆盖原始 online 状态。" >&2
        exit 1
    fi

    local tmp_state
    tmp_state="$(mktemp)"
    local sib online_path orig
    for sib in "${TARGET_SIBLINGS[@]}"; do
        online_path="/sys/devices/system/cpu/cpu${sib}/online"
        if [[ ! -w "$online_path" ]]; then
            rm -f "$tmp_state"
            echo "错误: $online_path 不可写，无法下线 CPU $sib" >&2
            exit 1
        fi
        orig="$(cat "$online_path")"
        printf "%s=%s\n" "$sib" "$orig" >>"$tmp_state"
    done

    mv "$tmp_state" "$state_file"
    chmod 644 "$state_file"

    for sib in "${TARGET_SIBLINGS[@]}"; do
        online_path="/sys/devices/system/cpu/cpu${sib}/online"
        if [[ "$(cat "$online_path")" != "0" ]]; then
            echo 0 >"$online_path"
        fi
    done

    echo "已记录原始状态到 $state_file，并下线 SMT siblings: ${TARGET_SIBLINGS[*]}"
    cmd_status
}

# 【作用】严格按 offline 时保存的原始记录恢复各 sibling 的在线状态，避免误开启原本关闭的核心。
# 【原理】逐行解析 state_file，经白名单核对只允许操作 TARGET_SIBLINGS，按记录的 0/1 原值写回 sysfs
#         后删除状态文件并展示恢复结果。
cmd_restore() {
    local state_file="${1:-$DEFAULT_STATE_FILE}"
    require_root

    if [[ ! -r "$state_file" ]]; then
        echo "错误: 找不到状态文件 $state_file，无法确认下线前的原始状态。" >&2
        exit 1
    fi

    local line sib orig online_path
    while IFS='=' read -r sib orig; do
        [[ -z "$sib" ]] && continue
        # 安全白名单：只允许恢复目标 sibling 集合。
        case " ${TARGET_SIBLINGS[*]} " in
            *" $sib "*) ;;
            *)
                echo "错误: 状态文件包含非预期 CPU ID: $sib" >&2
                exit 1
                ;;
        esac
        online_path="/sys/devices/system/cpu/cpu${sib}/online"
        if [[ "$orig" == "0" || "$orig" == "1" ]]; then
            if [[ "$(cat "$online_path")" != "$orig" ]]; then
                echo "$orig" >"$online_path"
            fi
        else
            echo "警告: 跳过非法状态值 $sib=$orig" >&2
        fi
    done <"$state_file"

    rm -f "$state_file"
    echo "已按原始记录恢复 SMT siblings 状态。"
    cmd_status
}

if [[ $# -lt 1 ]]; then
    usage
    exit 1
fi

case "$1" in
    status)
        cmd_status
        ;;
    offline)
        cmd_offline "${2:-$DEFAULT_STATE_FILE}"
        ;;
    restore)
        cmd_restore "${2:-$DEFAULT_STATE_FILE}"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage
        exit 1
        ;;
esac
