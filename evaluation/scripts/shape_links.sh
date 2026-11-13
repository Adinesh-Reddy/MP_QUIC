#!/bin/bash
# Gives the two paths their capacity, delay, loss and queue. Runs on the two ROUTER machines:
# type the SAME command on both, router A takes the values of path A and router B the
# values of path B. A router shapes both directions of its path (as a link of Mininet in
# the paper).
#
#   ./evaluation/scripts/shape_links.sh                       default paths (see below)
#   ./evaluation/scripts/shape_links.sh --rate-a 20mbit --delay-a 5ms --rate-b 2mbit --delay-b 40ms
#   ./evaluation/scripts/shape_links.sh --loss-a 1 --loss-b 1 random losses of 1 % on both paths
#   ./evaluation/scripts/shape_links.sh show                  what is installed now
#   ./evaluation/scripts/shape_links.sh clear                 remove the shaping
#
# The delay is the one-way delay of the path: the round-trip time is twice that value.

cd "$(dirname "$0")/../.." || exit 1
. evaluation/scripts/config.sh

RATE_A=8mbit     # capacity of path A
DELAY_A=10ms     # one-way delay of path A
LOSS_A=0         # random losses on path A, in percent
RATE_B=4mbit
DELAY_B=25ms
LOSS_B=0
QUEUE=50         # packets that can wait in the queue of a path

ACTION=shape
while [ $# -gt 0 ]; do
    case "$1" in
        --rate-a)  RATE_A=$2;  shift 2 ;;
        --delay-a) DELAY_A=$2; shift 2 ;;
        --loss-a)  LOSS_A=$2;  shift 2 ;;
        --rate-b)  RATE_B=$2;  shift 2 ;;
        --delay-b) DELAY_B=$2; shift 2 ;;
        --loss-b)  LOSS_B=$2;  shift 2 ;;
        --queue)   QUEUE=$2;   shift 2 ;;
        show|clear) ACTION=$1; shift ;;
        *) echo "unknown argument: $1"; sed -n '2,14p' "$0"; exit 2 ;;
    esac
done

detect_role
if [ "$ROLE" != "router" ]; then
    echo "error: the paths are shaped on the two router machines (this is the $ROLE)"
    exit 1
fi

if [ "$ROUTER_PATH" = "a" ]; then
    RATE=$RATE_A; DELAY=$DELAY_A; LOSS=$LOSS_A
else
    RATE=$RATE_B; DELAY=$DELAY_B; LOSS=$LOSS_B
fi

if [ "$ACTION" = "show" ]; then
    echo "router $ROUTER_PATH, towards the client ($IF_CLIENT): $(tc qdisc show dev "$IF_CLIENT" root)"
    echo "router $ROUTER_PATH, towards the server ($IF_SERVER): $(tc qdisc show dev "$IF_SERVER" root)"
    exit 0
fi

if [ "$ACTION" = "clear" ]; then
    $SUDO tc qdisc del dev "$IF_CLIENT" root 2>/dev/null
    $SUDO tc qdisc del dev "$IF_SERVER" root 2>/dev/null
    echo "router $ROUTER_PATH: shaping removed from $IF_CLIENT and $IF_SERVER"
    exit 0
fi

# Shapes the packets that leave through one interface
shape() {
    # Without this the network card joins TCP segments into "packets" of up to 64 kB before
    # the router forwards them: the queue would be counted wrongly, to the advantage of TCP.
    $SUDO ethtool -K "$1" tso off gso off gro off 2>/dev/null

    if ! $SUDO tc qdisc replace dev "$1" root netem rate "$RATE" delay "$DELAY" loss "$LOSS%" limit "$QUEUE"; then
        echo "error: cannot install netem on $1."
        echo "       On Ubuntu cloud images: sudo apt install linux-modules-extra-\$(uname -r)"
        exit 1
    fi
}

shape "$IF_SERVER"   # direction client -> server
shape "$IF_CLIENT"   # direction server -> client

echo "router $ROUTER_PATH: path $ROUTER_PATH = $RATE, delay $DELAY, loss $LOSS %, queue $QUEUE packets (both directions)"
