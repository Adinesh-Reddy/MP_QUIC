#!/bin/bash
# ==============================================================================
# evaluation/scripts/run_benchmarks.sh
# Runs benchmarking suite for a specific scenario on Client VM.
# Generates per-scenario CSV results and formatted summary tables.
#
# Supported Scenarios:
#   scenario1 (or 1, low_bdp)    : Low BDP / Asymmetric (8M/10ms + 4M/25ms)
#   scenario2 (or 2, lossy)      : Low BDP with 1% random packet loss
#   scenario3 (or 3, high_bdp)   : High BDP / Long Delay (20M/100ms + 5M/150ms)
#   scenario4 (or 4, short)      : Short Transfer (256 KB file, test256K.bin)
#
# Usage:
#   ./run_benchmarks.sh --scenario scenario1 [--runs 3] [--file test20M.bin] [--stats]
#   ./run_benchmarks.sh --scenario 2
#   ./run_benchmarks.sh --scenario 3
#   ./run_benchmarks.sh --scenario 4
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/config.sh"

RAW_SCENARIO="scenario1"
RUNS=3
FILE=""
TIMEOUT=300
WITH_STATS=1
WITH_BASELINES=1

while [ $# -gt 0 ]; do
    case "$1" in
        --scenario) RAW_SCENARIO="$2"; shift 2 ;;
        --runs) RUNS="$2"; shift 2 ;;
        --file) FILE="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --stats) WITH_STATS=1; shift ;;
        --no-stats) WITH_STATS=0; shift ;;
        --with-baselines) WITH_BASELINES=1; shift ;;
        --skip-baselines) WITH_BASELINES=0; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# Standardize scenario name
case "$RAW_SCENARIO" in
    1|scenario1|low_bdp|default)
        SCENARIO="scenario1"
        SCENARIO_DESC="Scenario 1: Low BDP / Asymmetric (8M/10ms + 4M/25ms)"
        [ -z "$FILE" ] && FILE="test20M.bin"
        ;;
    2|scenario2|lossy)
        SCENARIO="scenario2"
        SCENARIO_DESC="Scenario 2: Low BDP with 1% Loss"
        [ -z "$FILE" ] && FILE="test20M.bin"
        ;;
    3|scenario3|high_bdp)
        SCENARIO="scenario3"
        SCENARIO_DESC="Scenario 3: High BDP / Long Delay (20M/100ms + 5M/150ms)"
        [ -z "$FILE" ] && FILE="test20M.bin"
        ;;
    4|scenario4|short|short_transfer)
        SCENARIO="scenario4"
        SCENARIO_DESC="Scenario 4: Short 256KB Transfer (Web Object Simulation)"
        [ -z "$FILE" ] && FILE="test256K.bin"
        ;;
    *)
        SCENARIO="$RAW_SCENARIO"
        SCENARIO_DESC="Scenario: $RAW_SCENARIO"
        [ -z "$FILE" ] && FILE="test20M.bin"
        ;;
esac

detect_role >/dev/null 2>&1
mkdir -p "$RESULTS" "$LOGS"

CSV_OUT="$RESULTS/results_${SCENARIO}.csv"
TABLE_OUT="$RESULTS/table_${SCENARIO}.txt"
CLIENT_LOG="$LOGS/client_${SCENARIO}.log"

echo "========================================================================"
echo "          MP-QUIC BENCHMARK SUITE : $SCENARIO_DESC               "
echo "========================================================================"
echo "Scenario ID   : $SCENARIO"
echo "Payload File  : $FILE"
echo "Repetitions   : $RUNS runs per configuration"
echo "CSV Output    : $CSV_OUT"
echo "Table Output  : $TABLE_OUT"
echo "Log File      : $CLIENT_LOG"
echo "========================================================================"

