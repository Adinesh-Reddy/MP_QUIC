#!/bin/bash
# ==============================================================================
# MP-QUIC Scenario 1: Automated 100-Scenario Evaluation
# Replicates ACM CoNEXT 2017: "Multipath QUIC: Design and Evaluation" (Section 4.1)
# ==============================================================================

set -e

# Default configuration
NUM_SCENARIOS=${1:-100}
OUT_CSV="results_scenario1_100.csv"
FILE="test20M.bin"

SERVER_A="10.0.2.2:4433"
SERVER_B="10.0.3.2:4433"
LOCAL_A="10.0.0.1"
LOCAL_B="10.0.1.1"

ROUTER_A="vyos@10.0.0.254"
ROUTER_B="vyos@10.0.1.254"

CLIENT_BIN="./bin/mpq_client"

if [ ! -f "$CLIENT_BIN" ]; then
    echo "[ERROR] $CLIENT_BIN not found! Run 'make' first."
    exit 1
fi

echo "======================================================================"
echo " MP-QUIC Scenario 1 Evaluation: $NUM_SCENARIOS Scenarios"
echo " (Each scenario runs 3 repetitions: Single A, Single B, Multi A, Multi B)"
echo " Output will be written to: $OUT_CSV"
echo "======================================================================"

# Write CSV Header
echo "scenario_id,rate_a_mbps,delay_a_ms,limit_a,loss_a_pct,rate_b_mbps,delay_b_ms,limit_b,loss_b_pct,g_single_a,g_single_b,g_multi_a_first,g_multi_b_first,eben_a_first,eben_b_first" > "$OUT_CSV"

# Function to run client with timeout and extract goodput_kbps
run_client() {
    local cmd="$1"
    local output
    # 30 second timeout per download to prevent hanging on loss
    output=$(timeout 35s $cmd 2>/dev/null | grep ^RESULT || true)
    local kbps
    kbps=$(echo "$output" | grep -o 'goodput_kbps=[0-9]*' | cut -d= -f2)
    if [ -z "$kbps" ]; then
        echo "0"
    else
        echo "$kbps"
    fi
}

# Function to compute median of 3 runs using Python
calc_median() {
    python3 -c "import statistics; print(int(statistics.median([$1, $2, $3])))"
}

# Function to compute EBen using the CoNEXT 2017 Page 4 formula
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

# ------------------------------------------------------------------------------
# 100 Scenarios Space-Filling Generator
# Varies all 4 factors across realistic VM ranges:
#   Capacity:  5 to 25 Mbps
#   Delay:     5 to 50 ms (RTT = 10 to 100 ms)
#   Queue:     30 to 200 packets (Queuing delay = 20 to 300 ms)
#   Loss:      0%, 0.5%, 1%, 1.5%, 2%
# ------------------------------------------------------------------------------

