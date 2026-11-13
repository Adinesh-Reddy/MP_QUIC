# Settings shared by all the evaluation scripts. This file is not run: the other scripts
# include it with ". config.sh".
#
# Test bed: the topology of the paper (Figure 3). Four machines (virtual machines): a client
# and a server with two interfaces each, joined by two disjoint paths. On each path a router
# gives the path its capacity, delay, loss and queue.
#
#              10.0.0.0/24                      10.0.2.0/24
#           .1 ------------ .254 [router A] .254 ------------ .2
#   client                                                       server
#           .1 ------------ .254 [router B] .254 ------------ .2
#              10.0.1.0/24                      10.0.3.0/24
#
# Every value can be changed without editing the file, for example:
#     PORT=5000 ./evaluation/scripts/start_server.sh

CLIENT_A=${CLIENT_A:-10.0.0.1}   # address of the client on path A
CLIENT_B=${CLIENT_B:-10.0.1.1}   # address of the client on path B
SERVER_A=${SERVER_A:-10.0.2.2}   # address of the server on path A
SERVER_B=${SERVER_B:-10.0.3.2}   # address of the server on path B

ROUTER_A_CLIENT=${ROUTER_A_CLIENT:-10.0.0.254}   # router A, side of the client
ROUTER_A_SERVER=${ROUTER_A_SERVER:-10.0.2.254}   # router A, side of the server
ROUTER_B_CLIENT=${ROUTER_B_CLIENT:-10.0.1.254}   # router B, side of the client
ROUTER_B_SERVER=${ROUTER_B_SERVER:-10.0.3.254}   # router B, side of the server

# The four networks (used for the routes of the client and of the server)
NET_A_CLIENT=${NET_A_CLIENT:-10.0.0.0/24}
NET_B_CLIENT=${NET_B_CLIENT:-10.0.1.0/24}
NET_A_SERVER=${NET_A_SERVER:-10.0.2.0/24}
NET_B_SERVER=${NET_B_SERVER:-10.0.3.0/24}
PREFIX=${PREFIX:-24}             # size of the four networks

PORT=${PORT:-4433}               # UDP port of mpq_server
IPERF_PORT=${IPERF_PORT:-5201}   # iperf3 server for TCP and UDP
MPTCP_PORT=${MPTCP_PORT:-5202}   # iperf3 server for Multipath TCP

BIN=${BIN:-bin}                             # folder of mpq_server, mpq_client, mpq_reqres
FILES=${FILES:-evaluation/files}            # files served by the server
RESULTS=${RESULTS:-evaluation/results}      # where the measurements are written

IPERF3=${IPERF3:-iperf3}
MPTCPIZE=${MPTCPIZE:-mptcpize run}          # makes a TCP program use Multipath TCP

# Commands that change the network need root
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

# Prints the name of the network interface that has the given IPv4 address
interface_of() {
    ip -o -4 addr show | awk -v wanted="$1" '{ split($4, a, "/"); if (a[1] == wanted) { print $2; exit } }'
}

# Finds out on which machine the script runs. Sets ROLE:
#   server, client   IF_A and IF_B are the interfaces of path A and path B
#   router           ROUTER_PATH is a or b; IF_CLIENT and IF_SERVER are the interfaces
#                    towards the client and towards the server
detect_role() {
    IF_A=$(interface_of "$SERVER_A")
    IF_B=$(interface_of "$SERVER_B")
    if [ -n "$IF_A" ] && [ -n "$IF_B" ]; then
        ROLE=server
        return 0
    fi

    IF_A=$(interface_of "$CLIENT_A")
    IF_B=$(interface_of "$CLIENT_B")
    if [ -n "$IF_A" ] && [ -n "$IF_B" ]; then
        ROLE=client
        return 0
    fi

    IF_CLIENT=$(interface_of "$ROUTER_A_CLIENT")
    IF_SERVER=$(interface_of "$ROUTER_A_SERVER")
    if [ -n "$IF_CLIENT" ] && [ -n "$IF_SERVER" ]; then
        ROLE=router
        ROUTER_PATH=a
        return 0
    fi

    IF_CLIENT=$(interface_of "$ROUTER_B_CLIENT")
    IF_SERVER=$(interface_of "$ROUTER_B_SERVER")
    if [ -n "$IF_CLIENT" ] && [ -n "$IF_SERVER" ]; then
        ROLE=router
        ROUTER_PATH=b
        return 0
    fi

    echo "error: this machine has not the addresses of the client, of the server or of a router."
    echo "       Run evaluation/scripts/setup_addresses.sh first."
    exit 1
}

# Stops the script when it does not run on the expected machine
require_role() {
    detect_role
    if [ "$ROLE" != "$1" ]; then
        echo "error: this script runs on the $1 machine (this is the $ROLE)"
        exit 1
    fi
}

# Bytes received so far on an interface (to show which link carried the data)
rx_bytes() {
    cat "/sys/class/net/$1/statistics/rx_bytes"
}
