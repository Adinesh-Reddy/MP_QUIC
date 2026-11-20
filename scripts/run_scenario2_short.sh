#!/bin/bash
# ==============================================================================
# MP-QUIC Scenario 2: Short File Downloads (256 KB) - Section 4.2 CoNEXT 2017
# Replicates Figure 9 (Completion Time CDF) & Figure 10 (EBen Boxplot)
# ==============================================================================

set -e

NUM_SCENARIOS=${1:-10}
OUT_CSV="results_scenario2_short.csv"
FILE="test256K.bin"
OUT_FILE="/tmp/test256K_download.bin"

SERVER_A_IP="10.0.2.2"
SERVER_B_IP="10.0.3.2"
LOCAL_A_IP="10.0.0.1"
LOCAL_B_IP="10.0.1.1"

ROUTER_A="vyos@10.0.0.254"
ROUTER_B="vyos@10.0.1.254"

CLIENT_BIN="./bin/mpq_client"

if [ ! -f "$CLIENT_BIN" ]; then
    echo "[ERROR] $CLIENT_BIN not found! Run 'make' first."
    exit 1
fi

echo "======================================================================"
echo " Starting Section 4.2: Short 256 KB Downloads Benchmark"
echo " Scenarios : $NUM_SCENARIOS"
echo " Output CSV: $OUT_CSV"
echo " Payload   : 256 KB ($FILE)"
echo "======================================================================"

# Write CSV Header with both completion times (ms) and goodput (kbps)
echo "scenario_id,rate_a,delay_a,rate_b,delay_b,time_tcp_a,time_tcp_b,time_mptcp_a,time_mptcp_b,time_quic_a,time_quic_b,time_mpquic_a,time_mpquic_b,g_tcp_a,g_tcp_b,g_mptcp_a,g_mptcp_b,g_quic_a,g_quic_b,g_mpquic_a,g_mpquic_b,eben_mptcp_a,eben_mptcp_b,eben_mpquic_a,eben_mpquic_b" > "$OUT_CSV"

# Function to run mpq_client for short transfer
# Returns: time_ms,goodput_kbps
run_quic_short() {
    local cmd="$1"
    local output
    output=$(timeout 20s $cmd 2>/dev/null | grep ^RESULT || true)
    local t_ms g_kbps
    t_ms=$(echo "$output" | grep -o 'time_ms=[0-9]*' | cut -d= -f2 || true)
    g_kbps=$(echo "$output" | grep -o 'goodput_kbps=[0-9]*' | cut -d= -f2 || true)
    if [ -z "$t_ms" ] || [ "$t_ms" -eq 0 ]; then t_ms="0"; fi
    if [ -z "$g_kbps" ]; then g_kbps="0"; fi
    echo "$t_ms,$g_kbps"
}

# Function to run iperf3 for short transfer (256 KB)
# Returns: time_ms,goodput_kbps
run_iperf_short() {
    local is_mptcp="$1"
    local srv_ip="$2"
    local loc_ip="$3"
    local port="$4"
    local prefix=""
    if [ "$is_mptcp" = "1" ]; then prefix="mptcpize run "; fi

    local json_out
    json_out=$(timeout 20s $prefix iperf3 -c "$srv_ip" -B "$loc_ip" -p "$port" -R -n 256K -J 2>/dev/null || true)
    
    python3 -c "
import json, sys
try:
    d = json.loads('''$json_out''')
    sec = d['end']['sum_received']['seconds']
    bps = d['end']['sum_received']['bits_per_second']
    time_ms = int(sec * 1000.0)
    g_kbps = int(bps / 1000.0)
    print(f'{time_ms},{g_kbps}')
except Exception:
    print('0,0')
"
}

calc_median_pair() {
    python3 -c "
import statistics
p1 = '$1'.split(',')
p2 = '$2'.split(',')
p3 = '$3'.split(',')
t = int(statistics.median([int(p1[0]), int(p2[0]), int(p3[0])]))
g = int(statistics.median([int(p1[1]), int(p2[1]), int(p3[1])]))
print(f'{t},{g}')
"
}

calc_eben() {
    python3 -c "
gm = float($1)
ga = float($2)
gb = float($3)
gbest = max(ga, gb)
gsum = ga + gb
if gbest == 0 or gsum == gbest:
    print('0.000')
elif gm >= gbest:
    print(f'{(gm - gbest) / (gsum - gbest):.3f}')
else:
    print(f'{(gm - gbest) / gbest:.3f}')
"
}

