#!/bin/bash
# ==============================================================================
# evaluation/scripts/start_server.sh
# Starts MP-QUIC server and baseline daemons on Server VM.
# Usage:
#   ./start_server.sh [--scenario <name>] [--dir <path>]
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/config.sh"

SCENARIO="default"
while [ $# -gt 0 ]; do
    case "$1" in
        --scenario) SCENARIO="$2"; shift 2 ;;
        --dir) FILES="$2"; shift 2 ;;
        *) shift ;;
    esac
done

detect_role >/dev/null 2>&1
if [ "$ROLE" != "server" ] && [ "$ROLE" != "loopback" ] && [ "${FORCE:-0}" -ne 1 ]; then
    echo "Warning: This script is intended to run on MPQ-Server (detected role: $ROLE)."
    echo "Press Enter to proceed anyway, or Ctrl-C to cancel."
    read -r -t 5 || true
fi

if [ ! -x "$BIN/mpq_server" ]; then
    echo "Error: $BIN/mpq_server is not built. Run 'make' first."
    exit 1
fi

mkdir -p "$FILES" "$LOGS"

generate_file() {
    local name="$1"
    local size="$2"
    local target="$FILES/$name"
    if [ ! -f "$target" ] || [ "$(wc -c < "$target" 2>/dev/null || echo 0)" -ne "$size" ]; then
        echo "Generating $name ($size bytes)..."
        head -c "$size" /dev/urandom > "$target"
    fi
    sha256sum < "$target" | cut -d' ' -f1 > "$target.sha256"
    echo "  [FILE READY] $name ($(wc -c < "$target") bytes) SHA256: $(cat "$target.sha256")"
}

echo "=== Preparing Test Datasets ==="
generate_file test20M.bin 20971520
generate_file test5M.bin 5242880
generate_file test256K.bin 262144

# Configure MPTCP endpoints on server if kernel supports it
if [ "$(id -u)" -eq 0 ] || sudo -n true 2>/dev/null; then
    if $SUDO sysctl -q -w net.mptcp.enabled=1 2>/dev/null; then
        $SUDO ip mptcp limits set subflows 2 add_addr_accepted 2 2>/dev/null || true
        if [ -n "$IF_A" ] && [ -n "$IF_B" ]; then
            $SUDO ip mptcp endpoint flush 2>/dev/null || true
            $SUDO ip mptcp endpoint add "$SERVER_A" dev "$IF_A" signal 2>/dev/null || true
            $SUDO ip mptcp endpoint add "$SERVER_B" dev "$IF_B" signal 2>/dev/null || true
        fi
    fi
fi

PIDS=""
cleanup() {
    echo ""
    echo "Stopping background servers..."
    if [ -n "$PIDS" ]; then
        kill $PIDS 2>/dev/null || true
    fi
    echo "Server shutdown complete."
    exit 0
}
trap cleanup SIGINT SIGTERM EXIT

# Start iperf3 servers if available
if command -v "$IPERF3" >/dev/null 2>&1; then
    "$IPERF3" -s -p "$IPERF_PORT" >/dev/null 2>&1 &
    PIDS="$PIDS $!"
    echo "  [SERVICE] iperf3 TCP/UDP listening on port $IPERF_PORT (PID: $!)"

    if command -v ${MPTCPIZE%% *} >/dev/null 2>&1; then
        $MPTCPIZE "$IPERF3" -s -p "$MPTCP_PORT" >/dev/null 2>&1 &
        PIDS="$PIDS $!"
        echo "  [SERVICE] MPTCP iperf3 listening on port $MPTCP_PORT (PID: $!)"
    fi
fi

SERVER_LOG="$LOGS/server_${SCENARIO}.log"
echo "=========================================================="
echo "Starting MP-QUIC Server"
echo "  Listen Path A : $SERVER_A:$PORT"
echo "  Listen Path B : $SERVER_B:$PORT"
echo "  Files Dir     : $FILES"
echo "  Log File      : $SERVER_LOG"
echo "=========================================================="
echo "Press Ctrl-C to terminate all services."

# Run mpq_server in foreground tee'd to log
"$BIN/mpq_server" --listen "$SERVER_A:$PORT" --listen "$SERVER_B:$PORT" --dir "$FILES" 2>&1 | tee -a "$SERVER_LOG"
