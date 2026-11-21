#!/usr/bin/env python3
"""
MP-QUIC Scenario 2: Short 256 KB File Downloads Plotter
Generates:
  - CDF of Transfer Completion Time (us) - (MP)QUIC vs (MP)TCP
  - Aggregation Benefit (E_Ben) Distribution for Short Transfers
"""

import sys
import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

def main():
    csv_file = sys.argv[1] if len(sys.argv) > 1 else 'results_scenario2_short.csv'
    
    if not os.path.exists(csv_file):
        print(f"[ERROR] '{csv_file}' not found! Run 'scripts/run_scenario2_short.sh' first.")
        sys.exit(1)

    df = pd.read_csv(csv_file)
    print(f"Loaded {len(df)} short transfer scenarios from '{csv_file}'.")

    # Filter out invalid / zero rows if any
    valid = df[(df['time_us_quic_a'] > 0) | (df['time_us_mpquic_a'] > 0)].copy()
    if len(valid) == 0:
        print("[ERROR] No valid non-zero rows in CSV!")
        sys.exit(1)

    has_tcp = ('time_us_tcp_a' in valid.columns and (valid['time_us_tcp_a'] > 0).any())
    has_mptcp = ('time_us_mptcp_a' in valid.columns and (valid['time_us_mptcp_a'] > 0).any())

    print("\nScenario 2: Short 256 KB Download Summary")
    print(f"Valid Scenarios: {len(valid)}")
    print(f"Median QUIC Path A Time  : {valid['time_us_quic_a'].median():.1f} us")
    if 'time_us_mpquic_a' in valid.columns:
        print(f"Median MP-QUIC Time      : {valid['time_us_mpquic_a'].median():.1f} us")
    if has_tcp:
        print(f"Median TCP Path A Time   : {valid['time_us_tcp_a'].median():.1f} us")
        speedup = valid['time_us_tcp_a'].median() / valid['time_us_quic_a'].median()
        print(f"QUIC vs TCP Speedup      : {speedup:.2f}x")
    if 'eben_mpquic_a' in valid.columns:
        print(f"Median MP-QUIC E_Ben     : {valid['eben_mpquic_a'].median():.3f}")
    if has_mptcp and 'eben_mptcp_a' in valid.columns:
        print(f"Median MPTCP E_Ben       : {valid['eben_mptcp_a'].median():.3f}")
    print("")

    # Plot 1: CDF of completion time (us)
    plt.figure(figsize=(8, 5.5))
    
    # Single-Path QUIC
    quic_times = np.sort(valid[valid['time_us_quic_a'] > 0]['time_us_quic_a'].values)
    quic_cdf = np.arange(1, len(quic_times) + 1) / len(quic_times)
    plt.plot(quic_times, quic_cdf, label='Single-Path QUIC (Path A)', color='#1f77b4', linewidth=2.5)

    # Multipath QUIC
    if 'time_us_mpquic_a' in valid.columns and (valid['time_us_mpquic_a'] > 0).any():
        mpq_times = np.sort(valid[valid['time_us_mpquic_a'] > 0]['time_us_mpquic_a'].values)
        mpq_cdf = np.arange(1, len(mpq_times) + 1) / len(mpq_times)
        plt.plot(mpq_times, mpq_cdf, label='MP-QUIC (Path A initial)', color='#2ca02c', linewidth=2.2, linestyle='-')

    # Single-Path TCP
    if has_tcp:
        tcp_times = np.sort(valid[valid['time_us_tcp_a'] > 0]['time_us_tcp_a'].values)
        tcp_cdf = np.arange(1, len(tcp_times) + 1) / len(tcp_times)
        plt.plot(tcp_times, tcp_cdf, label='Single-Path TCP (Path A)', color='#d62728', linewidth=2.0, linestyle='--')

    # Multipath TCP
    if has_mptcp:
        mptcp_times = np.sort(valid[valid['time_us_mptcp_a'] > 0]['time_us_mptcp_a'].values)
        mptcp_cdf = np.arange(1, len(mptcp_times) + 1) / len(mptcp_times)
        plt.plot(mptcp_times, mptcp_cdf, label='MPTCP (Path A initial)', color='#ff7f0e', linewidth=2.0, linestyle=':')

    plt.xlabel('Download Completion Time (us)', fontsize=12)
    plt.ylabel('CDF (Fraction of Transfers)', fontsize=12)
    plt.title('Short 256 KB Download Completion Time CDF', fontsize=13, fontweight='bold')
    plt.grid(True, alpha=0.3)
    plt.legend(loc='lower right', frameon=True, fontsize=10)
    plt.ylim(-0.02, 1.02)
    plt.tight_layout()

    out_cdf = 'completion_time_cdf.png'
    plt.savefig(out_cdf, dpi=300)
    plt.close()
    print(f"[SAVED] {out_cdf}")

    # -------------------------------------------------------------------------
    # PLOT 2: AGGREGATION BENEFIT (E_Ben) BOXPLOT
    # -------------------------------------------------------------------------
    plt.figure(figsize=(7, 5))
    
    box_data = []
    box_labels = []

    if has_mptcp and 'eben_mptcp_a' in valid.columns:
        mptcp_eben = valid['eben_mptcp_a'].dropna().values
        box_data.append(mptcp_eben)
        box_labels.append('MPTCP\n(Path A Init)')

    if 'eben_mpquic_a' in valid.columns:
        mpq_eben = valid['eben_mpquic_a'].dropna().values
        box_data.append(mpq_eben)
        box_labels.append('MP-QUIC\n(Path A Init)')

    if 'eben_mpquic_b' in valid.columns and (valid['eben_mpquic_b'] != 0).any():
        mpq_b_eben = valid['eben_mpquic_b'].dropna().values
        box_data.append(mpq_b_eben)
        box_labels.append('MP-QUIC\n(Path B Init)')

    if box_data:
        bplot = plt.boxplot(box_data, tick_labels=box_labels, patch_artist=True,
                            medianprops=dict(color='black', linewidth=2.5),
                            widths=0.45)
        
        colors = ['#ffbb78', '#98df8a', '#aec7e8']
        for patch, color in zip(bplot['boxes'], colors[:len(box_data)]):
            patch.set_facecolor(color)
            patch.set_alpha(0.8)

        plt.axhline(0, color='red', linestyle='--', linewidth=1.5, label='E_Ben = 0 (No Multipath Benefit)')
        plt.ylabel('Aggregation Benefit (E_Ben)', fontsize=12)
        plt.title('Aggregation Benefit (E_Ben) for Short Transfers\n(Demonstrating Multipath Disadvantage for Small Objects)', fontsize=12, fontweight='bold')
        plt.ylim(-1.0, 1.0)
        plt.grid(True, axis='y', alpha=0.3)
        plt.legend(loc='upper right', frameon=True)
        plt.tight_layout()

        out_boxplot = 'eben_short_boxplot.png'
        plt.savefig(out_boxplot, dpi=300)
        plt.close()
        print(f"[SAVED] {out_boxplot}")

    print("Done.")

if __name__ == '__main__':
    main()
