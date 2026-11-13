#!/bin/bash
# The other protocols on the same links, to compare with MP-QUIC. Measured with iperf3,
# the server sends and the client receives (as in a download).
# Runs on the client machine; start_server.sh must be running on the server machine.
#
#   udp    single_a, single_b   no congestion control, no retransmission: what the link can carry
#   tcp    single_a, single_b   the TCP of the Linux kernel on one link
#   mptcp  multipath_a_first    Multipath TCP of the Linux kernel, first subflow on network A
#   mptcp  multipath_b_first    the same, first subflow on network B
#
#   ./evaluation/scripts/run_baselines.sh                     20 MB per test, 3 runs of each
#   ./evaluation/scripts/run_baselines.sh --bytes 256K        the short transfer
#   ./evaluation/scripts/run_baselines.sh --runs 5 --label lossy
#   ./evaluation/scripts/run_baselines.sh --udp-rate 50M      sending rate of the UDP test
#                                                             (must be above the link capacity)
#
# Needs on both machines: sudo apt install iperf3 mptcpize
# Results are added to evaluation/results/throughput.csv, and a summary is printed at the end.

cd "$(dirname "$0")/../.." || exit 1
. evaluation/scripts/config.sh

BYTES=20M
RUNS=3
LABEL=default
UDP_RATE=20M
UDP_TIME=10

while [ $# -gt 0 ]; do
    case "$1" in
        --bytes)    BYTES=$2;    shift 2 ;;
        --runs)     RUNS=$2;     shift 2 ;;
        --label)    LABEL=$2;    shift 2 ;;
        --udp-rate) UDP_RATE=$2; shift 2 ;;
        --udp-time) UDP_TIME=$2; shift 2 ;;
        *) echo "unknown argument: $1"; sed -n '2,19p' "$0"; exit 2 ;;
    esac
done

require_role client

if ! command -v "$IPERF3" > /dev/null; then
    echo "error: iperf3 is not installed (sudo apt install iperf3)"
    exit 1
fi

HAVE_MPTCP=1
if ! command -v ${MPTCPIZE%% *} > /dev/null; then
    echo "note: mptcpize is not installed, Multipath TCP is skipped (sudo apt install mptcpize)"
    HAVE_MPTCP=0
elif ! $SUDO sysctl -q -w net.mptcp.enabled=1 2>/dev/null; then
    echo "note: this kernel has no Multipath TCP, it is skipped"
    HAVE_MPTCP=0
else
    # The client may open a second subflow towards the address that the server announces
    $SUDO ip mptcp limits set subflows 2 add_addr_accepted 2
fi

mkdir -p "$RESULTS"
CSV="$RESULTS/throughput.csv"
if [ ! -f "$CSV" ]; then
    echo "label,protocol,config,run,bytes,time_ms,goodput_kbps,rx_a_bytes,rx_b_bytes,status" > "$CSV"
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Reads the report of iperf3 (JSON) and prints "bytes time_ms goodput_kbps lost_percent" as
# seen by the receiver, or nothing when the test failed.
read_report() {
    python3 - "$1" <<'EOF'
import json, sys
try:
    report = json.load(open(sys.argv[1]))
    end = report["end"]
    # "sum_received" is what arrived at the receiver. (For UDP, "sum" holds the bytes that
    # the sender sent: most of them are lost when the sender is faster than the link.)
    received = end["sum_received"]
    seconds = received["seconds"]
    print(received["bytes"], int(seconds * 1000), int(received["bytes"] * 8 / seconds / 1000),
          "%.1f" % end.get("sum", {}).get("lost_percent", 0.0))
except Exception:
    pass
EOF
}

# measure <protocol> <config> <run> <command...>
measure() {
    protocol=$1
    config=$2
    run=$3
    shift 3

    RX_A=$(rx_bytes "$IF_A")
    RX_B=$(rx_bytes "$IF_B")
    # -i 0.1: iperf3 notices the end of a test only at the end of a reporting interval.
    # With the default of 1 second the measured time would be rounded up to a full second.
    "$@" -i 0.1 --json > "$TMP/report.json" 2> "$TMP/error.log"
    RX_A=$(( $(rx_bytes "$IF_A") - RX_A ))
    RX_B=$(( $(rx_bytes "$IF_B") - RX_B ))

    set -- $(read_report "$TMP/report.json")
    if [ $# -eq 4 ] && [ "$1" -gt 0 ]; then
        EXTRA=""
        [ "$protocol" = "udp" ] && EXTRA="  ($4 % of the packets lost)"
        printf "[run %s/%s] %-5s %-18s %7s kbit/s  %6s ms  link A %5s kB, link B %5s kB%s\n" \
               "$run" "$RUNS" "$protocol" "$config" "$3" "$2" $((RX_A / 1000)) $((RX_B / 1000)) "$EXTRA"
        echo "$LABEL,$protocol,$config,$run,$1,$2,$3,$RX_A,$RX_B,ok" >> "$CSV"
    else
        FAILURES=$((FAILURES + 1))
        MESSAGE=$(sed -n 's/.*"error":[^"]*"\([^"]*\)".*/\1/p' "$TMP/report.json" | head -n 1)
        echo "[run $run/$RUNS] $protocol $config FAILED: ${MESSAGE:-$(tail -n 1 "$TMP/error.log")}"
        echo "$LABEL,$protocol,$config,$run,0,0,0,$RX_A,$RX_B,failed" >> "$CSV"
    fi
    sleep 1 # the iperf3 server gets ready for the next test, the queues of the links empty
}

echo "=== UDP, TCP and Multipath TCP with iperf3 (label: $LABEL) ==="
FAILURES=0
for run in $(seq 1 "$RUNS"); do
    # -R: the server sends. -B: the local address, and so the link that is used.
    measure udp single_a "$run" "$IPERF3" -c "$SERVER_A" -B "$CLIENT_A" -p "$IPERF_PORT" -R -u -b "$UDP_RATE" -t "$UDP_TIME"
    measure udp single_b "$run" "$IPERF3" -c "$SERVER_B" -B "$CLIENT_B" -p "$IPERF_PORT" -R -u -b "$UDP_RATE" -t "$UDP_TIME"

    measure tcp single_a "$run" "$IPERF3" -c "$SERVER_A" -B "$CLIENT_A" -p "$IPERF_PORT" -R -n "$BYTES"
    measure tcp single_b "$run" "$IPERF3" -c "$SERVER_B" -B "$CLIENT_B" -p "$IPERF_PORT" -R -n "$BYTES"

    if [ "$HAVE_MPTCP" -eq 1 ]; then
        measure mptcp multipath_a_first "$run" $MPTCPIZE "$IPERF3" -c "$SERVER_A" -B "$CLIENT_A" -p "$MPTCP_PORT" -R -n "$BYTES"
        measure mptcp multipath_b_first "$run" $MPTCPIZE "$IPERF3" -c "$SERVER_B" -B "$CLIENT_B" -p "$MPTCP_PORT" -R -n "$BYTES"
    fi
done

echo
evaluation/scripts/summary.sh --label "$LABEL"

if [ "$FAILURES" -gt 0 ]; then
    echo "$FAILURES test(s) FAILED"
    exit 1
fi
exit 0
