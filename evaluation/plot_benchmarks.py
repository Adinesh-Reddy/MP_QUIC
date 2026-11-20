#!/usr/bin/env python3
"""
evaluation/plot_benchmarks.py
Comprehensive 4-Protocol Benchmarking & Plotting Suite.
Directly compares:
  1. UDP (Capacity ceiling)
  2. TCP (Single-path baseline)
  3. MPTCP (Linux kernel Multipath TCP)
  4. MP-QUIC (Our implementation)

Plots generated in evaluation/plots/:
  - fig1_protocol_comparison_4way.png : Direct 4-protocol Goodput comparison across all scenarios
  - fig2_aggregation_benefit.png      : Aggregation Benefit (EBen) comparing MP-QUIC vs MPTCP
  - fig3_detailed_breakdown.png       : Complete breakdown of single-path vs multipath for all protocols
  - fig4_traffic_distribution.png     : Link A vs Link B traffic split (% share) for MP-QUIC & MPTCP
  - fig5_summary_dashboard.png        : All-in-one 4-panel slide-ready dashboard
"""

import os
import sys
import glob
import csv
import argparse
import statistics
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

def parse_args():
    parser = argparse.ArgumentParser(description="Plot MP-QUIC 4-Protocol Benchmark Results")
    parser.add_argument("--results-dir", default="evaluation/results", help="Directory containing results_*.csv")
    parser.add_argument("--out-dir", default="evaluation/plots", help="Directory to save generated plots")
    return parser.parse_args()

def format_scenario_label(s):
    labels = {
        "scenario1": "Scenario 1\n(Low BDP)",
        "scenario2": "Scenario 2\n(Lossy 1%)",
        "scenario3": "Scenario 3\n(High BDP)",
        "scenario4": "Scenario 4\n(Short 256K)",
        "low_bdp": "Scenario 1\n(Low BDP)",
        "lossy": "Scenario 2\n(Lossy 1%)",
        "high_bdp": "Scenario 3\n(High BDP)",
        "short": "Scenario 4\n(Short 256K)",
        "short_transfer": "Scenario 4\n(Short 256K)",
    }
    return labels.get(s.lower(), s.upper().replace("_", " "))

def load_scenario_results(results_dir):
    csv_files = glob.glob(os.path.join(results_dir, "results_*.csv"))
    if not csv_files:
        print(f"Warning: No results_*.csv files found in {results_dir}")
        return {}

    scenarios = {}
    for fpath in sorted(csv_files):
        basename = os.path.basename(fpath)
        scenario_name = basename.replace("results_", "").replace(".csv", "")
        
        with open(fpath, "r") as f:
            reader = csv.DictReader(f)
            data = {}
            for row in reader:
                if row.get("status") != "ok":
                    continue
                proto = row.get("protocol", "mpquic")
                cfg = row.get("config", "")
                key = (proto, cfg)
                goodput_kbps = float(row.get("goodput_kbps", 0))
                time_ms = float(row.get("time_ms", 0))
                rx_a = float(row.get("rx_a_bytes", 0))
                rx_b = float(row.get("rx_b_bytes", 0))

                if key not in data:
                    data[key] = {
                        "goodput_mbps": [],
                        "time_ms": [],
                        "rx_a": 0.0,
                        "rx_b": 0.0
                    }
                data[key]["goodput_mbps"].append(goodput_kbps / 1000.0)
                data[key]["time_ms"].append(time_ms)
                data[key]["rx_a"] += rx_a
                data[key]["rx_b"] += rx_b
            
            if data:
                scenarios[scenario_name] = data

    return scenarios

def calc_eben(gm, ga, gb):
    gmax = max(ga, gb)
    if gm >= gmax:
        denom = (ga + gb - gmax)
        return (gm - gmax) / denom if denom > 0 else 0.0
    else:
        return (gm - gmax) / gmax if gmax > 0 else 0.0

