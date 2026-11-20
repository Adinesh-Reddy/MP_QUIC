#!/usr/bin/env python3
"""
MP-QUIC Scenario 1 Evaluation Plotter
Replicates Figure 3 (CDF of Speedup) and Figure 4 (EBen Boxplot)
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
        print(f"[ERROR] Could not find '{csv_file}'. Run 'scripts/run_scenario1_100.sh' first!")
        sys.exit(1)

    df = pd.read_csv(csv_file)
    num_scenarios = len(df)
    print(f"Loaded {num_scenarios} scenarios from '{csv_file}'.")

    # Filter out any failed runs (where goodput is 0)
    valid_df = df[(df['g_single_a'] > 0) & (df['g_single_b'] > 0) & 
                  (df['g_multi_a_first'] > 0) & (df['g_multi_b_first'] > 0)].copy()

    if len(valid_df) == 0:
        print("[ERROR] No valid non-zero data points found in CSV!")
        sys.exit(1)

    # 1. Calculate Speedup Ratio (Multipath Goodput / Best Single Path Goodput)
    best_single = np.maximum(valid_df['g_single_a'], valid_df['g_single_b'])
    ratio_a = (valid_df['g_multi_a_first'] / best_single).values
    ratio_b = (valid_df['g_multi_b_first'] / best_single).values

    # Statistical summaries
    faster_a = np.sum(ratio_a > 1.0) / len(ratio_a) * 100.0
    faster_b = np.sum(ratio_b > 1.0) / len(ratio_b) * 100.0
    median_ratio_a = np.median(ratio_a)
    median_ratio_b = np.median(ratio_b)
    median_eben_a = valid_df['eben_a_first'].median()
    median_eben_b = valid_df['eben_b_first'].median()

    print("\n======================================================================")
    print("                    EVALUATION SUMMARY STATISTICS                     ")
    print("======================================================================")
    print(f"Total Scenarios Evaluated: {len(valid_df)}")
    print(f"Multipath Faster than Best Single Path (A First): {faster_a:.1f}%")
    print(f"Multipath Faster than Best Single Path (B First): {faster_b:.1f}%")
    print(f"Median Speedup Ratio (A First): {median_ratio_a:.2f}x")
    print(f"Median Speedup Ratio (B First): {median_ratio_b:.2f}x")
    print(f"Median Aggregation Benefit EBen (A First): {median_eben_a:.2f}")
    print(f"Median Aggregation Benefit EBen (B First): {median_eben_b:.2f}")
    print("======================================================================\n")

    # -------------------------------------------------------------------------
    # FIGURE 3: CDF of Speedup Ratio
    # -------------------------------------------------------------------------
    plt.figure(figsize=(8, 5))
    
    # Sort for CDF calculation
    sorted_a = np.sort(ratio_a)
    cdf_a = np.arange(1, len(sorted_a) + 1) / len(sorted_a)
    
    sorted_b = np.sort(ratio_b)
    cdf_b = np.arange(1, len(sorted_b) + 1) / len(sorted_b)

    plt.plot(sorted_a, cdf_a, label='MP-QUIC (Path A First)', color='#1f77b4', linewidth=2.5)
    plt.plot(sorted_b, cdf_b, label='MP-QUIC (Path B First)', color='#ff7f0e', linestyle='--', linewidth=2.5)

    # Parity reference line (Ratio = 1.0 means Multipath == Single Path)
    plt.axvline(x=1.0, color='gray', linestyle=':', linewidth=1.5, label='Parity (Ratio = 1.0)')

    plt.xlabel('Speedup Ratio (Multipath Goodput / Best Single Path Goodput)', fontsize=11)
    plt.ylabel('CDF (Fraction of Scenarios)', fontsize=11)
    plt.title(f'Figure 3 Reproduction: CDF of Multipath Speedup ({num_scenarios} Scenarios)', fontsize=13, fontweight='bold')
    plt.xlim(left=0.5, right=max(2.2, np.max(sorted_a) + 0.1))
    plt.ylim(-0.02, 1.02)
    plt.grid(True, alpha=0.3)
    plt.legend(loc='lower right', frameon=True)
    plt.tight_layout()

    fig3_path = 'figure3_speedup_cdf.png'
    plt.savefig(fig3_path, dpi=300)
    plt.close()
    print(f"[PLOT] Generated CDF plot: '{fig3_path}'")

    # -------------------------------------------------------------------------
    # FIGURE 4: Boxplot of EBen Aggregation Benefit
    # -------------------------------------------------------------------------
    plt.figure(figsize=(7, 5))
    
    box_data = [valid_df['eben_a_first'].values, valid_df['eben_b_first'].values]
    box = plt.boxplot(box_data, labels=['Path A First', 'Path B First'], patch_artist=True, widths=0.45)
    
    # Style the box plot
    colors = ['#aec7e8', '#ffbb78']
    for patch, color in zip(box['boxes'], colors):
        patch.set_facecolor(color)
        patch.set_alpha(0.8)

    plt.axhline(y=1.0, color='green', linestyle=':', linewidth=1.5, label='Perfect Aggregation (1.0)')
    plt.axhline(y=0.0, color='red', linestyle='--', linewidth=1.5, label='No Benefit (0.0)')

    plt.ylabel('Experimental Aggregation Benefit (EBen)', fontsize=11)
    plt.title(f'Figure 4 Reproduction: Aggregation Benefit EBen ({num_scenarios} Scenarios)', fontsize=13, fontweight='bold')
    plt.ylim(-0.2, 1.3)
    plt.grid(True, axis='y', alpha=0.3)
    plt.legend(loc='lower left', frameon=True)
    plt.tight_layout()

    fig4_path = 'figure4_eben_boxplot.png'
    plt.savefig(fig4_path, dpi=300)
    plt.close()
    print(f"[PLOT] Generated Boxplot: '{fig4_path}'")

    print("\nAll figures successfully generated in current directory.")

if __name__ == '__main__':
    main()
