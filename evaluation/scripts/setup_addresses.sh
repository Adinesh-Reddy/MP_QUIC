#!/bin/bash
# Gives this machine its addresses and routes in the test bed of config.sh.
#
#   ./evaluation/scripts/setup_addresses.sh client   <interface of path A> <interface of path B>
#   ./evaluation/scripts/setup_addresses.sh server   <interface of path A> <interface of path B>
#   ./evaluation/scripts/setup_addresses.sh router-a <interface to the client> <interface to the server>
#   ./evaluation/scripts/setup_addresses.sh router-b <interface to the client> <interface to the server>
#
# "ip -br link" lists the interfaces. Everything is lost at the next reboot: run it again.

cd "$(dirname "$0")/../.." || exit 1
. evaluation/scripts/config.sh

if [ $# -ne 3 ]; then
    sed -n '2,9p' "$0"
    echo "interfaces of this machine:"
    ip -br link
    exit 2
fi

# "replace" does not fail when the address or the route is already there
set_address() {
    $SUDO ip addr replace "$1/$PREFIX" dev "$2" || exit 1
    $SUDO ip link set "$2" up || exit 1
}

case "$1" in
    client)
        set_address "$CLIENT_A" "$2"
        set_address "$CLIENT_B" "$3"
        # The server is reached through router A on path A and through router B on path B
        $SUDO ip route replace "$NET_A_SERVER" via "$ROUTER_A_CLIENT" dev "$2" || exit 1
        $SUDO ip route replace "$NET_B_SERVER" via "$ROUTER_B_CLIENT" dev "$3" || exit 1
        ;;
    server)
        set_address "$SERVER_A" "$2"
        set_address "$SERVER_B" "$3"
        $SUDO ip route replace "$NET_A_CLIENT" via "$ROUTER_A_SERVER" dev "$2" || exit 1
        $SUDO ip route replace "$NET_B_CLIENT" via "$ROUTER_B_SERVER" dev "$3" || exit 1
        ;;
    router-a)
        set_address "$ROUTER_A_CLIENT" "$2"
        set_address "$ROUTER_A_SERVER" "$3"
        $SUDO sysctl -q -w net.ipv4.ip_forward=1 || exit 1   # a router forwards packets
        ;;
    router-b)
        set_address "$ROUTER_B_CLIENT" "$2"
        set_address "$ROUTER_B_SERVER" "$3"
        $SUDO sysctl -q -w net.ipv4.ip_forward=1 || exit 1
        ;;
    *)
        sed -n '2,9p' "$0"
        exit 2
        ;;
esac

echo "$1 is ready:"
ip -br -4 addr show dev "$2"
ip -br -4 addr show dev "$3"
