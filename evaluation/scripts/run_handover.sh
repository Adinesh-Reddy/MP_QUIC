#!/bin/bash
# Network handover (step 6 of the VM guide, Section 4.3 and Figure 11 of the paper).
# The client sends a request of 750 bytes every 400 ms and measures the time until the
# answer is back. After 3 seconds link A, which carries the initial path, is cut: every
# packet is lost in both directions. With two paths the connection moves to link B.
# Runs on the client machine; start_server.sh must be running on the server machine.
#
#   ./evaluation/scripts/run_handover.sh                 two paths, link A cut after 3 s
#   ./evaluation/scripts/run_handover.sh --single-path   one path only: the connection is lost
#   ./evaluation/scripts/run_handover.sh --no-cut        two paths, nothing is cut (reference)
#   ./evaluation/scripts/run_handover.sh --cut-after 5 --count 50 --label lossy
#
# The link is cut on this machine only (no command to type on the server during the test)
# and repaired at the end; the shaping of shape_links.sh is not touched.
# The delays are written to evaluation/results/handover_<label>_<mode>.csv.

cd "$(dirname "$0")/../.." || exit 1
. evaluation/scripts/config.sh

CUT_AFTER=3
COUNT=35
INTERVAL_MS=400
SIZE=750
LABEL=default
MODE=multipath

while [ $# -gt 0 ]; do
    case "$1" in
        --cut-after)   CUT_AFTER=$2;   shift 2 ;;
        --count)       COUNT=$2;       shift 2 ;;
        --interval-ms) INTERVAL_MS=$2; shift 2 ;;
        --size)        SIZE=$2;        shift 2 ;;
        --label)       LABEL=$2;       shift 2 ;;
        --single-path) MODE=single;    shift ;;
        --no-cut)      MODE=nocut;     shift ;;
        *) echo "unknown argument: $1"; sed -n '2,15p' "$0"; exit 2 ;;
    esac
done

require_role client

if [ ! -x "$BIN/mpq_reqres" ]; then
    echo "error: $BIN/mpq_reqres is not built: run 'make' first"
    exit 1
fi

mkdir -p "$RESULTS"
OUT="$RESULTS/handover_${LABEL}_${MODE}.csv"
LOG=$(mktemp)

# Cuts link A: a filter drops every packet that leaves and every packet that arrives
cut_link() {
    $SUDO tc qdisc add dev "$IF_A" clsact &&
    $SUDO tc filter add dev "$IF_A" egress matchall action drop &&
    $SUDO tc filter add dev "$IF_A" ingress matchall action drop
}

repair_link() {
    $SUDO tc qdisc del dev "$IF_A" clsact 2>/dev/null
}

cleanup() {
    repair_link
    rm -f "$LOG"
}
trap cleanup EXIT
repair_link # in case an earlier run was interrupted

LOCALS="--local $CLIENT_A --local $CLIENT_B"
if [ "$MODE" = "single" ]; then
    LOCALS="--local $CLIENT_A"
fi

echo "=== Handover test ($MODE, label: $LABEL): $COUNT requests of $SIZE bytes, one every $INTERVAL_MS ms ==="

# shellcheck disable=SC2086
"$BIN/mpq_reqres" --server "$SERVER_A:$PORT" $LOCALS --count "$COUNT" --interval-ms "$INTERVAL_MS" \
                  --size "$SIZE" --out "$OUT" > "$LOG" 2>&1 &
CLIENT_PID=$!

if [ "$MODE" != "nocut" ]; then
    sleep "$CUT_AFTER"
    if cut_link; then
        echo ">>> link A ($IF_A) cut after $CUT_AFTER s"
    else
        echo "error: cannot cut link A"
        kill "$CLIENT_PID" 2>/dev/null
        exit 1
    fi
fi

wait "$CLIENT_PID"
CLIENT_STATUS=$?
repair_link

grep -E "^(REQUEST|PATH|RESULT) " "$LOG"
echo

# Summary: the delays before and after the cut, and the largest one (the handover itself)
CUT_MS=$((CUT_AFTER * 1000))
[ "$MODE" = "nocut" ] && CUT_MS=999999999
awk -F, -v cut_ms="$CUT_MS" '
function median(a, count,    i, j, t) {
    if (count == 0) return -1
    for (i = 2; i <= count; i++)
        for (j = i; j > 1 && a[j - 1] > a[j]; j--) { t = a[j]; a[j] = a[j - 1]; a[j - 1] = t }
    return (count % 2 == 1) ? a[(count + 1) / 2] : (a[count / 2] + a[count / 2 + 1]) / 2
}
NR == 1 { next }
{
    sent++
    if ($3 < 0) { lost++; next }
    if ($3 > worst) { worst = $3; worst_id = $1; worst_sent = $2 }
    if ($2 < cut_ms) before[++nb] = $3; else after[++na] = $3
}
END {
    printf "requests sent: %d, answered: %d, without answer: %d\n", sent, sent - lost, lost
    if (nb > 0) printf "median delay before the cut: %7.1f ms  (%d requests)\n", median(before, nb), nb
    if (na > 0) printf "median delay after the cut:  %7.1f ms  (%d requests)\n", median(after, na), na
    if (sent > lost) printf "largest delay:               %7.1f ms  (request %d, sent at %.0f ms)\n", worst, worst_id, worst_sent
}' "$OUT"
echo "delays written to $OUT"

exit "$CLIENT_STATUS"
