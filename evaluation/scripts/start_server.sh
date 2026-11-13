#!/bin/bash
# Starts everything the server machine needs for the evaluation, and stops it all on Ctrl-C:
#   - the test files (random bytes) and their SHA-256
#   - mpq_server on both networks                          (MP-QUIC)
#   - an iperf3 server for the TCP and UDP measurements    (port 5201)
#   - an iperf3 server that speaks Multipath TCP           (port 5202)
#
#   ./evaluation/scripts/start_server.sh
#
# iperf3 and mptcpize are only needed for the comparison with TCP, MPTCP and UDP
# (run_baselines.sh): without them the MP-QUIC server still starts.

cd "$(dirname "$0")/../.." || exit 1
. evaluation/scripts/config.sh
require_role server

if [ ! -x "$BIN/mpq_server" ]; then
    echo "error: $BIN/mpq_server is not built: run 'make' first"
    exit 1
fi

# --- test files: the sizes of the paper (20 MB, long transfer) and 256 kB (short transfer) ---
mkdir -p "$FILES"
make_file() {
    if [ ! -f "$FILES/$1" ]; then
        head -c "$2" /dev/urandom > "$FILES/$1"
    fi
    # The client downloads this small file first and so knows the SHA-256 to expect
    sha256sum < "$FILES/$1" | cut -d' ' -f1 > "$FILES/$1.sha256"
    echo "  $1  $(wc -c < "$FILES/$1") bytes  sha256 $(cat "$FILES/$1.sha256")"
}
echo "Files in $FILES:"
make_file test20M.bin 20971520
make_file test256K.bin 262144

# --- Multipath TCP: the server announces both of its addresses to the client ---
PIDS=""
if $SUDO sysctl -q -w net.mptcp.enabled=1 2>/dev/null; then
    $SUDO ip mptcp limits set subflows 2 add_addr_accepted 2
    $SUDO ip mptcp endpoint add "$SERVER_A" dev "$IF_A" signal 2>/dev/null
    $SUDO ip mptcp endpoint add "$SERVER_B" dev "$IF_B" signal 2>/dev/null
else
    echo "note: this kernel has no Multipath TCP (net.mptcp.enabled is missing)"
fi

# --- iperf3 servers ---
if command -v "$IPERF3" > /dev/null; then
    "$IPERF3" -s -p "$IPERF_PORT" > /dev/null 2>&1 &
    PIDS="$PIDS $!"
    echo "iperf3 (TCP, UDP) on port $IPERF_PORT"

    if command -v ${MPTCPIZE%% *} > /dev/null; then
        $MPTCPIZE "$IPERF3" -s -p "$MPTCP_PORT" > /dev/null 2>&1 &
        PIDS="$PIDS $!"
        echo "iperf3 (Multipath TCP) on port $MPTCP_PORT"
    else
        echo "note: mptcpize is not installed, no Multipath TCP server (sudo apt install mptcpize)"
    fi
else
    echo "note: iperf3 is not installed, no TCP/UDP/MPTCP servers (sudo apt install iperf3)"
fi

cleanup() {
    [ -n "$PIDS" ] && kill $PIDS 2>/dev/null
}
trap cleanup EXIT

# --- MP-QUIC server, in the foreground: its messages show every connection and its paths ---
echo "mpq_server on $SERVER_A:$PORT and $SERVER_B:$PORT   (Ctrl-C stops everything)"
"$BIN/mpq_server" --listen "$SERVER_A:$PORT" --listen "$SERVER_B:$PORT" --dir "$FILES"
