# Split Synced Threading

## Overview

The split-sync threading feature is an enhancement to the block-depth-threading algorithm that isolates synced addresses from unsynced addresses across different scanner threads. This prevents fully or near-fully synced addresses from being blocked by addresses that are still catching up to the blockchain.

Split-sync is enabled by setting `--split-sync-threads` to a value greater than 0, which specifies the percentage of threads to allocate for synced accounts.

## Requirements

- Requires `--block-depth-threading`
- Only affects initial thread assignment at startup
- `--split-sync-threads` accepts a value from 0-1 representing the percentage of threads for synced accounts (e.g., 0.25 = 25%)
- `--split-sync-depth` accepts a numeric value representing the maximum block depth for an address to be considered synced (defaults to 10)
- If `--split-sync-threads` is not provided or set to 0, split-sync is disabled

## Arguments

- `--split-sync-threads=[numthreadspercent]`: Specified as a number from 0-1, where 0.25 = 25%. Defaults to 0 (disabled) if not specified. Split-sync is enabled when this value is > 0.
- `--split-sync-depth=[synced depth]`: The maximum block depth for an account to be considered synced. Defaults to 10 if not specified. Accounts with `raw_blockdepth ≤ split-sync-depth` are considered synced.

## Behavior

### Address Classification

Addresses are classified as either **synced** or **unsynced** based on their block depth:
- **Synced address**: `raw_blockdepth ≤ --split-sync-depth` value (default 10)
- **Unsynced address**: `raw_blockdepth > --split-sync-depth` value

Note: The `--min-block-depth` parameter is only used for assigning a minimum block depth value to addresses for workload calculations, and is not used to determine if an address is synced.

### Thread Allocation Algorithm

When `--split-sync-threads > 0`:

1. **Calculate synced thread count**: The number of threads allocated for synced accounts is calculated as `ceil(split-sync-threads × total_threads)`, rounded up. The remaining threads are used for unsynced accounts.

2. **Separate accounts**: Accounts are classified as synced or unsynced based on `--split-sync-depth`.

3. **Distribute synced accounts**: Synced accounts are distributed across the allocated synced threads using **round-robin assignment** (account 0 → thread 0, account 1 → thread 1, ..., account N → thread (N % num_synced_threads)). Accounts are already sorted by block depth (smallest to largest) from the block-depth-threading preparation.

4. **Distribute unsynced accounts**: Unsynced accounts are distributed across the remaining threads using the **standard block-depth-threading algorithm** (alternating over/under-allocation strategy) starting from the first unsynced thread.

### Edge Cases

- **No synced accounts**: If there are no synced accounts, all threads are used for unsynced accounts with standard block-depth-threading.
- **Fewer synced accounts than threads**: If there are fewer synced accounts than threads allocated for synced accounts, only as many synced threads are used as there are synced accounts (one account per thread). The remaining threads are used for unsynced accounts.
- **Split-sync disabled**: If `--split-sync-threads` is 0 or not set (default), split-sync is disabled and all accounts use standard block-depth-threading.

## Example

With 4 threads, `--split-sync-threads=0.25`, and `--split-sync-depth=16`, and 20 accounts with varying sync states:
- Accounts A-H: 16 blocks each (synced) = 128 blocks
- Accounts I-L: 100 blocks each (unsynced) = 400 blocks
- Accounts M-P: 300 blocks each (unsynced) = 1,200 blocks
- Accounts Q-T: 500 blocks each (unsynced) = 2,000 blocks

**Thread allocation**:
- Synced threads: `ceil(0.25 × 4) = 1` thread
- Unsynced threads: `4 - 1 = 3` threads

**With split-sync enabled**:
- Thread 0 (synced, round-robin): A, B, C, D, E, F, G, H (8 accounts)
  - Contains: 8 synced (A-H) only - **synced thread**
- Thread 1 (unsynced, block-depth-threading): I, J, K, L, M (800 blocks)
  - Contains: 5 unsynced (I-M) only - **unsynced thread**
- Thread 2 (unsynced, block-depth-threading): N, O, P, Q (1,400 blocks)
  - Contains: 4 unsynced - **unsynced thread**
- Thread 3 (unsynced, block-depth-threading): R, S, T (1,500 blocks)
  - Contains: 3 unsynced - **unsynced thread**
- **Result**: Synced addresses (A-H) are isolated in Thread 0 and can process updates quickly without waiting for unsynced addresses. All unsynced addresses are separated into threads 1-3 using block-depth-threading.

**Without split-sync** (standard block-depth-threading):
- Total: 3,728 blocks, target: 932 blocks/thread
- Thread 0 (even, over-allocate): A, B, C, D, E, F, G, H, I, J, K, L, M, N (1,128 blocks)
  - Contains: 8 synced (A-H) + 6 unsynced (I-N) - **mixed**
- Thread 1 (odd, under-allocate): O, P (600 blocks)
- Thread 2 (even, over-allocate): Q, R (1,000 blocks)
- Thread 3 (odd, under-allocate): S, T (1,000 blocks)
- **Problem**: Synced addresses (A-H) are mixed with unsynced addresses (I-N) in Thread 0, causing synced addresses to wait for unsynced ones to catch up

## Benefits

This approach ensures that synced addresses receive timely updates without being delayed by the potentially lengthy synchronization process of newly added or far-behind addresses. By pre-allocating threads and using round-robin distribution for synced accounts, the algorithm provides predictable thread allocation and better isolation between synced and unsynced workloads.