def get_scenario_metrics(data):
    """Computes median values for UDP, TCP, MPTCP, and MP-QUIC."""
    def med(proto, cfg):
        vals = data.get((proto, cfg), {}).get("goodput_mbps", [])
        return statistics.median(vals) if vals else 0.0

    # UDP Capacity
    udp_a = med('udp', 'single_a')
    udp_b = med('udp', 'single_b')
    udp_total = udp_a + udp_b

    # TCP
    tcp_a = med('tcp', 'single_a')
    tcp_b = med('tcp', 'single_b')
    tcp_best = max(tcp_a, tcp_b)

    # MPTCP
    mptcp_a = med('mptcp', 'multipath_a_first')
    mptcp_b = med('mptcp', 'multipath_b_first')
    mptcp_val = max(mptcp_a, mptcp_b)

    # MP-QUIC
    mpq_a = med('mpquic', 'single_a')
    mpq_b = med('mpquic', 'single_b')
    mpq_multi_a = med('mpquic', 'multipath_a_first')
    mpq_multi_b = med('mpquic', 'multipath_b_first')
    mpq_multi = max(mpq_multi_a, mpq_multi_b)

    return {
        'udp_a': udp_a, 'udp_b': udp_b, 'udp_total': udp_total,
        'tcp_a': tcp_a, 'tcp_b': tcp_b, 'tcp_best': tcp_best,
        'mptcp_multi': mptcp_val, 'mptcp_a': mptcp_a, 'mptcp_b': mptcp_b,
        'mpq_a': mpq_a, 'mpq_b': mpq_b, 'mpq_multi': mpq_multi,
        'mpq_multi_a': mpq_multi_a, 'mpq_multi_b': mpq_multi_b
    }

# ==============================================================================
# Plot 1: Direct 4-Protocol Goodput Comparison
# ==============================================================================
def plot_4way_goodput_comparison(scenarios, out_dir):
    scenario_names = list(scenarios.keys())
    if not scenario_names:
        return

    udp_vals = []
    tcp_vals = []
    mptcp_vals = []
    mpquic_vals = []

    for s in scenario_names:
        m = get_scenario_metrics(scenarios[s])
        udp_vals.append(m['udp_total'])
        tcp_vals.append(m['tcp_best'])
        mptcp_vals.append(m['mptcp_multi'])
        mpquic_vals.append(m['mpq_multi'])

    fig, ax = plt.subplots(figsize=(11, 6), dpi=300)
    x = np.arange(len(scenario_names))
    width = 0.20

    # 4 distinct protocol bars
    has_udp = any(v > 0 for v in udp_vals)
    has_tcp = any(v > 0 for v in tcp_vals)
    has_mptcp = any(v > 0 for v in mptcp_vals)

    bars = []
    if has_udp:
        r0 = ax.bar(x - 1.5*width, udp_vals, width, label='UDP (Theoretical Capacity)', color='#17becf', edgecolor='black', hatch='//', alpha=0.85)
        bars.append(r0)
    if has_tcp:
        r1 = ax.bar(x - 0.5*width, tcp_vals, width, label='TCP (Single Best Path)', color='#7f7f7f', edgecolor='black', alpha=0.9)
        bars.append(r1)
    if has_mptcp:
        r2 = ax.bar(x + 0.5*width, mptcp_vals, width, label='MPTCP (Kernel Multipath)', color='#d62728', edgecolor='black', alpha=0.9)
        bars.append(r2)
    
    r3 = ax.bar(x + (1.5*width if has_mptcp else 0.5*width), mpquic_vals, width, label='MP-QUIC (Our Multipath QUIC)', color='#2ca02c', edgecolor='black', linewidth=1.5, alpha=0.95)
    bars.append(r3)

    for r_group in bars:
        for rect in r_group:
            h = rect.get_height()
            if h > 0:
                ax.annotate(f'{h:.1f}',
                            xy=(rect.get_x() + rect.get_width() / 2, h),
                            xytext=(0, 3), textcoords="offset points",
                            ha='center', va='bottom', fontsize=8, fontweight='bold')

    ax.set_ylabel("Goodput (Mbps)", fontsize=12, fontweight='bold')
    ax.set_title("Protocol Comparison: UDP vs TCP vs MPTCP vs MP-QUIC", fontsize=14, fontweight='bold', pad=15)
    ax.set_xticks(x)
    ax.set_xticklabels([format_scenario_label(s) for s in scenario_names], fontsize=10, fontweight='bold')
    ax.legend(frameon=True, facecolor='white', framealpha=0.9, fontsize=9, loc='upper left')
    ax.grid(axis='y', linestyle='--', alpha=0.5)
    ax.set_ylim(bottom=0)

    plt.tight_layout()
    out_file = os.path.join(out_dir, "fig1_protocol_comparison_4way.png")
    plt.savefig(out_file)
    plt.close()
    print(f"  [SAVED] {out_file}")

