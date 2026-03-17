# Balance New Addresses

## Overview

The `--balance-new-addresses` option changes how new addresses are assigned to scanner threads while the scanner is actively running. Instead of using round-robin distribution, addresses are assigned to threads based on their current scanning progress, ensuring better workload distribution and faster synchronization.

## Motivation

The default round-robin algorithm assigns new addresses sequentially to threads without considering their current state. This can lead to:
- New addresses being assigned to threads that are far behind, causing unnecessary delays
- Imbalanced workload when addresses with different scan heights are added
- Inefficient thread utilization when threads finish at different times

The balance-new-addresses algorithm addresses these issues by intelligently matching new addresses to threads based on their scanning progress.

## Configuration

- `--balance-new-addresses`: Enable balanced assignment of new addresses to threads (default: false)

## Usage

Enable balanced new address assignment:
```bash
monero-lws-daemon --balance-new-addresses [other options]
```

Or in config file:
```
balance-new-addresses=true
```

## Algorithm

### Thread Selection Criteria

When a new address is added (e.g., an inactive account becomes active), the algorithm selects a thread using the following priority:

1. **Primary preference**: Thread with the highest scan height that is **not above** the account's scan height
   - If multiple threads meet this criteria, choose the one with the fewest addresses
   
2. **Fallback**: If all threads are above the account's scan height, choose the thread with the **lowest** scan height
   - This minimizes backtracking when all threads have already passed the account's starting point
   - If multiple threads have the same lowest height, choose the one with the fewest addresses

### Scan Height Tracking

Each thread maintains a `current_min_height` value representing the minimum scan height of all accounts currently assigned to that thread. This value is:
- Updated when new accounts are added to the thread (if the new account has a lower scan height)
- Updated periodically as the thread processes blocks and advances its scanning progress
- Used by the algorithm to determine which thread is best suited for a new account

### Scope

- **Only affects local threads**: The algorithm only applies to threads running on the local daemon. When enabled and local threads are available, **all new addresses are assigned exclusively to local threads**. Remote scanner clients will not receive any new addresses unless there are zero local threads available.
- **Only affects new addresses**: This algorithm is used when addresses are added dynamically (e.g., inactive accounts becoming active). Initial thread assignment at startup uses the standard block-depth-threading or round-robin algorithms.
- **Works with other threading options**: Can be used alongside `--block-depth-threading` and `--split-synced` options.

## Example

Consider a scenario with 3 local threads and a new account that needs to be assigned:

**Thread states:**
- Thread 0: scanning at height 3,000,000 (5 accounts)
- Thread 1: scanning at height 3,100,000 (3 accounts)
- Thread 2: scanning at height 2,900,000 (7 accounts)

**Scenario 1: New account at height 3,050,000**
- Thread 0: height 3,000,000 ≤ 3,050,000 ✓
- Thread 1: height 3,100,000 > 3,050,000 ✗
- Thread 2: height 2,900,000 ≤ 3,050,000 ✓
- **Result**: Thread 0 is selected (highest height ≤ account height, and fewer accounts than Thread 2)

**Scenario 2: New account at height 2,800,000 (all threads are above)**
- Thread 0: height 3,000,000 > 2,800,000 ✗
- Thread 1: height 3,100,000 > 2,800,000 ✗
- Thread 2: height 2,900,000 > 2,800,000 ✗
- **Result**: Thread 2 is selected (lowest height among all threads, minimizing backtracking)

**Scenario 3: New account at height 3,200,000 (all threads are below)**
- Thread 0: height 3,000,000 ≤ 3,200,000 ✓
- Thread 1: height 3,100,000 ≤ 3,200,000 ✓
- Thread 2: height 2,900,000 ≤ 3,200,000 ✓
- **Result**: Thread 1 is selected (highest height ≤ account height, and fewer accounts than Thread 0)

## Comparison with Round-Robin

**Round-Robin Algorithm** (default):
- Assigns addresses sequentially: Thread 0, Thread 1, Thread 2, Thread 0, ...
- Does not consider thread state or account scan height
- Simple and predictable, but can lead to suboptimal assignments

**Balance New Addresses Algorithm**:
- Considers each thread's current scanning progress
- Matches accounts to threads based on scan height compatibility
- More complex, but provides better workload distribution and faster synchronization

## When New Addresses Are Added

The balance-new-addresses algorithm is triggered when:
- An inactive account becomes active (status changes from inactive to active)
- New accounts are detected during periodic checks (every 10 seconds)

It is **not** triggered by:
- Initial thread assignment at startup (uses block-depth-threading or round-robin)
- Full account reassignment (e.g., after rescan operations)
- Accounts assigned to remote scanner clients

## Benefits

- **Faster synchronization**: New addresses are assigned to threads that are already at or near their scan height
- **Reduced backtracking**: Minimizes cases where threads need to scan backwards to process new accounts
- **Better workload distribution**: Accounts are distributed based on actual thread progress rather than arbitrary rotation
- **Improved thread utilization**: Threads that are ahead receive new work more efficiently

## Limitations

- **Remote threads excluded**: When enabled and local threads are available, remote scanner clients will not receive any new addresses. Remote threads only receive work if there are zero local threads available.
- Does not affect initial thread assignment at startup
- Requires tracking thread scan heights, which adds minimal overhead
- May not provide benefits if all threads are at similar heights
- **Important**: If you have remote scanner clients and want them to receive new addresses, do not enable this option, or ensure you have zero local scanner threads configured

