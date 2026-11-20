#!/bin/bash
# ==============================================================================
# evaluation/scripts/run_all_scenarios.sh
# Master runner for all 4 evaluation scenarios:
#   1. Scenario 1 (scenario1) : Low BDP (Default Asymmetric: 8M/10ms + 4M/25ms)
#   2. Scenario 2 (scenario2) : Lossy Link (Low BDP with 1% random packet loss)
#   3. Scenario 3 (scenario3) : High BDP (Transcontinental / Long delay: 20M/100ms + 5M/150ms)
#   4. Scenario 4 (scenario4) : Short Transfer (256 KB file, test256K.bin)
#
# Followed by automatic generation of all graphs and tables!
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/config.sh"

RUNS=3
AUTO_SSH=${AUTO_SSH:-0}
ROUTER_A_SSH_HOST=${ROUTER_A_SSH_HOST:-"vyos@$ROUTER_A_CLIENT"}
ROUTER_B_SSH_HOST=${ROUTER_B_SSH_HOST:-"vyos@$ROUTER_B_CLIENT"}

while [ $# -gt 0 ]; do
    case "$1" in
        --runs) RUNS="$2"; shift 2 ;;
        --auto-ssh) AUTO_SSH=1; shift ;;
        --ssh-a) ROUTER_A_SSH_HOST="$2"; AUTO_SSH=1; shift 2 ;;
        --ssh-b) ROUTER_B_SSH_HOST="$2"; AUTO_SSH=1; shift 2 ;;
        *) shift ;;
    esac
done

echo "========================================================================"
echo "          MP-QUIC COMPLETE EVALUATION SUITE (ALL 4 SCENARIOS)           "
echo "========================================================================"
echo "This script executes all 4 MP-QUIC evaluation scenarios and generates"
echo "the final comparison performance tables and plots."
if [ "$AUTO_SSH" -eq 1 ]; then
    echo "Mode: Fully Automated via SSH to Routers ($ROUTER_A_SSH_HOST, $ROUTER_B_SSH_HOST)"
else
    echo "Mode: Interactive Prompt (Prompts you to shape routers at each scenario)"
fi
echo "========================================================================"

execute_scenario() {
    local scenario="$1"
    local desc="$2"
    local file_arg="$3"
    echo ""
    echo "========================================================================"
    echo "STEP: Running $scenario"
    echo "  Description: $desc"
    echo "========================================================================"

    if [ "$AUTO_SSH" -eq 1 ]; then
        echo ">>> Automatically configuring routers via SSH..."
        echo "Configuring Router A ($ROUTER_A_SSH_HOST)..."
        ssh -o BatchMode=yes -o ConnectTimeout=3 "$ROUTER_A_SSH_HOST" \
            "cd ~/MP_QUIC 2>/dev/null || cd MP_QUIC 2>/dev/null || true; ./evaluation/scripts/shape_router.sh $scenario --router a" 2>/dev/null || {
            echo "  [SSH FAILED on Router A]. Falling back to manual prompt..."
            AUTO_SSH=0
        }

        if [ "$AUTO_SSH" -eq 1 ]; then
            echo "Configuring Router B ($ROUTER_B_SSH_HOST)..."
            ssh -o BatchMode=yes -o ConnectTimeout=3 "$ROUTER_B_SSH_HOST" \
                "cd ~/MP_QUIC 2>/dev/null || cd MP_QUIC 2>/dev/null || true; ./evaluation/scripts/shape_router.sh $scenario --router b" 2>/dev/null || {
                echo "  [SSH FAILED on Router B]. Falling back to manual prompt..."
                AUTO_SSH=0
            }
        fi
    fi

    if [ "$AUTO_SSH" -eq 0 ]; then
        echo "Please shape Router A and Router B for '$scenario':"
        echo "  [Terminal Router A] : ./evaluation/scripts/shape_router.sh $scenario --router a"
        echo "  [Terminal Router B] : ./evaluation/scripts/shape_router.sh $scenario --router b"
        echo ""
        read -r -p "==> Once the router commands have been executed, press [Enter] to continue..."
    fi

    echo ""
    if [ -n "$file_arg" ]; then
        "$SCRIPT_DIR/run_benchmarks.sh" --scenario "$scenario" --runs "$RUNS" --file "$file_arg"
    else
        "$SCRIPT_DIR/run_benchmarks.sh" --scenario "$scenario" --runs "$RUNS"
    fi
}

# Scenario 1: Low BDP
execute_scenario "scenario1" "Scenario 1: Low BDP / Asymmetric (8M/10ms + 4M/25ms), 20MB file"

# Scenario 2: Lossy
execute_scenario "scenario2" "Scenario 2: Low BDP with 1% random loss (8M/10ms + 4M/25ms), 20MB file"

# Scenario 3: High BDP
execute_scenario "scenario3" "Scenario 3: High BDP / Long delay (20M/100ms + 5M/150ms), 20MB file"

# Scenario 4: Short Transfer
execute_scenario "scenario4" "Scenario 4: Short 256KB Web Object Transfer (8M/10ms + 4M/25ms)" "test256K.bin"

echo ""
echo "========================================================================"
echo "All 4 scenarios completed! Generating publication graphs..."
echo "========================================================================"
python3 "$BASE_DIR/evaluation/plot_benchmarks.py" --results-dir "$RESULTS" --out-dir "$PLOTS"

echo ""
echo "========================================================================"
echo "🎉 BENCHMARKING & PLOTTING COMPLETE! 🎉"
echo "Tables available in : $RESULTS/table_*.txt"
echo "CSVs available in   : $RESULTS/results_*.csv"
echo "Plots available in  : $PLOTS/ (*.png)"
echo "========================================================================"
