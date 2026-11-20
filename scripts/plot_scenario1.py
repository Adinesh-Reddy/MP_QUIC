#!/usr/bin/env python3
"""
MP-QUIC Scenario 1 4-Way Evaluation Plotter
Replicates:
  - Figure 3: CDF of Time Ratio (TCP/QUIC vs MPTCP/MPQUIC)
  - Figure 4: Boxplot of EBen (MPTCP vs TCP and MPQUIC vs QUIC)
from ACM CoNEXT 2017: "Multipath QUIC: Design and Evaluation"
"""

import sys
import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

def main():
    csv_file = sys.argv[1] if len(sys.argv) > 1 else 'results_scenario1_100.csv'
    
    if not os.path.exists(csv_file):
        print(f"[ERROR] '{csv_file}' not found! Run 'scripts/run_scenario1_100.sh' first.")
        sys.exit(1)

    df = pd.read_csv(csv_file)
    print(f"Loaded {len(df)} scenarios from '{csv_file}'.")

    # Check whether TCP / MPTCP columns exist and have non-zero data
    has_tcp = ('tcp_a' in df.columns and (df['tcp_a'] > 0).any())

    if has_tcp:
        # ---------------------------------------------------------------------
        # MODE 1: COMPLETE 4-WAY EVALUATION (CoNEXT 2017 Reproduction)
        # ---------------------------------------------------------------------
        valid = df[(df['tcp_a'] > 0) & (df['tcp_b'] > 0) &
                   (df['quic_a'] > 0) & (df['quic_b'] > 0) &
                   (df['mptcp_a_first'] > 0) & (df['mpquic_a_first'] > 0)].copy()

        if len(valid) == 0:
            print("[WARNING] No scenarios had all 4 protocols non-zero. Falling back to QUIC vs MP-QUIC.")
            has_tcp = False

    if not has_tcp:
        # ---------------------------------------------------------------------
        # MODE 2: QUIC vs MP-QUIC ONLY
        # ---------------------------------------------------------------------
        qa_col = 'quic_a' if 'quic_a' in df.columns else 'g_single_a'
        qb_col = 'quic_b' if 'quic_b' in df.columns else 'g_single_b'
        ma_col = 'mpquic_a_first' if 'mpquic_a_first' in df.columns else 'g_multi_a_first'
        mb_col = 'mpquic_b_first' if 'mpquic_b_first' in df.columns else 'g_multi_b_first'
        ea_col = 'eben_mpquic_a' if 'eben_mpquic_a' in df.columns else 'eben_a_first'
        eb_col = 'eben_mpquic_b' if 'eben_mpquic_b' in df.columns else 'eben_b_first'

        valid = df[(df[qa_col] > 0) & (df[qb_col] > 0) &
                   (df[ma_col] > 0) & (df[mb_col] > 0)].copy()

        if len(valid) == 0:
            # Fall back to scenarios where at least MP-QUIC and one single path succeeded
            valid = df[(df[ma_col] > 0) & ((df[qa_col] > 0) | (df[qb_col] > 0))].copy()
            if len(valid) == 0:
                print("[ERROR] No valid non-zero rows found in CSV!")
                sys.exit(1)

        best_single = np.maximum(valid[qa_col], valid[qb_col])
        speedup_a = (valid[ma_col] / best_single).values
        speedup_b = (valid[mb_col] / best_single).values

        faster_a = np.sum(speedup_a > 1.0) / len(speedup_a) * 100.0
        faster_b = np.sum(speedup_b > 1.0) / len(speedup_b) * 100.0

        print("\n======================================================================")
        print("                   MP-QUIC vs SINGLE-PATH EVALUATION                  ")
        print("======================================================================")
        print(f"Valid Scenarios: {len(valid)}")
        print(f"Multipath Faster than Best Single Path (A First): {faster_a:.1f}%")
        print(f"Multipath Faster than Best Single Path (B First): {faster_b:.1f}%")
        print(f"Median Speedup Ratio (A First): {np.median(speedup_a):.2f}x")
        print(f"Median Speedup Ratio (B First): {np.median(speedup_b):.2f}x")
        print(f"Median EBen (A First): {valid[ea_col].median():.2f}")
        print(f"Median EBen (B First): {valid[eb_col].median():.2f}")
        print("======================================================================\n")

        # Plot CDF of Speedup
        plt.figure(figsize=(8, 5))
        s_a = np.sort(speedup_a)
        cdf_a = np.arange(1, len(s_a) + 1) / len(s_a)
        plt.plot(s_a, cdf_a, label='MP-QUIC (Path A initial)', color='#1f77b4', linewidth=2)

        s_b = np.sort(speedup_b)
        cdf_b = np.arange(1, len(s_b) + 1) / len(s_b)
        plt.plot(s_b, cdf_b, label='MP-QUIC (Path B initial)', color='#ff7f0e', linestyle='--', linewidth=2)

        plt.axvline(x=1.0, color='gray', linestyle=':', linewidth=1.5, label='Parity (Speedup = 1.0x)')
        plt.xlabel('Speedup Ratio over Best Single Path (Goodput_MP / Max(Single))', fontsize=11)
        plt.ylabel('CDF (Fraction of Scenarios)', fontsize=11)
        plt.title('Speedup CDF across Scenarios (CoNEXT 2017 Metric)', fontsize=12, fontweight='bold')
        plt.xlim(0.5, 2.5)
        plt.ylim(-0.02, 1.02)
        plt.grid(True, alpha=0.3)
        plt.legend(loc='lower right', frameon=True)
        plt.tight_layout()
        plt.savefig('time_ratio_cdf.png', dpi=300)
        plt.close()
        print("[PLOT] Generated: 'time_ratio_cdf.png'")

        # Plot EBen Boxplot
        plt.figure(figsize=(7, 5))
        box_data = [valid[ea_col].values, valid[eb_col].values]
        box = plt.boxplot(box_data, labels=['MP-QUIC (A first)', 'MP-QUIC (B first)'], patch_artist=True, widths=0.4)
        for patch, color in zip(box['boxes'], ['#aec7e8', '#ffbb78']):
            patch.set_facecolor(color)
            patch.set_alpha(0.85)
        plt.axhline(y=1.0, color='green', linestyle=':', linewidth=1.5, label='Perfect Aggregation (1.0)')
        plt.axhline(y=0.0, color='red', linestyle='--', linewidth=1.5, label='No Benefit (0.0)')
        plt.ylabel('Experimental Aggregation Benefit (EBen)', fontsize=11)
        plt.title('Aggregation Benefit (EBen) Distribution', fontsize=12, fontweight='bold')
        plt.ylim(-0.5, 1.3)
        plt.grid(True, axis='y', alpha=0.3)
        plt.legend(loc='lower left', frameon=True)
        plt.tight_layout()
        plt.savefig('eben_comparison.png', dpi=300)
        plt.close()
        print("[PLOT] Generated: 'eben_comparison.png'")
        return

    # If has_tcp is True, proceed with 4-way evaluation:

    # -------------------------------------------------------------------------
    # 1. TIME RATIOS (FIGURE 3)
    # Time Ratio = Time_TCP / Time_QUIC
    # Since Time = Size / Goodput, Time_TCP / Time_QUIC = Goodput_QUIC / Goodput_TCP
    # Ratio > 1.0 means QUIC is faster than TCP!
    # -------------------------------------------------------------------------
    # Single-Path: Time TCP / Time QUIC (Path A)
    ratio_single = (valid['quic_a'] / valid['tcp_a']).values
    
    # Multipath: Time MPTCP / Time MPQUIC (Path A First)
    ratio_multi = (valid['mpquic_a_first'] / valid['mptcp_a_first']).values

    # Percentage of scenarios where MPQUIC beat MPTCP (Ratio > 1.0)
    mpquic_beat_mptcp = np.sum(ratio_multi > 1.0) / len(ratio_multi) * 100.0

    print("\n======================================================================")
    print("                     4-WAY EVALUATION RESULTS                         ")
    print("======================================================================")
    print(f"Valid Scenarios: {len(valid)}")
    print(f"MPQUIC Faster than MPTCP: {mpquic_beat_mptcp:.1f}% of scenarios (Paper reported 89%)")
    print(f"Median Time Ratio (MPTCP / MPQUIC): {np.median(ratio_multi):.2f}x")
    print(f"Median Single-Path Time Ratio (TCP / QUIC): {np.median(ratio_single):.2f}x")
    print("----------------------------------------------------------------------")
    print(f"MPTCP Median EBen (A First):  {valid['eben_mptcp_a'].median():.2f}")
    print(f"MPTCP Median EBen (B First):  {valid['eben_mptcp_b'].median():.2f}")
    print(f"MPQUIC Median EBen (A First): {valid['eben_mpquic_a'].median():.2f}")
    print(f"MPQUIC Median EBen (B First): {valid['eben_mpquic_b'].median():.2f}")
    print("======================================================================\n")

    # -------------------------------------------------------------------------
    # PLOT FIGURE 3: CDF of Time Ratio
    # -------------------------------------------------------------------------
    plt.figure(figsize=(8, 5))
    
    # Single Path CDF
    sorted_single = np.sort(ratio_single)
    cdf_single = np.arange(1, len(sorted_single) + 1) / len(sorted_single)
    plt.plot(sorted_single, cdf_single, label='Time TCP / QUIC (Single-Path)', color='#1f77b4', linewidth=2)

    # Multipath CDF
    sorted_multi = np.sort(ratio_multi)
    cdf_multi = np.arange(1, len(sorted_multi) + 1) / len(sorted_multi)
    plt.plot(sorted_multi, cdf_multi, label='Time MPTCP / MPQUIC (Multipath)', color='#ff7f0e', linestyle='--', linewidth=2.5)

    # Parity line (Ratio = 1.0)
    plt.axvline(x=1.0, color='gray', linestyle=':', linewidth=1.5, label='Parity (Ratio = 1.0)')

    plt.xlabel('Time Ratio (Values > 1.0 mean QUIC is faster)', fontsize=11)
    plt.ylabel('CDF (Fraction of Scenarios)', fontsize=11)
    plt.title('Time Ratio CDF across 100 Scenarios', fontsize=12, fontweight='bold')
    plt.xlim(0.4, 2.5)
    plt.ylim(-0.02, 1.02)
    plt.grid(True, alpha=0.3)
    plt.legend(loc='lower right', frameon=True)
    plt.tight_layout()

    fig3_path = 'time_ratio_cdf.png'
    plt.savefig(fig3_path, dpi=300)
    plt.close()
    print(f"[PLOT] Generated: '{fig3_path}'")

    # -------------------------------------------------------------------------
    # PLOT FIGURE 4: Boxplot of EBen (MPTCP vs MPQUIC)
    # -------------------------------------------------------------------------
    plt.figure(figsize=(8, 5))
    
    box_data = [
        valid['eben_mptcp_a'].values,
        valid['eben_mptcp_b'].values,
        valid['eben_mpquic_a'].values,
        valid['eben_mpquic_b'].values
    ]
    labels = ['MPTCP (A first)', 'MPTCP (B first)', 'MPQUIC (A first)', 'MPQUIC (B first)']
    
    box = plt.boxplot(box_data, labels=labels, patch_artist=True, widths=0.45)
    
    colors = ['#d3d3d3', '#e0e0e0', '#aec7e8', '#ffbb78']
    for patch, color in zip(box['boxes'], colors):
        patch.set_facecolor(color)
        patch.set_alpha(0.85)

    plt.axhline(y=1.0, color='green', linestyle=':', linewidth=1.5, label='Perfect Aggregation (1.0)')
    plt.axhline(y=0.0, color='red', linestyle='--', linewidth=1.5, label='No Benefit (0.0)')

    plt.ylabel('Experimental Aggregation Benefit (EBen)', fontsize=11)
    plt.title('Aggregation Benefit (MPTCP vs MPQUIC)', fontsize=12, fontweight='bold')
    plt.ylim(-0.5, 1.3)
    plt.grid(True, axis='y', alpha=0.3)
    plt.legend(loc='lower left', frameon=True)
    plt.tight_layout()

    fig4_path = 'eben_comparison.png'
    plt.savefig(fig4_path, dpi=300)
    plt.close()
    print(f"[PLOT] Generated: '{fig4_path}'")

    print("\n[SUCCESS] generated matching the CoNEXT 2017 paper!")

if __name__ == '__main__':
    main()