# ==============================================================================
# Plot 2: Aggregation Benefit (EBen) - MP-QUIC vs MPTCP
# ==============================================================================
def plot_aggregation_benefit(scenarios, out_dir):
    scenario_names = list(scenarios.keys())
    if not scenario_names:
        return

    mp_ebens = []
    mptcp_ebens = []
    scen_labels = []

    for s in scenario_names:
        m = get_scenario_metrics(scenarios[s])
        if m['mpq_a'] > 0 and m['mpq_b'] > 0 and m['mpq_multi'] > 0:
            mp_ebens.append(calc_eben(m['mpq_multi'], m['mpq_a'], m['mpq_b']))
            scen_labels.append(format_scenario_label(s))

            if m['tcp_a'] > 0 and m['tcp_b'] > 0 and m['mptcp_multi'] > 0:
                mptcp_ebens.append(calc_eben(m['mptcp_multi'], m['tcp_a'], m['tcp_b']))
            else:
                mptcp_ebens.append(None)

    if not mp_ebens:
        return

    fig, ax = plt.subplots(figsize=(8.5, 5), dpi=300)
    x = np.arange(len(scen_labels))
    width = 0.35

    rects1 = ax.bar(x - width/2 if any(mptcp_ebens) else x, mp_ebens, width,
                    label="MP-QUIC", color="#2ca02c", edgecolor='black')
    for rect in rects1:
        h = rect.get_height()
        ax.annotate(f'{h:.2f}',
                    xy=(rect.get_x() + rect.get_width() / 2, max(h, 0)),
                    xytext=(0, 3), textcoords="offset points",
                    ha='center', va='bottom', fontsize=9, fontweight='bold')

    if any(mptcp_ebens):
        mptcp_vals = [v if v is not None else 0 for v in mptcp_ebens]
        rects2 = ax.bar(x + width/2, mptcp_vals, width, label="MPTCP", color="#d62728", edgecolor='black')
        for rect in rects2:
            h = rect.get_height()
            if h > 0:
                ax.annotate(f'{h:.2f}',
                            xy=(rect.get_x() + rect.get_width() / 2, h),
                            xytext=(0, 3), textcoords="offset points",
                            ha='center', va='bottom', fontsize=9, fontweight='bold')

    ax.axhline(1.0, color='blue', linestyle='--', linewidth=1.5, label='Ideal Aggregation (EBen = 1.0)')
    ax.axhline(0.0, color='gray', linestyle='-', linewidth=0.8)

    ax.set_ylabel("Aggregation Benefit (EBen)", fontsize=12, fontweight='bold')
    ax.set_title("Bandwidth Aggregation Benefit (EBen): MP-QUIC vs MPTCP", fontsize=13, fontweight='bold', pad=15)
    ax.set_xticks(x)
    ax.set_xticklabels(scen_labels, fontsize=10, fontweight='bold')
    ax.set_ylim(-0.2, 1.25)
    ax.grid(axis='y', linestyle='--', alpha=0.5)
    ax.legend(frameon=True, facecolor='white', loc='lower right', fontsize=9)

    plt.tight_layout()
    out_file = os.path.join(out_dir, "fig2_aggregation_benefit.png")
    plt.savefig(out_file)
    plt.close()
    print(f"  [SAVED] {out_file}")