# Initialize CSV header
echo "scenario,protocol,config,run,bytes,time_ms,goodput_kbps,rx_a_bytes,rx_b_bytes,sha256_match,status" > "$CSV_OUT"

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# Fetch expected hash from server
"$BIN/mpq_client" --server "$SERVER_A:$PORT" --local "$CLIENT_A" --get "$FILE.sha256" \
                  --out "$TMP_DIR/expected.sha256" --timeout 20 > "$TMP_DIR/probe.log" 2>&1

if [ ! -s "$TMP_DIR/expected.sha256" ]; then
    echo "[ERROR] Cannot fetch $FILE.sha256 from server ($SERVER_A:$PORT)."
    echo "Make sure ./start_server.sh is running on the Server VM."
    cat "$TMP_DIR/probe.log"
    exit 1
fi
EXPECTED_SHA=$(tr -d ' \r\n' < "$TMP_DIR/expected.sha256")
echo "Verified expected SHA-256 for $FILE: $EXPECTED_SHA"
echo ""

# Configuration argument generator for mpq_client
get_mpq_args() {
    case "$1" in
        multipath_a_first) echo "--server $SERVER_A:$PORT --local $CLIENT_A --local $CLIENT_B" ;;
        multipath_b_first) echo "--server $SERVER_B:$PORT --local $CLIENT_B --local $CLIENT_A" ;;
        single_a)          echo "--server $SERVER_A:$PORT --local $CLIENT_A" ;;
        single_b)          echo "--server $SERVER_B:$PORT --local $CLIENT_B" ;;
    esac
}

CONFIGS="multipath_a_first multipath_b_first single_a single_b"

