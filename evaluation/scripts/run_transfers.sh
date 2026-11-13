#!/bin/bash
# File download over MP-QUIC, measured in the four ways of the paper (step 5 of the VM guide).
# Runs on the client machine; start_server.sh must be running on the server machine.
#
#   multipath_a_first   two paths, the connection starts on network A
#   multipath_b_first   two paths, the connection starts on network B
#   single_a            one path, network A only
#   single_b            one path, network B only
#
#   ./evaluation/scripts/run_transfers.sh                        20 MB file, 3 runs of each
#   ./evaluation/scripts/run_transfers.sh --file test256K.bin    the short transfer
#   ./evaluation/scripts/run_transfers.sh --runs 5 --label lossy name of this network setting
#   ./evaluation/scripts/run_transfers.sh --stats                also record the state of the paths
#
# Every download is checked: the SHA-256 of the received file must be the one of the server.
# Results are added to evaluation/results/throughput.csv, and a summary is printed at the end.

cd "$(dirname "$0")/../.." || exit 1
. evaluation/scripts/config.sh

FILE=test20M.bin
RUNS=3
LABEL=default
TIMEOUT=300
WITH_STATS=0
CONFIGS="multipath_a_first multipath_b_first single_a single_b"

while [ $# -gt 0 ]; do
    case "$1" in
        --file)    FILE=$2;    shift 2 ;;
        --runs)    RUNS=$2;    shift 2 ;;
        --label)   LABEL=$2;   shift 2 ;;
        --timeout) TIMEOUT=$2; shift 2 ;;
        --configs) CONFIGS=$2; shift 2 ;;
        --stats)   WITH_STATS=1; shift ;;
        *) echo "unknown argument: $1"; sed -n '2,16p' "$0"; exit 2 ;;
    esac
done

require_role client

if [ ! -x "$BIN/mpq_client" ]; then
    echo "error: $BIN/mpq_client is not built: run 'make' first"
    exit 1
fi

mkdir -p "$RESULTS"
CSV="$RESULTS/throughput.csv"
if [ ! -f "$CSV" ]; then
    echo "label,protocol,config,run,bytes,time_ms,goodput_kbps,rx_a_bytes,rx_b_bytes,status" > "$CSV"
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# The addresses that mpq_client uses in each of the four ways
client_arguments() {
    case "$1" in
        multipath_a_first) echo "--server $SERVER_A:$PORT --local $CLIENT_A --local $CLIENT_B" ;;
        multipath_b_first) echo "--server $SERVER_B:$PORT --local $CLIENT_B --local $CLIENT_A" ;;
        single_a)          echo "--server $SERVER_A:$PORT --local $CLIENT_A" ;;
        single_b)          echo "--server $SERVER_B:$PORT --local $CLIENT_B" ;;
        *) echo "unknown configuration: $1" >&2; exit 2 ;;
    esac
}

# Value of "name=value" in the RESULT line of mpq_client
field() {
    echo "$RESULT_LINE" | sed -n "s/.* $1=\([^ ]*\).*/\1/p"
}

# --- the SHA-256 to expect: the server keeps it in a small file next to the big one ---
if ! "$BIN/mpq_client" --server "$SERVER_A:$PORT" --local "$CLIENT_A" --get "$FILE.sha256" \
                       --out "$TMP/expected.sha256" --timeout 20 > "$TMP/client.log" 2>&1; then
    echo "error: cannot download $FILE.sha256 from $SERVER_A:$PORT"
    echo "       Is start_server.sh running on the server machine? Does 'ping $SERVER_A' work?"
    tail -n 3 "$TMP/client.log"
    exit 1
fi
EXPECTED=$(tr -d ' \n' < "$TMP/expected.sha256")
echo "=== MP-QUIC download of $FILE (label: $LABEL), expected sha256 $EXPECTED ==="

FAILURES=0
for run in $(seq 1 "$RUNS"); do
    for config in $CONFIGS; do
        ARGS=$(client_arguments "$config") || exit 2
        if [ "$WITH_STATS" -eq 1 ]; then
            ARGS="$ARGS --stats $RESULTS/paths_${LABEL}_${config}_run${run}.csv"
        fi

        RX_A=$(rx_bytes "$IF_A")
        RX_B=$(rx_bytes "$IF_B")
        # shellcheck disable=SC2086
        "$BIN/mpq_client" $ARGS --get "$FILE" --out "$TMP/out.bin" --timeout "$TIMEOUT" > "$TMP/client.log" 2>&1
        RX_A=$(( $(rx_bytes "$IF_A") - RX_A ))
        RX_B=$(( $(rx_bytes "$IF_B") - RX_B ))

        RESULT_LINE=$(grep "^RESULT " "$TMP/client.log" | tail -n 1)
        STATUS=$(field status)
        RECEIVED=$(sha256sum < "$TMP/out.bin" | cut -d' ' -f1)

        if [ "$STATUS" = "ok" ] && [ "$RECEIVED" = "$EXPECTED" ] && [ "$(field sha256)" = "$EXPECTED" ]; then
            printf "[run %s/%s] %-18s %7s kbit/s  %6s ms  paths=%s  link A %5s kB, link B %5s kB  sha256 ok\n" \
                   "$run" "$RUNS" "$config" "$(field goodput_kbps)" "$(field time_ms)" "$(field paths)" \
                   $((RX_A / 1000)) $((RX_B / 1000))
            echo "$LABEL,mpquic,$config,$run,$(field bytes),$(field time_ms),$(field goodput_kbps),$RX_A,$RX_B,ok" >> "$CSV"
        else
            FAILURES=$((FAILURES + 1))
            if [ "$STATUS" = "ok" ]; then
                echo "[run $run/$RUNS] $config FAILED: the received file differs from the original"
            else
                echo "[run $run/$RUNS] $config FAILED: ${RESULT_LINE:-$(tail -n 1 "$TMP/client.log")}"
            fi
            echo "$LABEL,mpquic,$config,$run,0,0,0,$RX_A,$RX_B,failed" >> "$CSV"
        fi

        sleep 1 # the server closes the connection, the queues of the links empty
    done
done

echo
evaluation/scripts/summary.sh --label "$LABEL"

if [ "$FAILURES" -gt 0 ]; then
    echo "$FAILURES download(s) FAILED"
    exit 1
fi
exit 0