# ==============================================================================
# Plot 3: Detailed Breakdown (Single vs Multipath across all protocols)
# ==============================================================================
def plot_detailed_breakdown(scenarios, out_dir):
    scenario_names = list(scenarios.keys())
    if not scenario_names:
        return

    configs = [
        (('udp', 'single_a'), 'UDP Link A', '#a6cee3'),
        (('udp', 'single_b'), 'UDP Link B', '#b2df8a'),
        (('tcp', 'single_a'), 'TCP Link A', '#6baed6'),
        (('tcp', 'single_b'), 'TCP Link B', '#9ecae1'),
        (('mptcp', 'multipath_a_first'), 'MPTCP (Both)', '#fb6a4a'),
        (('mpquic', 'single_a'), 'MP-QUIC Link A', '#31a354'),
        (('mpquic', 'single_b'), 'MP-QUIC Link B', '#a1d99b'),
        (('mpquic', 'multipath_a_first'), 'MP-QUIC (Both)', '#006d2c'),
    ]

    active_configs = []
    for cfg_key, label, color in configs:
        if any(cfg_key in scenarios[s] for s in scenario_names):
            active_configs.append((cfg_key, label, color))

    if not active_configs:
        return

    fig, ax = plt.subplots(figsize=(12, 6), dpi=300)
    x = np.arange(len(scenario_names))
    total_bars = len(active_configs)
    width = 0.85 / total_bars

    for i, (cfg_key, label, color) in enumerate(active_configs):
        means = []
        for s in scenario_names:
            vals = scenarios[s].get(cfg_key, {}).get("goodput_mbps", [])
            means.append(statistics.median(vals) if vals else 0.0)

        offset = (i - total_bars / 2 + 0.5) * width
        rects = ax.bar(x + offset, means, width, label=label, color=color, edgecolor='black', alpha=0.9)
        for rect in rects:
            h = rect.get_height()
            if h > 0:
                ax.annotate(f'{h:.1f}',
                            xy=(rect.get_x() + rect.get_width() / 2, h),
                            xytext=(0, 2), textcoords="offset points",
                            ha='center', va='bottom', fontsize=7, rotation=90)

    ax.set_ylabel("Goodput (Mbps)", fontsize=12, fontweight='bold')
    ax.set_title("Detailed Protocol Breakdown (Single-Path vs Multipath)", fontsize=13, fontweight='bold', pad=15)
    ax.set_xticks(x)
    ax.set_xticklabels([format_scenario_label(s) for s in scenario_names], fontsize=10, fontweight='bold')
    ax.legend(frameon=True, facecolor='white', fontsize=8, loc='upper left', ncol=2)
    ax.grid(axis='y', linestyle='--', alpha=0.5)
    ax.set_ylim(bottom=0)

    plt.tight_layout()
    out_file = os.path.join(out_dir, "fig3_detailed_breakdown.png")
    plt.savefig(out_file)
    plt.close()
    print(f"  [SAVED] {out_file}")

# ==============================================================================
# Plot 4: Traffic Distribution across Links (MP-QUIC vs MPTCP)
# ==============================================================================
def plot_traffic_distribution(scenarios, out_dir):
    scenario_names = list(scenarios.keys())
    if not scenario_names:
        return

    scen_labels = []
    pct_a_list = []
    pct_b_list = []

    for s in scenario_names:
        data = scenarios[s]
        entry = data.get(('mpquic', 'multipath_a_first'))
        if not entry:
            entry = data.get(('mpquic', 'multipath_b_first'))
        if entry:
            tot = entry['rx_a'] + entry['rx_b']
            if tot > 0:
                pct_a = (entry['rx_a'] / tot) * 100.0
                pct_b = (entry['rx_b'] / tot) * 100.0
                pct_a_list.append(pct_a)
                pct_b_list.append(pct_b)
                scen_labels.append(format_scenario_label(s))

    if not scen_labels:
        return

    fig, ax = plt.subplots(figsize=(8.5, 5), dpi=300)
    x = np.arange(len(scen_labels))
    width = 0.45

    p1 = ax.bar(x, pct_a_list, width, label='Link A (Fast / Low Delay)', color='#1f77b4', edgecolor='black')
    p2 = ax.bar(x, pct_b_list, width, bottom=pct_a_list, label='Link B (Slow / High Delay)', color='#ff7f0e', edgecolor='black')

    for i in range(len(x)):
        ax.text(x[i], pct_a_list[i] / 2, f"{pct_a_list[i]:.1f}%", ha='center', va='center', color='white', fontweight='bold', fontsize=10)
        ax.text(x[i], pct_a_list[i] + pct_b_list[i] / 2, f"{pct_b_list[i]:.1f}%", ha='center', va='center', color='white', fontweight='bold', fontsize=10)

    ax.set_ylabel("Traffic Share (%)", fontsize=12, fontweight='bold')
    ax.set_title("Traffic Distribution Across Disjoint Links (MP-QUIC)", fontsize=13, fontweight='bold', pad=15)
    ax.set_xticks(x)
    ax.set_xticklabels(scen_labels, fontsize=10, fontweight='bold')
    ax.set_ylim(0, 105)
    ax.legend(loc='upper right', frameon=True)
    ax.grid(axis='y', linestyle='--', alpha=0.5)

    plt.tight_layout()
    out_file = os.path.join(out_dir, "fig4_traffic_distribution.png")
    plt.savefig(out_file)
    plt.close()
    print(f"  [SAVED] {out_file}")

