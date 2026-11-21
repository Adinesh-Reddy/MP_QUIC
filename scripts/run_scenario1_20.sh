#!/bin/bash
# ==============================================================================
# MP-QUIC Scenario 1: Complete 4-Protocol Automated Benchmark (20 Scenarios)
# Evaluates TCP, MPTCP, Single-Path QUIC, and MP-QUIC
# ==============================================================================

set -e

NUM_SCENARIOS=${1:-20}
OUT_CSV="results_scenario1_20.csv"
FILE="test20M.bin"

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
echo " Starting 4-Way Evaluation: TCP vs QUIC & MPTCP vs MPQUIC"
echo " Scenarios: $NUM_SCENARIOS"
echo " Results will be saved to: $OUT_CSV"
echo "======================================================================"

# Write CSV Header with all 4 protocols
echo "scenario_id,rate_a,delay_a,loss_a,rate_b,delay_b,loss_b,tcp_a,tcp_b,mptcp_a_first,mptcp_b_first,quic_a,quic_b,mpquic_a_first,mpquic_b_first,eben_mptcp_a,eben_mptcp_b,eben_mpquic_a,eben_mpquic_b" > "$OUT_CSV"

# Function to run mpq_client and extract goodput in kbps
run_quic() {
    local cmd="$1"
    local output
    # Pass --timeout 180 directly to mpq_client so it closes cleanly (sends CONNECTION_CLOSE)
    # instead of being killed by SIGTERM and leaving a 30s zombie on mpq_server
    output=$(timeout 195s $cmd --timeout 180 2>/dev/null | grep ^RESULT || true)
    local kbps
    kbps=$(echo "$output" | grep -o 'goodput_kbps=[0-9]*' | cut -d= -f2)
    if [ -z "$kbps" ]; then echo "0"; else echo "$kbps"; fi
}

# Function to run iperf3 / mptcpize and extract receiver goodput in kbps via JSON
run_iperf() {
    local is_mptcp="$1"
    local srv_ip="$2"
    local loc_ip="$3"
    local port="$4"
    local prefix=""
    if [ "$is_mptcp" = "1" ]; then prefix="mptcpize run "; fi

    local json_out
    json_out=$(timeout 180s $prefix iperf3 -c "$srv_ip" -B "$loc_ip" -p "$port" -R -n 20M -J 2>/dev/null || true)

    printf '%s' "$json_out" | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
    bps = d['end']['sum_received']['bits_per_second']
    print(int(bps / 1000.0))
except Exception:
    print('0')
"
}