for s in $(seq 1 "$NUM_SCENARIOS"); do
    # Section 4.2 specifies Low-BDP scenarios without random losses (0% loss)
    # Rate: 5 to 20 Mbps, Delay: 10 to 40 ms
    RATE_A=$(( ((s * 5 + 3) % 16) + 5 ))
    DELAY_A=$(( ((s * 7 + 5) % 25) + 10 ))
    LIMIT_A=100

    RATE_B=$(( ((s * 9 + 2) % 16) + 5 ))
    DELAY_B=$(( ((s * 11 + 3) % 25) + 10 ))
    LIMIT_B=100

    echo ""
    echo "----------------------------------------------------------------------"
    echo ">>> [Scenario $s / $NUM_SCENARIOS] (256 KB Short Download)"
    echo "    Path A: ${RATE_A} Mbit/s, ${DELAY_A} ms delay (0% loss)"
    echo "    Path B: ${RATE_B} Mbit/s, ${DELAY_B} ms delay (0% loss)"
    echo "----------------------------------------------------------------------"

    # Shape routers
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$ROUTER_A" \
        "sudo tc qdisc replace dev eth0 root netem rate ${RATE_A}mbit delay ${DELAY_A}ms limit ${LIMIT_A} loss 0% && \
         sudo tc qdisc replace dev eth1 root netem rate ${RATE_A}mbit delay ${DELAY_A}ms limit ${LIMIT_A} loss 0%" 2>/dev/null || true

    ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$ROUTER_B" \
        "sudo tc qdisc replace dev eth0 root netem rate ${RATE_B}mbit delay ${DELAY_B}ms limit ${LIMIT_B} loss 0% && \
         sudo tc qdisc replace dev eth1 root netem rate ${RATE_B}mbit delay ${DELAY_B}ms limit ${LIMIT_B} loss 0%" 2>/dev/null || true

    sleep 0.5

    # 1. Single-Path TCP
    r1=$(run_iperf_short 0 "$SERVER_A_IP" "$LOCAL_A_IP" 5201); sleep 0.2
    r2=$(run_iperf_short 0 "$SERVER_A_IP" "$LOCAL_A_IP" 5201); sleep 0.2
    r3=$(run_iperf_short 0 "$SERVER_A_IP" "$LOCAL_A_IP" 5201); sleep 0.2
    pair=$(calc_median_pair "$r1" "$r2" "$r3")
    TIME_TCP_A=$(echo "$pair" | cut -d, -f1)
    G_TCP_A=$(echo "$pair" | cut -d, -f2)

    r1=$(run_iperf_short 0 "$SERVER_B_IP" "$LOCAL_B_IP" 5201); sleep 0.2
    r2=$(run_iperf_short 0 "$SERVER_B_IP" "$LOCAL_B_IP" 5201); sleep 0.2
    r3=$(run_iperf_short 0 "$SERVER_B_IP" "$LOCAL_B_IP" 5201); sleep 0.2
    pair=$(calc_median_pair "$r1" "$r2" "$r3")
    TIME_TCP_B=$(echo "$pair" | cut -d, -f1)
    G_TCP_B=$(echo "$pair" | cut -d, -f2)

    # 2. Multipath TCP
    r1=$(run_iperf_short 1 "$SERVER_A_IP" "$LOCAL_A_IP" 5202); sleep 0.2
    r2=$(run_iperf_short 1 "$SERVER_A_IP" "$LOCAL_A_IP" 5202); sleep 0.2
    r3=$(run_iperf_short 1 "$SERVER_A_IP" "$LOCAL_A_IP" 5202); sleep 0.2
    pair=$(calc_median_pair "$r1" "$r2" "$r3")
    TIME_MPTCP_A=$(echo "$pair" | cut -d, -f1)
    G_MPTCP_A=$(echo "$pair" | cut -d, -f2)

    r1=$(run_iperf_short 1 "$SERVER_B_IP" "$LOCAL_B_IP" 5202); sleep 0.2
    r2=$(run_iperf_short 1 "$SERVER_B_IP" "$LOCAL_B_IP" 5202); sleep 0.2
    r3=$(run_iperf_short 1 "$SERVER_B_IP" "$LOCAL_B_IP" 5202); sleep 0.2
    pair=$(calc_median_pair "$r1" "$r2" "$r3")
    TIME_MPTCP_B=$(echo "$pair" | cut -d, -f1)
    G_MPTCP_B=$(echo "$pair" | cut -d, -f2)

    # 3. Single-Path QUIC
    r1=$(run_quic_short "$CLIENT_BIN --server $SERVER_A_IP:4433 --local $LOCAL_A_IP --get $FILE --out $OUT_FILE"); sleep 0.2
    r2=$(run_quic_short "$CLIENT_BIN --server $SERVER_A_IP:4433 --local $LOCAL_A_IP --get $FILE --out $OUT_FILE"); sleep 0.2
    r3=$(run_quic_short "$CLIENT_BIN --server $SERVER_A_IP:4433 --local $LOCAL_A_IP --get $FILE --out $OUT_FILE"); sleep 0.2
    pair=$(calc_median_pair "$r1" "$r2" "$r3")
    TIME_QUIC_A=$(echo "$pair" | cut -d, -f1)
    G_QUIC_A=$(echo "$pair" | cut -d, -f2)

    r1=$(run_quic_short "$CLIENT_BIN --server $SERVER_B_IP:4433 --local $LOCAL_B_IP --get $FILE --out $OUT_FILE"); sleep 0.2
    r2=$(run_quic_short "$CLIENT_BIN --server $SERVER_B_IP:4433 --local $LOCAL_B_IP --get $FILE --out $OUT_FILE"); sleep 0.2
    r3=$(run_quic_short "$CLIENT_BIN --server $SERVER_B_IP:4433 --local $LOCAL_B_IP --get $FILE --out $OUT_FILE"); sleep 0.2
    pair=$(calc_median_pair "$r1" "$r2" "$r3")
    TIME_QUIC_B=$(echo "$pair" | cut -d, -f1)
    G_QUIC_B=$(echo "$pair" | cut -d, -f2)

    # 4. Multipath QUIC
    r1=$(run_quic_short "$CLIENT_BIN --server $SERVER_A_IP:4433 --local $LOCAL_A_IP --local $LOCAL_B_IP --get $FILE --out $OUT_FILE"); sleep 0.2
    r2=$(run_quic_short "$CLIENT_BIN --server $SERVER_A_IP:4433 --local $LOCAL_A_IP --local $LOCAL_B_IP --get $FILE --out $OUT_FILE"); sleep 0.2
    r3=$(run_quic_short "$CLIENT_BIN --server $SERVER_A_IP:4433 --local $LOCAL_A_IP --local $LOCAL_B_IP --get $FILE --out $OUT_FILE"); sleep 0.2
    pair=$(calc_median_pair "$r1" "$r2" "$r3")
    TIME_MPQUIC_A=$(echo "$pair" | cut -d, -f1)
    G_MPQUIC_A=$(echo "$pair" | cut -d, -f2)

    r1=$(run_quic_short "$CLIENT_BIN --server $SERVER_B_IP:4433 --local $LOCAL_B_IP --local $LOCAL_A_IP --get $FILE --out $OUT_FILE"); sleep 0.2
    r2=$(run_quic_short "$CLIENT_BIN --server $SERVER_B_IP:4433 --local $LOCAL_B_IP --local $LOCAL_A_IP --get $FILE --out $OUT_FILE"); sleep 0.2
    r3=$(run_quic_short "$CLIENT_BIN --server $SERVER_B_IP:4433 --local $LOCAL_B_IP --local $LOCAL_A_IP --get $FILE --out $OUT_FILE"); sleep 0.2
    pair=$(calc_median_pair "$r1" "$r2" "$r3")
    TIME_MPQUIC_B=$(echo "$pair" | cut -d, -f1)
    G_MPQUIC_B=$(echo "$pair" | cut -d, -f2)

    # EBen calculations based on goodput
    EBEN_MPTCP_A=$(calc_eben "$G_MPTCP_A" "$G_TCP_A" "$G_TCP_B")
    EBEN_MPTCP_B=$(calc_eben "$G_MPTCP_B" "$G_TCP_A" "$G_TCP_B")
    EBEN_MPQUIC_A=$(calc_eben "$G_MPQUIC_A" "$G_QUIC_A" "$G_QUIC_B")
    EBEN_MPQUIC_B=$(calc_eben "$G_MPQUIC_B" "$G_QUIC_A" "$G_QUIC_B")

    echo "  [Time ms] TCP A: ${TIME_TCP_A}ms | QUIC A: ${TIME_QUIC_A}ms | MPTCP A: ${TIME_MPTCP_A}ms | MP-QUIC A: ${TIME_MPQUIC_A}ms"
    echo "  [Goodput] TCP A: ${G_TCP_A}k | QUIC A: ${G_QUIC_A}k | MP-QUIC A: ${G_MPQUIC_A}k | EBen: ${EBEN_MPQUIC_A}"

    echo "$s,$RATE_A,$DELAY_A,$RATE_B,$DELAY_B,$TIME_TCP_A,$TIME_TCP_B,$TIME_MPTCP_A,$TIME_MPTCP_B,$TIME_QUIC_A,$TIME_QUIC_B,$TIME_MPQUIC_A,$TIME_MPQUIC_B,$G_TCP_A,$G_TCP_B,$G_MPTCP_A,$G_MPTCP_B,$G_QUIC_A,$G_QUIC_B,$G_MPQUIC_A,$G_MPQUIC_B,$EBEN_MPTCP_A,$EBEN_MPTCP_B,$EBEN_MPQUIC_A,$EBEN_MPQUIC_B" >> "$OUT_CSV"
done

echo ""
echo "======================================================================"
echo "[SUCCESS] Section 4.2 Benchmark Completed! Results saved to $OUT_CSV"
echo "Run: python3 scripts/plot_scenario2_short.py $OUT_CSV"
echo "======================================================================"