# ==============================================================================
# Plot 5: All-in-One 4-Panel Dashboard
# ==============================================================================
def plot_dashboard(scenarios, out_dir):
    scenario_names = list(scenarios.keys())
    if not scenario_names:
        return

    fig = plt.figure(figsize=(15, 10), dpi=300)
    fig.suptitle("MP-QUIC Protocol Evaluation Dashboard (4-Node Testbed)", fontsize=16, fontweight='bold', y=0.98)

    # Panel 1: 4-Way Goodput (Top Left)
    ax1 = fig.add_subplot(2, 2, 1)
    x = np.arange(len(scenario_names))
    width = 0.20

    udp_vals = [get_scenario_metrics(scenarios[s])['udp_total'] for s in scenario_names]
    tcp_vals = [get_scenario_metrics(scenarios[s])['tcp_best'] for s in scenario_names]
    mptcp_vals = [get_scenario_metrics(scenarios[s])['mptcp_multi'] for s in scenario_names]
    mpquic_vals = [get_scenario_metrics(scenarios[s])['mpq_multi'] for s in scenario_names]

    ax1.bar(x - 1.5*width, udp_vals, width, label='UDP Cap.', color='#17becf', hatch='//')
    ax1.bar(x - 0.5*width, tcp_vals, width, label='TCP Best', color='#7f7f7f')
    ax1.bar(x + 0.5*width, mptcp_vals, width, label='MPTCP', color='#d62728')
    r_mp = ax1.bar(x + 1.5*width, mpquic_vals, width, label='MP-QUIC', color='#2ca02c')

    for r in r_mp:
        h = r.get_height()
        if h > 0:
            ax1.annotate(f"{h:.1f}", xy=(r.get_x() + r.get_width()/2, h), xytext=(0, 2),
                         textcoords="offset points", ha='center', va='bottom', fontsize=8, fontweight='bold')

    ax1.set_ylabel("Goodput (Mbps)", fontweight='bold')
    ax1.set_title("Goodput: UDP vs TCP vs MPTCP vs MP-QUIC", fontweight='bold')
    ax1.set_xticks(x)
    ax1.set_xticklabels([format_scenario_label(s) for s in scenario_names], fontsize=8, fontweight='bold')
    ax1.legend(fontsize=8, loc='upper left')
    ax1.grid(axis='y', linestyle='--', alpha=0.5)

    # Panel 2: Aggregation Benefit (Top Right)
    ax2 = fig.add_subplot(2, 2, 2)
    ebens_mp = []
    ebens_mptcp = []
    for s in scenario_names:
        m = get_scenario_metrics(scenarios[s])
        ebens_mp.append(calc_eben(m['mpq_multi'], m['mpq_a'], m['mpq_b']) if m['mpq_a'] > 0 and m['mpq_b'] > 0 else 0)
        ebens_mptcp.append(calc_eben(m['mptcp_multi'], m['tcp_a'], m['tcp_b']) if m['tcp_a'] > 0 and m['tcp_b'] > 0 else 0)

    w_eben = 0.35
    ax2.bar(x - w_eben/2, ebens_mp, w_eben, label='MP-QUIC', color='#2ca02c', edgecolor='black')
    ax2.bar(x + w_eben/2, ebens_mptcp, w_eben, label='MPTCP', color='#d62728', edgecolor='black')
    ax2.axhline(1.0, color='blue', linestyle='--', label='Ideal Aggregation (1.0)')

    for i in range(len(x)):
        if ebens_mp[i] > 0:
            ax2.annotate(f"{ebens_mp[i]:.2f}", xy=(x[i] - w_eben/2, ebens_mp[i]), xytext=(0, 2),
                         textcoords="offset points", ha='center', va='bottom', fontsize=8, fontweight='bold')

    ax2.set_ylabel("EBen Score", fontweight='bold')
    ax2.set_title("Aggregation Benefit (EBen): MP-QUIC vs MPTCP", fontweight='bold')
    ax2.set_xticks(x)
    ax2.set_xticklabels([format_scenario_label(s) for s in scenario_names], fontsize=8, fontweight='bold')
    ax2.set_ylim(-0.1, 1.25)
    ax2.legend(fontsize=8, loc='lower right')
    ax2.grid(axis='y', linestyle='--', alpha=0.5)

    # Panel 3: Traffic Share (Bottom Left)
    ax3 = fig.add_subplot(2, 2, 3)
    p_a = []
    p_b = []
    for s in scenario_names:
        entry = scenarios[s].get(('mpquic', 'multipath_a_first'))
        if entry and (entry['rx_a'] + entry['rx_b']) > 0:
            tot = entry['rx_a'] + entry['rx_b']
            p_a.append((entry['rx_a'] / tot) * 100)
            p_b.append((entry['rx_b'] / tot) * 100)
        else:
            p_a.append(50)
            p_b.append(50)

    ax3.bar(x, p_a, 0.4, label='Path A', color='#1f77b4')
    ax3.bar(x, p_b, 0.4, bottom=p_a, label='Path B', color='#ff7f0e')
    for i in range(len(x)):
        ax3.text(x[i], p_a[i] / 2, f"{p_a[i]:.0f}%", ha='center', va='center', color='white', fontweight='bold', fontsize=9)
        ax3.text(x[i], p_a[i] + p_b[i] / 2, f"{p_b[i]:.0f}%", ha='center', va='center', color='white', fontweight='bold', fontsize=9)
    ax3.set_ylabel("Traffic Share (%)", fontweight='bold')
    ax3.set_title("MP-QUIC Traffic Split Across Paths", fontweight='bold')
    ax3.set_xticks(x)
    ax3.set_xticklabels([format_scenario_label(s) for s in scenario_names], fontsize=8, fontweight='bold')
    ax3.legend(fontsize=8)
    ax3.grid(axis='y', linestyle='--', alpha=0.5)

    # Panel 4: Complete 4-Protocol Numerical Table (Bottom Right)
    ax4 = fig.add_subplot(2, 2, 4)
    ax4.axis('tight')
    ax4.axis('off')

    table_data = [["Scenario", "UDP Cap", "TCP", "MPTCP", "MP-QUIC", "EBen"]]
    for i, s in enumerate(scenario_names):
        m = get_scenario_metrics(scenarios[s])
        e = calc_eben(m['mpq_multi'], m['mpq_a'], m['mpq_b']) if m['mpq_a'] > 0 and m['mpq_b'] > 0 else 0
        table_data.append([
            format_scenario_label(s).replace("\n", " "),
            f"{m['udp_total']:.1f} M" if m['udp_total'] > 0 else "-",
            f"{m['tcp_best']:.1f} M" if m['tcp_best'] > 0 else "-",
            f"{m['mptcp_multi']:.1f} M" if m['mptcp_multi'] > 0 else "-",
            f"{m['mpq_multi']:.1f} M",
            f"{e:.2f}"
        ])

    table = ax4.table(cellText=table_data, loc='center', cellLoc='center')
    table.auto_set_font_size(False)
    table.set_fontsize(9)
    table.scale(1.1, 1.8)
    for j in range(6):
        table[(0, j)].set_facecolor('#d3d3d3')
        table[(0, j)].get_text().set_weight('bold')
    ax4.set_title("Complete 4-Protocol Results Summary", fontweight='bold', pad=10)

    plt.tight_layout(rect=[0, 0, 1, 0.95])
    out_file = os.path.join(out_dir, "fig5_summary_dashboard.png")
    plt.savefig(out_file)
    plt.close()
    print(f"  [SAVED] {out_file}")

def main():
    args = parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    scenarios = load_scenario_results(args.results_dir)

    if not scenarios:
        print(f"No valid benchmark results found in {args.results_dir}.")
        print("Run ./evaluation/scripts/run_benchmarks.sh first.")
        sys.exit(1)

    print("=== Generating MP-QUIC 4-Protocol Evaluation Graphs ===")
    plot_4way_goodput_comparison(scenarios, args.out_dir)
    plot_aggregation_benefit(scenarios, args.out_dir)
    plot_detailed_breakdown(scenarios, args.out_dir)
    plot_traffic_distribution(scenarios, args.out_dir)
    plot_dashboard(scenarios, args.out_dir)
    print("=== All 4-Protocol Evaluation Graphs Successfully Generated ===")

if __name__ == "__main__":
    main()