for s in $(seq 1 "$NUM_SCENARIOS"); do
    # Deterministic space-filling formulas (coprime modular sampling)
    # Ensures uniform dispersion without random clustering
    RATE_A=$(( ((s * 7 + 3) % 21) + 5 ))       # 5 to 25 Mbps
    DELAY_A=$(( ((s * 13 + 7) % 46) + 5 ))     # 5 to 50 ms (RTT = 10 to 100 ms)
    LIMIT_A=$(( ((s * 17 + 11) % 171) + 30 ))  # 30 to 200 packets
    LOSS_A=$(python3 -c "losses = [0, 0, 0, 0.5, 1.0, 1.5, 2.0]; print(losses[$s % len(losses)])")

    RATE_B=$(( ((s * 11 + 5) % 21) + 5 ))      # 5 to 25 Mbps
    DELAY_B=$(( ((s * 19 + 2) % 46) + 5 ))     # 5 to 50 ms
    LIMIT_B=$(( ((s * 23 + 13) % 171) + 30 ))  # 30 to 200 packets
    LOSS_B=$(python3 -c "losses = [0, 0, 0.5, 1.0, 0, 1.5, 0]; print(losses[$s % len(losses)])")

    echo ""
    echo "----------------------------------------------------------------------"
    echo ">>> Running Scenario $s / $NUM_SCENARIOS"
    echo "    Path A: ${RATE_A} Mbps, ${DELAY_A} ms delay, limit ${LIMIT_A}, ${LOSS_A}% loss"
    echo "    Path B: ${RATE_B} Mbps, ${DELAY_B} ms delay, limit ${LIMIT_B}, ${LOSS_B}% loss"
    echo "----------------------------------------------------------------------"

    # Apply network shaping to Router A via SSH
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$ROUTER_A" \
        "sudo tc qdisc replace dev eth0 root netem rate ${RATE_A}mbit delay ${DELAY_A}ms limit ${LIMIT_A} loss ${LOSS_A}% && \
         sudo tc qdisc replace dev eth1 root netem rate ${RATE_A}mbit delay ${DELAY_A}ms limit ${LIMIT_A} loss ${LOSS_A}%" || {
        echo "[WARNING] Failed to shape Router A! Check SSH connection."
    }

    # Apply network shaping to Router B via SSH
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$ROUTER_B" \
        "sudo tc qdisc replace dev eth0 root netem rate ${RATE_B}mbit delay ${DELAY_B}ms limit ${LIMIT_B} loss ${LOSS_B}% && \
         sudo tc qdisc replace dev eth1 root netem rate ${RATE_B}mbit delay ${DELAY_B}ms limit ${LIMIT_B} loss ${LOSS_B}%" || {
        echo "[WARNING] Failed to shape Router B! Check SSH connection."
    }

    sleep 1 # Allow network queues to clear

    # 1. Single Path A Only (3 runs)
    cmd_sa="$CLIENT_BIN --server $SERVER_A --local $LOCAL_A --get $FILE --out /tmp/o.bin"
    r1=$(run_client "$cmd_sa"); sleep 0.5
    r2=$(run_client "$cmd_sa"); sleep 0.5
    r3=$(run_client "$cmd_sa"); sleep 0.5
    G_A=$(calc_median "$r1" "$r2" "$r3")

    # 2. Single Path B Only (3 runs)
    cmd_sb="$CLIENT_BIN --server $SERVER_B --local $LOCAL_B --get $FILE --out /tmp/o.bin"
    r1=$(run_client "$cmd_sb"); sleep 0.5
    r2=$(run_client "$cmd_sb"); sleep 0.5
    r3=$(run_client "$cmd_sb"); sleep 0.5
    G_B=$(calc_median "$r1" "$r2" "$r3")

    # 3. Multipath (Both Paths, Path A first - 3 runs)
    cmd_ma="$CLIENT_BIN --server $SERVER_A --local $LOCAL_A --local $LOCAL_B --get $FILE --out /tmp/o.bin"
    r1=$(run_client "$cmd_ma"); sleep 0.5
    r2=$(run_client "$cmd_ma"); sleep 0.5
    r3=$(run_client "$cmd_ma"); sleep 0.5
    G_MA=$(calc_median "$r1" "$r2" "$r3")

    # 4. Multipath (Both Paths, Path B first - 3 runs)
    cmd_mb="$CLIENT_BIN --server $SERVER_B --local $LOCAL_B --local $LOCAL_A --get $FILE --out /tmp/o.bin"
    r1=$(run_client "$cmd_mb"); sleep 0.5
    r2=$(run_client "$cmd_mb"); sleep 0.5
    r3=$(run_client "$cmd_mb"); sleep 0.5
    G_MB=$(calc_median "$r1" "$r2" "$r3")

    # Calculate EBen
    EBEN_A=$(calc_eben "$G_MA" "$G_A" "$G_B")
    EBEN_B=$(calc_eben "$G_MB" "$G_A" "$G_B")

    echo "    [Results] SingleA: ${G_A}k | SingleB: ${G_B}k | Multi(A-first): ${G_MA}k (EBen=${EBEN_A}) | Multi(B-first): ${G_MB}k (EBen=${EBEN_B})"

    # Write data row to CSV
    echo "$s,$RATE_A,$DELAY_A,$LIMIT_A,$LOSS_A,$RATE_B,$DELAY_B,$LIMIT_B,$LOSS_B,$G_A,$G_B,$G_MA,$G_MB,$EBEN_A,$EBEN_B" >> "$OUT_CSV"
done

echo ""
echo "======================================================================"
echo " [SUCCESS] Benchmark finished! Completed $NUM_SCENARIOS scenarios."
echo " Raw results saved to: $OUT_CSV"
echo " Run 'python3 scripts/plot_scenario1.py' to generate Figure 3 & Figure 4!"
echo "======================================================================"
