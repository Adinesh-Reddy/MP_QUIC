#!/bin/bash
# Scenario 3: Active handover and path failure test

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SERVER_A_IP="10.0.2.2"
LOCAL_A_IP="10.0.0.1"
LOCAL_B_IP="10.0.1.1"

ROUTER_A="vyos@10.0.0.254"
ROUTER_B="vyos@10.0.1.254"

REQRES_BIN="$BASE_DIR/bin/mpq_reqres"
OUT_CSV="$SCRIPT_DIR/results_scenario3_handover.csv"
SINGLE_OUT_CSV="$SCRIPT_DIR/results_scenario3_single_fail.csv"

if [ ! -f "$REQRES_BIN" ]; then
    echo "[INFO] Compiling bin/mpq_reqres..."
    make -C "$BASE_DIR" bin/mpq_reqres
fi

# Detect interface of LOCAL_A_IP
IF_A=$(ip -o -4 addr show 2>/dev/null | awk -v wanted="$LOCAL_A_IP" '{split($4, a, "/"); if (a[1] == wanted) { print $2; exit }}')
if [ -z "$IF_A" ]; then
    IF_A="eth0"
fi

cleanup() {
    echo "[CLEANUP] Restoring traffic control filters on $IF_A..."
    sudo tc qdisc del dev "$IF_A" clsact 2>/dev/null || true
    # Restore Router A shaping
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=3 "$ROUTER_A" \
        "sudo tc qdisc replace dev eth0 root netem rate 8mbit delay 10ms limit 50 && \
         sudo tc qdisc replace dev eth1 root netem rate 8mbit delay 10ms limit 50" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "Starting Scenario 3: Handover Benchmark"
echo "Path A: 8 Mbit/s, 10 ms delay | Path B: 4 Mbit/s, 25 ms delay"
echo "Output CSV: $OUT_CSV"

# 1. Shape routers
echo "[1/4] Applying link profiles on Router A and Router B..."
ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=3 "$ROUTER_A" \
    "sudo tc qdisc replace dev eth0 root netem rate 8mbit delay 10ms limit 50 && \
     sudo tc qdisc replace dev eth1 root netem rate 8mbit delay 10ms limit 50" 2>/dev/null || true

ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=3 "$ROUTER_B" \
    "sudo tc qdisc replace dev eth0 root netem rate 4mbit delay 25ms limit 50 && \
     sudo tc qdisc replace dev eth1 root netem rate 4mbit delay 25ms limit 50" 2>/dev/null || true

sudo tc qdisc del dev "$IF_A" clsact 2>/dev/null || true
sleep 1

# 2. Run MP-QUIC handover test
echo ""
echo "[2/4] Running MP-QUIC handover flow (35 requests, 750B every 400ms)..."
"$REQRES_BIN" --server "$SERVER_A_IP:4433" --local "$LOCAL_A_IP" --local "$LOCAL_B_IP" \
              --interval-ms 400 --size 750 --count 35 --out "$OUT_CSV" &
PID_MPQ=$!

echo "  Sending requests over Path A..."
sleep 3.2

# Cut Path A
echo "  Cutting Path A at t=3.2s on $IF_A"
sudo tc qdisc add dev "$IF_A" clsact 2>/dev/null || true
sudo tc filter add dev "$IF_A" egress matchall action drop 2>/dev/null || true
sudo tc filter add dev "$IF_A" ingress matchall action drop 2>/dev/null || true

ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=2 "$ROUTER_A" \
    "sudo tc qdisc replace dev eth0 root netem loss 100%" 2>/dev/null || true

wait $PID_MPQ || true
echo "MP-QUIC handover finished. Saved to $OUT_CSV"

# 3. Restore filter before single-path test
sudo tc qdisc del dev "$IF_A" clsact 2>/dev/null || true
ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=3 "$ROUTER_A" \
    "sudo tc qdisc replace dev eth0 root netem rate 8mbit delay 10ms limit 50 && \
     sudo tc qdisc replace dev eth1 root netem rate 8mbit delay 10ms limit 50" 2>/dev/null || true
sleep 1

# 4. Run Single-Path QUIC on Path A with the same link cut
echo ""
echo "[3/4] Running single-path baseline..."
"$REQRES_BIN" --server "$SERVER_A_IP:4433" --local "$LOCAL_A_IP" \
              --interval-ms 400 --size 750 --count 35 --out "$SINGLE_OUT_CSV" &
PID_SINGLE=$!
sleep 3.2

echo "  Cutting Path A for single-path run..."
sudo tc qdisc add dev "$IF_A" clsact 2>/dev/null || true
sudo tc filter add dev "$IF_A" egress matchall action drop 2>/dev/null || true
sudo tc filter add dev "$IF_A" ingress matchall action drop 2>/dev/null || true

wait $PID_SINGLE || true
echo "Single-path run finished. Saved to $SINGLE_OUT_CSV"

# 5. Restore network
cleanup

echo ""
echo "Scenario 3 finished. Generating plots..."
python3 "$SCRIPT_DIR/plot_scenario3_handover.py" "$OUT_CSV" "$SINGLE_OUT_CSV"
