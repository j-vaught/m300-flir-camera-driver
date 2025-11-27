#!/usr/bin/env python3

import os
import re
import numpy as np
import matplotlib.pyplot as plt
from scipy import stats

# Test directories
test_dirs = {
    'V1 vis.0 (1920x1080 H.264)': 'test_results/v1_vis0',
    'V1 vis.1 (1280x720 MJPEG)': 'test_results/v1_vis1',
    'V2 vis.0 (1920x1080 H.264)': 'test_results/v2_vis0',
    'V2 vis.1 (1280x720 MJPEG)': 'test_results/v2_vis1',
}

fig, axes = plt.subplots(2, 2, figsize=(14, 10))
fig.suptitle('Frame Encoding Latency Analysis (Outliers Removed)', fontsize=16, fontweight='bold')

axes = axes.flatten()

for idx, (label, directory) in enumerate(test_dirs.items()):
    latencies = []

    # Extract latencies from filenames
    if os.path.exists(directory):
        files = os.listdir(directory)
        for filename in files:
            # Filename format: YYYY.MM.DD_HH.MM.SS.mmm_HW_hwTime_XXms.jpg
            match = re.search(r'_(\d+)ms\.jpg$', filename)
            if match:
                latency_ms = int(match.group(1))
                latencies.append(latency_ms)

    if not latencies:
        print(f"{label}: No files found")
        continue

    latencies = np.array(latencies)
    original_count = len(latencies)

    # Remove outliers using IQR method
    Q1 = np.percentile(latencies, 25)
    Q3 = np.percentile(latencies, 75)
    IQR = Q3 - Q1
    lower_bound = Q1 - 1.5 * IQR
    upper_bound = Q3 + 1.5 * IQR

    filtered_latencies = latencies[(latencies >= lower_bound) & (latencies <= upper_bound)]
    outlier_count = original_count - len(filtered_latencies)

    # Statistics
    mean_latency = np.mean(filtered_latencies)
    median_latency = np.median(filtered_latencies)
    std_latency = np.std(filtered_latencies)
    min_latency = np.min(filtered_latencies)
    max_latency = np.max(filtered_latencies)

    # Plot histogram
    ax = axes[idx]
    ax.hist(filtered_latencies, bins=40, color='steelblue', edgecolor='black', alpha=0.7)
    ax.axvline(mean_latency, color='red', linestyle='--', linewidth=2, label=f'Mean: {mean_latency:.1f}ms')
    ax.axvline(median_latency, color='green', linestyle='--', linewidth=2, label=f'Median: {median_latency:.1f}ms')

    ax.set_xlabel('Latency (ms)', fontsize=10)
    ax.set_ylabel('Frame Count', fontsize=10)
    ax.set_title(label, fontsize=11, fontweight='bold')
    ax.legend(fontsize=9)
    ax.grid(axis='y', alpha=0.3)

    # Stats text
    stats_text = f'Frames: {len(filtered_latencies)}\nOutliers: {outlier_count}\nRange: {min_latency}-{max_latency}ms\nStdDev: {std_latency:.2f}ms'
    ax.text(0.98, 0.97, stats_text, transform=ax.transAxes, fontsize=8,
            verticalalignment='top', horizontalalignment='right',
            bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))

    # Print summary
    print(f"\n{label}")
    print(f"  Total frames: {original_count}")
    print(f"  Outliers removed: {outlier_count}")
    print(f"  Valid frames: {len(filtered_latencies)}")
    print(f"  Mean latency: {mean_latency:.2f}ms")
    print(f"  Median latency: {median_latency:.2f}ms")
    print(f"  Std deviation: {std_latency:.2f}ms")
    print(f"  Min latency: {min_latency}ms")
    print(f"  Max latency: {max_latency}ms")

plt.tight_layout()
plt.savefig('/media/samsung/projects/Dual_FLIR_cpp_multi-stage/camera-driver/latency_analysis.png', dpi=150, bbox_inches='tight')
print("\n\nHistogram saved to: latency_analysis.png")
plt.show()
