# Block Depth Threading

## Overview

Block depth threading is a work distribution algorithm that balances scanner thread workload based on the amount of blockchain data each address needs to process, rather than simply distributing addresses evenly across threads. This approach addresses the performance inefficiency where threads with fewer blocks to scan finish early and become idle while other threads continue processing.

## Motivation

The default threading algorithm distributes addresses evenly across threads, treating each address as an equal unit of work. However, addresses have vastly different synchronization requirements:
- A fully synced address may only need to process a few recent blocks
- A newly added address may need to scan hundreds of thousands of blocks

This imbalance causes threads with mostly synced addresses to finish quickly and remain idle while threads with unsynced addresses continue working, resulting in poor CPU utilization and longer overall sync times.

## Configuration

- `--block-depth-threading`: Enable block depth threading algorithm (default: false)
- `--min-block-depth`: Minimum block depth value for workload calculations (default: 16)

## Usage

Enable block depth threading with default settings:
```bash
monero-lws-daemon --block-depth-threading [other options]
```

Enable with custom minimum block depth:
```bash
monero-lws-daemon --block-depth-threading --min-block-depth=32 [other options]
```

Or in config file:
```
block-depth-threading=true
min-block-depth=32
```

## Algorithm

### Block Depth Calculation

For each address, the **block depth** is calculated as the number of blocks remaining to be scanned:

```
blockdepth = max(current_blockchain_height - address_scan_height, min_block_depth)
```

Where `min_block_depth` is the value specified by `--min-block-depth` (default: 16).

Addresses with blockdepth less than the minimum are assigned the minimum value. This prevents edge cases where fully synced addresses would have zero blockdepth, which could cause:
- Division by zero or near-zero values in workload calculations
- Degenerate cases where many fully-synced accounts get assigned together
- Poor workload distribution when most accounts are fully synced

### Minimum Block Depth

The `--min-block-depth` flag sets the minimum block depth value used in workload calculations. This ensures that even fully synced accounts contribute meaningfully to workload balancing.

**Default value**: 16 blocks

**Example**: With `--min-block-depth=16`, an account at the current blockchain height (0 blocks remaining) is treated as having 16 blocks remaining for workload distribution purposes.

**When to adjust**: You may want to increase this value if you have many fully synced accounts and want them to contribute more to workload balancing, or decrease it if you want more precise distribution for nearly-synced accounts.

### Thread Assignment

1. **Calculate total work**: Sum all address blockdepths to get `total_blockdepth`
2. **Calculate target per thread**: `blockdepth_per_thread = total_blockdepth / thread_count`
3. **Sort addresses**: Order by blockdepth (smallest first)
4. **Distribute to threads**: 
   - Addresses are assigned sequentially to threads
   - Accounts are added to the current thread until the cumulative depth reaches or exceeds the target
   - When target is reached, move to the next thread
   - Final thread receives any remaining addresses

This overallocation strategy ensures more balanced workload distribution and better thread utilization throughout the scanning process.

## Example

With 4 threads and 20 accounts with varying sync states:
- Accounts A-H: 16 blocks each (synced, at minimum) = 128 blocks
- Accounts I-L: 100 blocks each = 400 blocks
- Accounts M-P: 300 blocks each = 1,200 blocks
- Accounts Q-T: 500 blocks each = 2,000 blocks

**Old Algorithm** (by count, evenly distributed - 5 accounts per thread):
- Thread 0: A, B, C, D, E (80 blocks) ✓ finishes immediately
- Thread 1: F, G, H, I, J (228 blocks) ⏱
- Thread 2: K, L, M, N, O (1,028 blocks) ⏱⏱
- Thread 3: P, Q, R, S, T (2,100 blocks) ⏱⏱⏱⏱ takes much longer
- **Problem**: Despite equal account count (5 per thread), massive workload imbalance - thread 3 has 26x more work than thread 0

**New Algorithm** (by depth, balanced workload with overallocation):
- Total: 3,728 blocks, target: 932 blocks/thread
- Thread 0: A, B, C, D, E, F, G, H, I, J, K, L, M, N (1,128 blocks) - adds accounts until depth >= 932, then moves to next thread
- Thread 1: O, P, Q (1,100 blocks) - adds accounts until depth >= 932, then moves to next thread
- Thread 2: R, S (1,000 blocks) - adds accounts until depth >= 932, then moves to next thread
- Thread 3: T (500 blocks) - final thread receives remaining account
- **Result**: All 4 threads utilized with better balance (500-1,128 vs 80-2,100 blocks), synced accounts efficiently grouped


## Benefits

- **Improved parallelization**: All threads remain active longer
- **Reduced sync time**: More efficient CPU utilization
- **Better resource usage**: Eliminates idle threads waiting for others to complete
- **Predictable performance**: Workload is distributed based on actual work required
