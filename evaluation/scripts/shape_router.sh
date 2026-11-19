#!/bin/bash
# ==============================================================================
# evaluation/scripts/shape_router.sh
# Applies tc netem link shaping on Router A or Router B.
#
# Supported Scenarios:
#   scenario1 (or 1, low_bdp)    : Scenario 1 - Low BDP / Asymmetric (8M/10ms + 4M/25ms)
#   scenario2 (or 2, lossy)      : Scenario 2 - Low BDP with 1% random loss
#   scenario3 (or 3, high_bdp)   : Scenario 3 - High BDP (20M/100ms + 5M/150ms)
#   scenario4 (or 4, short)      : Scenario 4 - Short Transfer (8M/10ms + 4M/25ms)
#   show                         : Display current tc netem configuration
#   reset (or clear)             : Clear all shaping
#
# Example usage:
#   ./shape_router.sh scenario1 --router a
#   ./shape_router.sh scenario2 --router b
#   ./shape_router.sh 3 --router a
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/config.sh"

detect_role >/dev/null 2>&1

MANUAL_ROUTER=""
RAW_SCENARIO=""

while [ $# -gt 0 ]; do
    case "$1" in
        --router)
            MANUAL_ROUTER="$2"
            shift 2
            ;;
        1|2|3|4|scenario1|scenario2|scenario3|scenario4|low_bdp|default|lossy|high_bdp|short|short_transfer|similar|show|reset|clear)
            RAW_SCENARIO="$1"
            shift
            ;;
        *)
            echo "Usage: $0 [scenario1|scenario2|scenario3|scenario4|reset|show] [--router a|b]"
            exit 1
            ;;
    esac
done

if [ -n "$MANUAL_ROUTER" ]; then
    ROUTER_PATH="$MANUAL_ROUTER"
    ROLE="router"
    if [ "$ROUTER_PATH" = "a" ]; then
        IF_CLIENT=$(interface_of "$ROUTER_A_CLIENT")
        IF_SERVER=$(interface_of "$ROUTER_A_SERVER")
    else
        IF_CLIENT=$(interface_of "$ROUTER_B_CLIENT")
        IF_SERVER=$(interface_of "$ROUTER_B_SERVER")
    fi
fi

# Fallback interface names if not bound by IP (e.g. eth0, eth1 on VyOS)
if [ -z "$IF_CLIENT" ] || [ -z "$IF_SERVER" ]; then
    IF_CLIENT="eth0"
    IF_SERVER="eth1"
fi

if [ -z "$ROUTER_PATH" ]; then
    HOSTNAME=$(hostname 2>/dev/null)
    if [[ "$HOSTNAME" =~ [aA] ]]; then
        ROUTER_PATH="a"
    elif [[ "$HOSTNAME" =~ [bB] ]]; then
        ROUTER_PATH="b"
    else
        echo "Please specify router: $0 <scenario> --router a|b"
        exit 1
    fi
fi

if [ "$RAW_SCENARIO" = "show" ]; then
    echo "=== Current Traffic Control Shaping (Router $ROUTER_PATH) ==="
    echo "Interface to Client ($IF_CLIENT):"
    tc qdisc show dev "$IF_CLIENT" root 2>/dev/null || true
    echo "Interface to Server ($IF_SERVER):"
    tc qdisc show dev "$IF_SERVER" root 2>/dev/null || true
    exit 0
fi

if [ "$RAW_SCENARIO" = "reset" ] || [ "$RAW_SCENARIO" = "clear" ]; then
    echo "Clearing shaping on Router $ROUTER_PATH ($IF_CLIENT, $IF_SERVER)..."
    $SUDO tc qdisc del dev "$IF_CLIENT" root 2>/dev/null || true
    $SUDO tc qdisc del dev "$IF_SERVER" root 2>/dev/null || true
    echo "Shaping cleared on Router $ROUTER_PATH."
    exit 0
fi

# Standardize scenario names
case "$RAW_SCENARIO" in
    1|scenario1|low_bdp|default|"")
        SCENARIO_NAME="scenario1 (Low BDP / Asymmetric)"
        if [ "$ROUTER_PATH" = "a" ]; then
            RATE="8mbit"; DELAY="10ms"; LOSS="0%"; QUEUE="50"
        else
            RATE="4mbit"; DELAY="25ms"; LOSS="0%"; QUEUE="50"
        fi
        ;;
    2|scenario2|lossy)
        SCENARIO_NAME="scenario2 (Low BDP with 1% Loss)"
        if [ "$ROUTER_PATH" = "a" ]; then
            RATE="8mbit"; DELAY="10ms"; LOSS="1%"; QUEUE="50"
        else
            RATE="4mbit"; DELAY="25ms"; LOSS="1%"; QUEUE="50"
        fi
        ;;
    3|scenario3|high_bdp)
        SCENARIO_NAME="scenario3 (High BDP / Long Delay)"
        if [ "$ROUTER_PATH" = "a" ]; then
            RATE="20mbit"; DELAY="100ms"; LOSS="0%"; QUEUE="200"
        else
            RATE="5mbit"; DELAY="150ms"; LOSS="0%"; QUEUE="200"
        fi
        ;;
    4|scenario4|short|short_transfer)
        SCENARIO_NAME="scenario4 (Short 256KB Transfer)"
        if [ "$ROUTER_PATH" = "a" ]; then
            RATE="8mbit"; DELAY="10ms"; LOSS="0%"; QUEUE="50"
        else
            RATE="4mbit"; DELAY="25ms"; LOSS="0%"; QUEUE="50"
        fi
        ;;
    similar)
        SCENARIO_NAME="scenario_similar (Symmetric 10M/10ms)"
        RATE="10mbit"; DELAY="10ms"; LOSS="0%"; QUEUE="50"
        ;;
    *)
        echo "Unknown scenario: $RAW_SCENARIO"
        echo "Valid choices: 1, 2, 3, 4, scenario1, scenario2, scenario3, scenario4, reset, show"
        exit 1
        ;;
esac

echo "=========================================================="
echo "Applying '$SCENARIO_NAME' on Router $ROUTER_PATH"
echo "  Path $ROUTER_PATH Profile: Rate=$RATE, Delay=$DELAY (one-way), Loss=$LOSS, Queue=$QUEUE pkts"
echo "  Interfaces: $IF_CLIENT (client-facing) & $IF_SERVER (server-facing)"
echo "=========================================================="

apply_shaping() {
    local iface="$1"
    $SUDO ethtool -K "$iface" tso off gso off gro off 2>/dev/null || true
    if ! $SUDO tc qdisc replace dev "$iface" root netem rate "$RATE" delay "$DELAY" loss "$LOSS" limit "$QUEUE"; then
        echo "Error: Failed to install netem on $iface."
        exit 1
    fi
}

apply_shaping "$IF_CLIENT"
apply_shaping "$IF_SERVER"

echo "Router $ROUTER_PATH shaping active:"
echo "  -> $IF_CLIENT: $(tc qdisc show dev "$IF_CLIENT" root)"
echo "  -> $IF_SERVER: $(tc qdisc show dev "$IF_SERVER" root)"