echo ">>> Phase 1: Running MP-QUIC Transfers <<<" | tee -a "$CLIENT_LOG"
for run in $(seq 1 "$RUNS"); do
    for cfg in $CONFIGS; do
        CLI_ARGS=$(get_mpq_args "$cfg")
        STATS_ARG=""
        if [ "$WITH_STATS" -eq 1 ]; then
            STATS_ARG="--stats $RESULTS/stats_${SCENARIO}_${cfg}_run${run}.csv"
        fi

        RX_A_PRE=$(rx_bytes "$IF_A")
        RX_B_PRE=$(rx_bytes "$IF_B")

        # Execute client transfer
        # shellcheck disable=SC2086
        "$BIN/mpq_client" $CLI_ARGS --get "$FILE" --out "$TMP_DIR/out.bin" \
                          $STATS_ARG --timeout "$TIMEOUT" > "$TMP_DIR/run.log" 2>&1
        RET=$?

        RX_A_POST=$(rx_bytes "$IF_A")
        RX_B_POST=$(rx_bytes "$IF_B")
        DIFF_A=$(( RX_A_POST - RX_A_PRE ))
        DIFF_B=$(( RX_B_POST - RX_B_PRE ))

        RES_LINE=$(grep "^RESULT" "$TMP_DIR/run.log" | tail -n 1)
        ACTUAL_SHA=$(sha256sum < "$TMP_DIR/out.bin" 2>/dev/null | cut -d' ' -f1)

        BYTES=$(echo "$RES_LINE" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')
        TIME_MS=$(echo "$RES_LINE" | sed -n 's/.*time_ms=\([0-9]*\).*/\1/p')
        GOODPUT=$(echo "$RES_LINE" | sed -n 's/.*goodput_kbps=\([0-9]*\).*/\1/p')
        [ -z "$BYTES" ] && BYTES=0
        [ -z "$TIME_MS" ] && TIME_MS=0
        [ -z "$GOODPUT" ] && GOODPUT=0

        SHA_MATCH="no"
        if [ "$RET" -eq 0 ] && [ "$ACTUAL_SHA" = "$EXPECTED_SHA" ]; then
            SHA_MATCH="yes"
            STATUS_STR="ok"
            printf "[Run %d/%d] MP-QUIC %-18s -> %7d kbps (%5.2f Mbps) | Time: %5d ms | Link A: %4d KB, Link B: %4d KB | SHA: OK\n" \
                   "$run" "$RUNS" "$cfg" "$GOODPUT" "$(awk "BEGIN {printf \"%.2f\", $GOODPUT/1000}")" \
                   "$TIME_MS" "$((DIFF_A/1024))" "$((DIFF_B/1024))" | tee -a "$CLIENT_LOG"
        else
            STATUS_STR="failed"
            printf "[Run %d/%d] MP-QUIC %-18s -> FAILED (Exit: %d)\n" "$run" "$RUNS" "$cfg" "$RET" | tee -a "$CLIENT_LOG"
        fi

        echo "$SCENARIO,mpquic,$cfg,$run,$BYTES,$TIME_MS,$GOODPUT,$DIFF_A,$DIFF_B,$SHA_MATCH,$STATUS_STR" >> "$CSV_OUT"
        sleep 1
    done
done

# Phase 2: Optional Baselines (TCP, UDP, MPTCP via iperf3)
if [ "$WITH_BASELINES" -eq 1 ] && command -v "$IPERF3" >/dev/null 2>&1; then
    echo "" | tee -a "$CLIENT_LOG"
    echo ">>> Phase 2: Running Comparative Baselines (iperf3) <<<" | tee -a "$CLIENT_LOG"

    run_iperf_test() {
        local proto="$1"
        local cfg="$2"
        local run_idx="$3"
        shift 3

        local rx_a_0=$(rx_bytes "$IF_A")
        local rx_b_0=$(rx_bytes "$IF_B")

        "$@" -i 0.1 --json > "$TMP_DIR/iperf.json" 2> "$TMP_DIR/iperf.err"
        local rx_a_1=$(rx_bytes "$IF_A")
        local rx_b_1=$(rx_bytes "$IF_B")
        local d_a=$(( rx_a_1 - rx_a_0 ))
        local d_b=$(( rx_b_1 - rx_b_0 ))

        python3 - "$TMP_DIR/iperf.json" <<'EOF' > "$TMP_DIR/iperf_parsed.txt" 2>/dev/null
import json, sys
try:
    with open(sys.argv[1]) as f:
        data = json.load(f)
    end = data["end"]
    rec = end["sum_received"]
    b = rec["bytes"]
    s = rec["seconds"]
    kbps = int(b * 8 / s / 1000)
    ms = int(s * 1000)
    print(f"{b} {ms} {kbps}")
except Exception:
    print("0 0 0")
EOF
        read -r B_REC MS_REC KBPS_REC < "$TMP_DIR/iperf_parsed.txt"
        local st="failed"
        if [ "$KBPS_REC" -gt 0 ]; then
            st="ok"
            printf "[Run %d/%d] %-7s %-18s -> %7d kbps (%5.2f Mbps) | Time: %5d ms\n" \
                   "$run_idx" "$RUNS" "$proto" "$cfg" "$KBPS_REC" "$(awk "BEGIN {printf \"%.2f\", $KBPS_REC/1000}")" "$MS_REC" | tee -a "$CLIENT_LOG"
        else
            printf "[Run %d/%d] %-7s %-18s -> FAILED\n" "$run_idx" "$RUNS" "$proto" "$cfg" | tee -a "$CLIENT_LOG"
        fi
        echo "$SCENARIO,$proto,$cfg,$run_idx,$B_REC,$MS_REC,$KBPS_REC,$d_a,$d_b,N/A,$st" >> "$CSV_OUT"
        sleep 1
    }

    IPERF_SIZE="20M"
    [ "$SCENARIO" = "scenario4" ] && IPERF_SIZE="256K"

    for run in $(seq 1 "$RUNS"); do
        # Single-path TCP
        run_iperf_test "tcp" "single_a" "$run" "$IPERF3" -c "$SERVER_A" -B "$CLIENT_A" -p "$IPERF_PORT" -R -n "$IPERF_SIZE"
        run_iperf_test "tcp" "single_b" "$run" "$IPERF3" -c "$SERVER_B" -B "$CLIENT_B" -p "$IPERF_PORT" -R -n "$IPERF_SIZE"
        # Single-path UDP capacity
        run_iperf_test "udp" "single_a" "$run" "$IPERF3" -c "$SERVER_A" -B "$CLIENT_A" -p "$IPERF_PORT" -R -u -b 20M -t 10
        run_iperf_test "udp" "single_b" "$run" "$IPERF3" -c "$SERVER_B" -B "$CLIENT_B" -p "$IPERF_PORT" -R -u -b 20M -t 10

        # MPTCP if supported
        if command -v ${MPTCPIZE%% *} >/dev/null 2>&1 && sysctl -n net.mptcp.enabled 2>/dev/null | grep -q 1; then
            run_iperf_test "mptcp" "multipath_a_first" "$run" $MPTCPIZE "$IPERF3" -c "$SERVER_A" -B "$CLIENT_A" -p "$MPTCP_PORT" -R -n "$IPERF_SIZE"
            run_iperf_test "mptcp" "multipath_b_first" "$run" $MPTCPIZE "$IPERF3" -c "$SERVER_B" -B "$CLIENT_B" -p "$MPTCP_PORT" -R -n "$IPERF_SIZE"
        fi
    done
fi

echo ""
echo ">>> Phase 3: Generating Formatted Scenario Results Table <<<" | tee -a "$CLIENT_LOG"

# Python summary processor to compute medians, EBen, and format ASCII table
python3 - "$CSV_OUT" "$SCENARIO" "$SCENARIO_DESC" > "$TABLE_OUT" <<'EOF'
import sys, csv, statistics

csv_path = sys.argv[1]
scenario = sys.argv[2]
scenario_desc = sys.argv[3] if len(sys.argv) > 3 else scenario

data = {}

with open(csv_path, 'r') as f:
    reader = csv.DictReader(f)
    for row in reader:
        if row.get('status') != 'ok':
            continue
        proto = row['protocol']
        cfg = row['config']
        key = (proto, cfg)
        goodput = float(row['goodput_kbps'])
        time_ms = float(row['time_ms'])
        rx_a = float(row['rx_a_bytes'])
        rx_b = float(row['rx_b_bytes'])

        if key not in data:
            data[key] = {'goodput': [], 'time': [], 'rx_a': 0.0, 'rx_b': 0.0}
        data[key]['goodput'].append(goodput)
        data[key]['time'].append(time_ms)
        data[key]['rx_a'] += rx_a
        data[key]['rx_b'] += rx_b

def median(arr):
    return statistics.median(arr) if arr else 0.0

def calc_eben(gm, ga, gb):
    gmax = max(ga, gb)
    if gm >= gmax:
        denom = (ga + gb - gmax)
        return (gm - gmax) / denom if denom > 0 else 0.0
    else:
        return (gm - gmax) / gmax if gmax > 0 else 0.0

keys_order = [
    # 1. UDP Capacity Ceiling
    ('udp', 'single_a', 'UDP Capacity Link A'),
    ('udp', 'single_b', 'UDP Capacity Link B'),
    # 2. TCP Baseline
    ('tcp', 'single_a', 'TCP Single Path A'),
    ('tcp', 'single_b', 'TCP Single Path B'),
    # 3. Multipath TCP
    ('mptcp', 'multipath_a_first', 'MPTCP Both (Subflow A 1st)'),
    ('mptcp', 'multipath_b_first', 'MPTCP Both (Subflow B 1st)'),
    # 4. MP-QUIC
    ('mpquic', 'single_a', 'MP-QUIC Single Path A'),
    ('mpquic', 'single_b', 'MP-QUIC Single Path B'),
    ('mpquic', 'multipath_a_first', 'MP-QUIC Both (Path A first)'),
    ('mpquic', 'multipath_b_first', 'MP-QUIC Both (Path B first)'),
]

g_a = median(data.get(('mpquic', 'single_a'), {}).get('goodput', []))
g_b = median(data.get(('mpquic', 'single_b'), {}).get('goodput', []))
g_tcp_a = median(data.get(('tcp', 'single_a'), {}).get('goodput', []))
g_tcp_b = median(data.get(('tcp', 'single_b'), {}).get('goodput', []))
udp_a = median(data.get(('udp', 'single_a'), {}).get('goodput', []))
udp_b = median(data.get(('udp', 'single_b'), {}).get('goodput', []))

print("+" + "-"*96 + "+")
print(f"|  4-PROTOCOL BENCHMARK RESULTS TABLE - {scenario_desc:<55} |")
print("+" + "-"*96 + "+")
print(f"| {'Protocol':<10} | {'Configuration':<26} | {'Runs':<4} | {'Goodput':<12} | {'Link A / B (%)':<14} | {'EBen':<5} |")
print("+" + "="*96 + "+")

last_proto = None
for proto, cfg, label in keys_order:
    if (proto, cfg) not in data:
        continue
    if last_proto is not None and proto != last_proto:
        print("|" + "-"*12 + "+" + "-"*28 + "+" + "-"*6 + "+" + "-"*14 + "+" + "-"*16 + "+" + "-"*7 + "|")
    last_proto = proto

    entry = data[(proto, cfg)]
    m_gp = median(entry['goodput'])
    m_mbps = m_gp / 1000.0
    runs_count = len(entry['goodput'])
    tot_rx = entry['rx_a'] + entry['rx_b']
    if tot_rx > 0:
        pct_a = (entry['rx_a'] / tot_rx) * 100.0
        pct_b = (entry['rx_b'] / tot_rx) * 100.0
        dist_str = f"{pct_a:4.1f}% / {pct_b:4.1f}%"
    else:
        dist_str = "  N/A  "

    eben_str = "  -  "
    if proto == 'mpquic' and 'multipath' in cfg and g_a > 0 and g_b > 0:
        eben_val = calc_eben(m_gp, g_a, g_b)
        eben_str = f"{eben_val:5.2f}"
    elif proto == 'mptcp' and g_tcp_a > 0 and g_tcp_b > 0:
        eben_val = calc_eben(m_gp, g_tcp_a, g_tcp_b)
        eben_str = f"{eben_val:5.2f}"

    p_name = proto.upper() if proto != 'mpquic' else 'MP-QUIC'
    cfg_name = cfg.replace('_', ' ').title()
    print(f"| {p_name:<10} | {label:<26} | {runs_count:<4} | {m_mbps:8.2f} Mbps | {dist_str:<14} | {eben_str:<5} |")

if udp_a > 0 and udp_b > 0:
    print("|" + "-"*12 + "+" + "-"*28 + "+" + "-"*6 + "+" + "-"*14 + "+" + "-"*16 + "+" + "-"*7 + "|")
    tot_udp = (udp_a + udp_b) / 1000.0
    pct_u_a = (udp_a / (udp_a + udp_b)) * 100.0
    pct_u_b = 100.0 - pct_u_a
    print(f"| {'UDP CAP':<10} | {'Total Path Ceiling (A+B)':<26} | {'-':<4} | {tot_udp:8.2f} Mbps | {pct_u_a:4.1f}% / {pct_u_b:4.1f}% | {'1.00':<5} |")

print("+" + "-"*96 + "+")
print("| EBen: 1.00 = perfect bandwidth aggregation (using sum of both paths)                           |")
print("|       0.00 = no gain over best single path (single-path equivalent)                            |")
print("+" + "-"*96 + "+")
EOF

# Display and log table
cat "$TABLE_OUT" | tee -a "$CLIENT_LOG"
echo ""
echo "Summary written to:"
echo "  Table : $TABLE_OUT"
echo "  CSV   : $CSV_OUT"
echo "  Log   : $CLIENT_LOG"
