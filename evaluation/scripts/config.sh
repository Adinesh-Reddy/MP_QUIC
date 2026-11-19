#!/bin/bash
# ==============================================================================
# evaluation/scripts/config.sh
# Central configuration for MP-QUIC 4-Node Testbed Evaluation
# MP-QUIC Network Evaluation Configuration
# ==============================================================================

# Testbed Topology IP Addresses:
#        10.0.0.0/24                         10.0.2.0/24
#     .1 ------------ .254 [Router A] .254 ------------ .2
# Client                                                   Server
#     .1 ------------ .254 [Router B] .254 ------------ .2
#        10.0.1.0/24                         10.0.3.0/24

CLIENT_A=${CLIENT_A:-10.0.0.1}
CLIENT_B=${CLIENT_B:-10.0.1.1}
SERVER_A=${SERVER_A:-10.0.2.2}
SERVER_B=${SERVER_B:-10.0.3.2}

ROUTER_A_CLIENT=${ROUTER_A_CLIENT:-10.0.0.254}
ROUTER_A_SERVER=${ROUTER_A_SERVER:-10.0.2.254}
ROUTER_B_CLIENT=${ROUTER_B_CLIENT:-10.0.1.254}
ROUTER_B_SERVER=${ROUTER_B_SERVER:-10.0.3.254}

NET_A_CLIENT=${NET_A_CLIENT:-10.0.0.0/24}
NET_B_CLIENT=${NET_B_CLIENT:-10.0.1.0/24}
NET_A_SERVER=${NET_A_SERVER:-10.0.2.0/24}
NET_B_SERVER=${NET_B_SERVER:-10.0.3.0/24}
PREFIX=${PREFIX:-24}

PORT=${PORT:-4433}
IPERF_PORT=${IPERF_PORT:-5201}
MPTCP_PORT=${MPTCP_PORT:-5202}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BIN=${BIN:-"$BASE_DIR/bin"}
FILES=${FILES:-"$BASE_DIR/evaluation/files"}
RESULTS=${RESULTS:-"$BASE_DIR/evaluation/results"}
LOGS=${LOGS:-"$BASE_DIR/evaluation/logs"}
PLOTS=${PLOTS:-"$BASE_DIR/evaluation/plots"}

IPERF3=${IPERF3:-iperf3}
MPTCPIZE=${MPTCPIZE:-"mptcpize run"}

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

# Locate network interface holding a specific IP
interface_of() {
    local target_ip="$1"
    ip -o -4 addr show 2>/dev/null | awk -v wanted="$target_ip" '{
        split($4, a, "/");
        if (a[1] == wanted) { print $2; exit }
    }'
}

# Read interface received bytes count
rx_bytes() {
    local iface="$1"
    if [ -n "$iface" ] && [ -f "/sys/class/net/$iface/statistics/rx_bytes" ]; then
        cat "/sys/class/net/$iface/statistics/rx_bytes"
    else
        echo 0
    fi
}

# Detect VM role based on configured IPs
detect_role() {
    if [ "${LOOPBACK:-0}" -eq 1 ]; then
        ROLE="loopback"
        IF_A="lo"
        IF_B="lo"
        return 0
    fi

    IF_A=$(interface_of "$CLIENT_A")
    IF_B=$(interface_of "$CLIENT_B")
    if [ -n "$IF_A" ] && [ -n "$IF_B" ]; then
        ROLE="client"
        return 0
    fi

    IF_A=$(interface_of "$SERVER_A")
    IF_B=$(interface_of "$SERVER_B")
    if [ -n "$IF_A" ] && [ -n "$IF_B" ]; then
        ROLE="server"
        return 0
    fi

    IF_CLIENT=$(interface_of "$ROUTER_A_CLIENT")
    IF_SERVER=$(interface_of "$ROUTER_A_SERVER")
    if [ -n "$IF_CLIENT" ] && [ -n "$IF_SERVER" ]; then
        ROLE="router"
        ROUTER_PATH="a"
        return 0
    fi

    IF_CLIENT=$(interface_of "$ROUTER_B_CLIENT")
    IF_SERVER=$(interface_of "$ROUTER_B_SERVER")
    if [ -n "$IF_CLIENT" ] && [ -n "$IF_SERVER" ]; then
        ROLE="router"
        ROUTER_PATH="b"
        return 0
    fi

    # Fallback to loopback if no testbed IPs match
    ROLE="unknown"
    return 1
}

require_role() {
    local expected="$1"
    if ! detect_role && [ "$ROLE" != "unknown" ]; then
        echo "Error: Unable to detect network role from interface addresses."
        exit 1
    fi
    if [ "$ROLE" != "$expected" ] && [ "$ROLE" != "loopback" ]; then
        echo "Error: Expected role '$expected', but detected '$ROLE'."
        echo "Make sure IP addresses are assigned or pass appropriate environment variables."
        exit 1
    fi
}