calc_median() {
    python3 -c "import statistics; print(int(statistics.median([$1, $2, $3])))"
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
    # 4 Path Factors:
    # 1. Capacity: 5 to 25 Mbps
    # 2. Base one-way delay: 5 to 50 ms (RTT = 10 to 100 ms)
    # 3. Queuing delay: 10 to 100 ms (buffer capacity = BDP + queue backlog)
    # 4. Random loss: 0 to 2%
    RATE_A=$(( ((s * 7 + 3) % 21) + 5 ))
    DELAY_A=$(( ((s * 13 + 7) % 46) + 5 ))
    QDELAY_A=$(( ((s * 17 + 11) % 91) + 10 ))
    # Router queue limit (packets) = (Rate_Mbps * (2*Delay_ms + Qdelay_ms)) / 12
    # Ensure a safe floor of at least 150 packets to avoid artificial buffer starvation
    CALC_LIMIT_A=$(( (RATE_A * (2 * DELAY_A + QDELAY_A) + 11) / 12 ))
    LIMIT_A=$(( CALC_LIMIT_A > 150 ? CALC_LIMIT_A : 150 ))
    LOSS_A=$(python3 -c "losses = [0, 0, 0, 0.5, 1.0, 1.5, 2.0]; print(losses[$s % 7])")

    RATE_B=$(( ((s * 11 + 5) % 21) + 5 ))
    DELAY_B=$(( ((s * 19 + 2) % 46) + 5 ))
    QDELAY_B=$(( ((s * 23 + 13) % 91) + 10 ))
    CALC_LIMIT_B=$(( (RATE_B * (2 * DELAY_B + QDELAY_B) + 11) / 12 ))
    LIMIT_B=$(( CALC_LIMIT_B > 150 ? CALC_LIMIT_B : 150 ))
    LOSS_B=$(python3 -c "losses = [0, 0, 0.5, 1.0, 0, 1.5, 0]; print(losses[$s % 7])")

    echo ""
    echo "----------------------------------------------------------------------"
    echo ">>> [Scenario $s / $NUM_SCENARIOS]"
    echo "    Path A: ${RATE_A}Mbit/s, ${DELAY_A}ms, limit ${LIMIT_A}, ${LOSS_A}% loss"
    echo "    Path B: ${RATE_B}Mbit/s, ${DELAY_B}ms, limit ${LIMIT_B}, ${LOSS_B}% loss"
    echo "----------------------------------------------------------------------"

    # Shape routers
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$ROUTER_A" \
        "sudo tc qdisc replace dev eth0 root netem rate ${RATE_A}mbit delay ${DELAY_A}ms limit ${LIMIT_A} loss ${LOSS_A}% && \
         sudo tc qdisc replace dev eth1 root netem rate ${RATE_A}mbit delay ${DELAY_A}ms limit ${LIMIT_A} loss ${LOSS_A}%" || {
        echo "    [WARNING] Failed to shape Router A!"
    }

    ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$ROUTER_B" \
        "sudo tc qdisc replace dev eth0 root netem rate ${RATE_B}mbit delay ${DELAY_B}ms limit ${LIMIT_B} loss ${LOSS_B}% && \
         sudo tc qdisc replace dev eth1 root netem rate ${RATE_B}mbit delay ${DELAY_B}ms limit ${LIMIT_B} loss ${LOSS_B}%" || {
        echo "    [WARNING] Failed to shape Router B!"
    }

    sleep 1

    # ------------------ 1. SINGLE-PATH TCP (Port 5201) ------------------
    t1=$(run_iperf 0 "$SERVER_A_IP" "$LOCAL_A_IP" 5201); sleep 0.3
    t2=$(run_iperf 0 "$SERVER_A_IP" "$LOCAL_A_IP" 5201); sleep 0.3
    t3=$(run_iperf 0 "$SERVER_A_IP" "$LOCAL_A_IP" 5201); sleep 0.3
    TCP_A=$(calc_median "$t1" "$t2" "$t3")

    t1=$(run_iperf 0 "$SERVER_B_IP" "$LOCAL_B_IP" 5201); sleep 0.3
    t2=$(run_iperf 0 "$SERVER_B_IP" "$LOCAL_B_IP" 5201); sleep 0.3
    t3=$(run_iperf 0 "$SERVER_B_IP" "$LOCAL_B_IP" 5201); sleep 0.3
    TCP_B=$(calc_median "$t1" "$t2" "$t3")

    # ------------------ 2. MULTIPATH TCP (Port 5202) ------------------
    t1=$(run_iperf 1 "$SERVER_A_IP" "$LOCAL_A_IP" 5202); sleep 0.3
    t2=$(run_iperf 1 "$SERVER_A_IP" "$LOCAL_A_IP" 5202); sleep 0.3
    t3=$(run_iperf 1 "$SERVER_A_IP" "$LOCAL_A_IP" 5202); sleep 0.3
    MPTCP_A=$(calc_median "$t1" "$t2" "$t3")

    t1=$(run_iperf 1 "$SERVER_B_IP" "$LOCAL_B_IP" 5202); sleep 0.3
    t2=$(run_iperf 1 "$SERVER_B_IP" "$LOCAL_B_IP" 5202); sleep 0.3
    t3=$(run_iperf 1 "$SERVER_B_IP" "$LOCAL_B_IP" 5202); sleep 0.3
    MPTCP_B=$(calc_median "$t1" "$t2" "$t3")

    # ------------------ 3. SINGLE-PATH QUIC (Port 4433) ------------------
    cmd_qa="$CLIENT_BIN --server $SERVER_A_IP:4433 --local $LOCAL_A_IP --get $FILE --out /tmp/o.bin"
    q1=$(run_quic "$cmd_qa"); sleep 0.3
    q2=$(run_quic "$cmd_qa"); sleep 0.3
    q3=$(run_quic "$cmd_qa"); sleep 0.3
    QUIC_A=$(calc_median "$q1" "$q2" "$q3")

    cmd_qb="$CLIENT_BIN --server $SERVER_B_IP:4433 --local $LOCAL_B_IP --get $FILE --out /tmp/o.bin"
    q1=$(run_quic "$cmd_qb"); sleep 0.3
    q2=$(run_quic "$cmd_qb"); sleep 0.3
    q3=$(run_quic "$cmd_qb"); sleep 0.3
    QUIC_B=$(calc_median "$q1" "$q2" "$q3")

    # ------------------ 4. MULTIPATH QUIC (Port 4433) ------------------
    cmd_mqa="$CLIENT_BIN --server $SERVER_A_IP:4433 --local $LOCAL_A_IP --local $LOCAL_B_IP --get $FILE --out /tmp/o.bin"
    q1=$(run_quic "$cmd_mqa"); sleep 0.3
    q2=$(run_quic "$cmd_mqa"); sleep 0.3
    q3=$(run_quic "$cmd_mqa"); sleep 0.3
    MPQUIC_A=$(calc_median "$q1" "$q2" "$q3")

    cmd_mqb="$CLIENT_BIN --server $SERVER_B_IP:4433 --local $LOCAL_B_IP --local $LOCAL_A_IP --get $FILE --out /tmp/o.bin"
    q1=$(run_quic "$cmd_mqb"); sleep 0.3
    q2=$(run_quic "$cmd_mqb"); sleep 0.3
    q3=$(run_quic "$cmd_mqb"); sleep 0.3
    MPQUIC_B=$(calc_median "$q1" "$q2" "$q3")

    # Calculate EBen for MPTCP and MPQUIC
    EBEN_MPTCP_A=$(calc_eben "$MPTCP_A" "$TCP_A" "$TCP_B")
    EBEN_MPTCP_B=$(calc_eben "$MPTCP_B" "$TCP_A" "$TCP_B")

    EBEN_MPQUIC_A=$(calc_eben "$MPQUIC_A" "$QUIC_A" "$QUIC_B")
    EBEN_MPQUIC_B=$(calc_eben "$MPQUIC_B" "$QUIC_A" "$QUIC_B")

    echo "    [TCP / MPTCP] TCP_A: ${TCP_A}k | TCP_B: ${TCP_B}k | MPTCP(A): ${MPTCP_A}k | MPTCP(B): ${MPTCP_B}k"
    echo "    [QUIC/MPQUIC] QUIC_A: ${QUIC_A}k | QUIC_B: ${QUIC_B}k | MPQUIC(A): ${MPQUIC_A}k | MPQUIC(B): ${MPQUIC_B}k"
    echo "    [EBen Scores] MPTCP(A)=$EBEN_MPTCP_A, MPTCP(B)=$EBEN_MPTCP_B | MPQUIC(A)=$EBEN_MPQUIC_A, MPQUIC(B)=$EBEN_MPQUIC_B"

    echo "$s,$RATE_A,$DELAY_A,$LOSS_A,$RATE_B,$DELAY_B,$LOSS_B,$TCP_A,$TCP_B,$MPTCP_A,$MPTCP_B,$QUIC_A,$QUIC_B,$MPQUIC_A,$MPQUIC_B,$EBEN_MPTCP_A,$EBEN_MPTCP_B,$EBEN_MPQUIC_A,$EBEN_MPQUIC_B" >> "$OUT_CSV"
done

echo ""
echo "======================================================================"
echo " Complete! All 4 protocols tested across $NUM_SCENARIOS scenarios."
echo " Output saved to: $OUT_CSV"
echo " Run 'python3 scripts/plot_scenario1.py' to generate the plots!"
echo "======================================================================"
