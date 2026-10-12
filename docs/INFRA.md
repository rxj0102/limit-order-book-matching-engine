# Lock-free infrastructure

Covers include/lobkit/infra (currently the SPSC ring buffer), how it was verified, and what it measured. Every number below was measured in the environment in section 6, which is a virtual machine. Read section 7 before quoting any of them.

## 1. SPSC ring

One producer thread, one consumer thread, power-of-two capacity fixed at construction, no allocation after construction, no blocking. A full ring makes `try_push` fail and an empty ring makes `try_pop` fail; the caller chooses to spin, yield or drop.

    lobkit::SpscRing<Event> ring(4096);
    ring.try_push(e);                 // producer thread only
    ring.try_push_n(batch, n);        // returns how many fit
    ring.try_pop(e);                  // consumer thread only
    ring.try_pop_n(out, n);           // returns how many were available

The element type must be trivially copyable. The class does not (and cannot) check that only one thread calls each side.

### Layout

Two counters, tail (owned by the producer) and head (owned by the consumer), count total items ever pushed and popped. They are never wrapped, only masked when indexing. That makes full equal to tail minus head being capacity and empty equal to tail being head, so every slot is usable and there is no ambiguity, and unsigned subtraction stays correct when the 64 bit counters themselves overflow (tested).

Each side keeps a private cached copy of the other side counter and reloads the shared one only when the cached value says the ring looks full (producer) or empty (consumer). A stale cache can only make a side more pessimistic, never wrong. In the steady state a side touches only its own line.

    line A:  tail (atomic, producer writes)   cached_head (producer private)
    line B:  head (atomic, consumer writes)   cached_tail (consumer private)
    line C:  mask, slot pointer               (read only after construction)
    slots :  separate, line aligned allocation

Each group is aligned to `kCacheLine` bytes and the class is padded to a multiple of it, so no two groups, and no neighbouring heap object, share a line.

### Memory orders

The orders are justified in comments next to every atomic operation in include/lobkit/infra/spsc_ring.hpp. In summary:

| Operation | Order | Why |
|---|---|---|
| Own counter load (tail by producer, head by consumer) | relaxed | Only this thread ever writes it, so it reads its own latest value |
| Publish: tail store after writing the slot | release | Makes the slot contents visible to a consumer that acquires the new tail |
| Consumer load of tail (when its cache says empty) | acquire | Pairs with the release above, so the slot write happens before the consumer read |
| Free: head store after reading the slot | release | Orders the consumer read of the slot before any later producer overwrite |
| Producer load of head (when its cache says full) | acquire | Pairs with the release above, so the slot is really free before it is overwritten |
| size_approx loads | relaxed | The result is a statistic, no data is accessed based on it |

No operation needs seq_cst: each counter has a single writer, and no decision depends on a store-to-load ordering between two different variables (there is no Dekker pattern). The ablation in section 5 measures what seq_cst would cost.

## 2. How it was verified

| Check | Result |
|---|---|
| Unit tests: capacity validation, empty and full edges, exact fill to capacity, FIFO across many wraparounds, counter overflow (indices started 5 below 2^64), batch partial counts, batches straddling the physical end, single and batch mixed | 11 tests pass |
| Timed stress test (tests/infra/test_spsc_stress.cpp): producer emits a strictly increasing sequence with a checksum word, consumer verifies every item. Capacities 2, 8 and 1024, single and batch (random sizes 1 to 32), 0.4 s each by default, LOBKIT_STRESS_SECONDS to lengthen. Fails on reordering, loss, duplication or a torn read | passes, in debug, release, ASan, UBSan and TSan |
| ThreadSanitizer | clean, with the stress test running under it |
| The comparison queues (mutex, naive atomic) run the same stress test | pass, so the comparison is between correct queues |

Does the TSan run actually have teeth? The memory orders were weakened one at a time and the stress test re-run under TSan (0.3 s per configuration):

| Change to the ring | Outcome |
|---|---|
| tail store release to relaxed | TSan reports a data race |
| producer load of head acquire to relaxed | TSan reports a data race |
| consumer load of tail acquire to relaxed | TSan reports a data race |
| head store release to relaxed | TSan reports a data race |
| full check off by one (producer may overwrite the oldest unread slot) | TSan reports a data race |

This was a one-off manual check, like the mutation check in TESTING.md. It shows the test setup detects each of these classes of mistake. It does not prove the final orders are minimal or that no other bug exists; TSan only sees executions that happened, and x86 hides many ordering bugs at runtime (the TSan model does not).

## 3. Cache line size: measured, not assumed

`std::hardware_destructive_interference_size` reports 64 here. GCC warns that using it in a header is an ABI hazard because it varies with compiler flags, and on some Intel cores the adjacent line prefetcher can make two lines in a 128 byte pair interfere. So the line size is a project constant, `kCacheLine` in include/lobkit/infra/cache_line.hpp, set to 128 by default and overridable with `-DLOBKIT_CACHE_LINE=<bytes>`.

I measured the interference distance with bench/bench_false_sharing.cpp: two pinned threads each hammer their own counter, the counters are placed `stride` bytes apart, and throughput is compared with a stride of 512. Two probes were used. A plain load and store loop turned out to be a weak probe here (a line shared in a loop that can sit in the store buffer cost only 0 to 25 percent and drowned in noise), so the conclusion rests on the locked `fetch_add` probe, which needs exclusive ownership of the line on every operation:

| Stride in bytes | cpus 1,2: relative throughput | cpus 0,3: relative throughput |
|---|---|---|
| 8, 16, 32, 56 | 0.20 | 0.19 to 0.20 |
| 64 | 1.00 | 1.04 |
| 72 | 1.00 | 1.00 |
| 120, 128, 136 | 1.01 to 1.03 | 1.01 to 1.04 |
| 192, 256, 512 | 1.00 to 1.03 | 0.94 to 1.01 |

Reading: two variables in the same 64 byte line slow each other about 5 times. At 64 bytes the penalty is gone, and, importantly, the 64 byte stride puts the variables in the same 128 byte aligned pair, so there is no sign of adjacent line pair interference on this machine. The ablation in section 5 agrees: a ring padded to 64 and one padded to 128 are indistinguishable (105 and 104 million items per second).

So the measured requirement here is 64 bytes. The default is 128 anyway, because the cost is a few hundred bytes per ring and I could not reproduce, in this environment, the bare-metal Intel behaviour that motivates it. If you build for a machine where you have measured that 64 suffices and want the smaller footprint, pass `-DLOBKIT_CACHE_LINE=64`. If anyone changes the default, re-run bench_false_sharing on the target and record the result in docs/results.

Raw output: docs/results/phase3/false_sharing_cpu1_2.txt and false_sharing_cpu0_3.txt.

## 4. Throughput and latency against the baselines

Baselines (bench/queues.hpp) share the ring storage scheme so that only synchronization and layout differ:

- MutexQueue: a ring guarded by std::mutex. Batch calls take the lock once for the whole batch.
- NaiveAtomicQueue: both indices are std::atomic with default seq_cst, adjacent in one line, every call loads the other index. Batch calls are loops of single calls, because there is no cached view to amortize.

### Throughput (items per second, saturated, 4096 slots, 8 byte payload)

Interleaved round-robin run (bench/bench_interleaved.cpp), 21 rounds of 2,000,000 items each, producer on cpu 1 and consumer on cpu 2. Median with min to max in parentheses, in million items per second. The run was done twice, the second row is the rerun.

| Queue | Batch 1 | Batch 1 (rerun) | Batch 64 | Batch 64 (rerun) |
|---|---|---|---|---|
| SpscRing | 103 (30 to 124) | 98 (49 to 184) | 188 (158 to 234) | 206 (148 to 314) |
| MutexQueue | 4.1 (3.4 to 5.1) | 4.3 (3.6 to 5.4) | 40 (32 to 46) | 40 (34 to 82) |
| NaiveAtomicQueue | 5.1 (4.5 to 6.4) | 7.2 (6.0 to 31) | 8.0 (6.2 to 8.8) | 7.8 (5.6 to 28) |

On this machine the ring is roughly 23 to 25 times the mutex queue and 14 to 20 times the naive atomic queue for single items, and about 5 times the mutex queue and 25 times the naive queue with batches of 64. The naive queue gains nothing from batching, as expected, since its batch is a loop.

The same suite run with Google Benchmark (bench/bench_queues.cpp, benchmarks one after another, 7 repetitions of 2 transfers, median) gave much lower ring numbers, because it ran at a time when this VM was in a slower state (see section 7):

| Queue | Payload | Batch 1 | Batch 16 | Batch 64 |
|---|---|---|---|---|
| SpscRing | 8 B | 24.6 | 124 | 190 |
| MutexQueue | 8 B | 2.7 | 21.9 | 38.4 |
| NaiveAtomicQueue | 8 B | 7.6 | 7.6 | 7.5 |
| SpscRing | 48 B | 18.0 | 61.0 | 120.6 |
| MutexQueue | 48 B | 3.4 | 22.8 | 31.4 |
| NaiveAtomicQueue | 48 B | 6.6 | 7.1 | 7.2 |

The coefficient of variation across repetitions was 3 to 14 percent within the run. The ranking of the three queues is the same in every run I made; the absolute ring numbers moved by a factor of four between runs. Treat the ratios as the finding and the absolute values as a range on this host.

### Latency (one way, producer stamp to consumer receipt)

A producer creates an item every 2000 ns (busy waiting on the TSC), so the queue stays nearly empty and the measurement is hand-off cost, not queueing behind a backlog. 100,000 items per run, the first 10 percent discarded, 5 runs, median across runs of each percentile. TSC 2.1 GHz, timer overhead 26 ns median (lfence, rdtsc, lfence back to back). There were no negative differences in any run (no cross core TSC skew visible). Values in nanoseconds.

Hand-off latency, measured from the moment the batch was pushed (the cost of the queue):

| Queue | Batch | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| SpscRing | 1 | 347 | 372 | 5,028 | 135,679 | 222,043 |
| NaiveAtomic | 1 | 443 | 490 | 10,831 | 107,890 | 187,487 |
| MutexQueue | 1 | 1,550 | 1,799 | 45,472 | 336,796 | 609,443 |
| SpscRing | 16 | 491 | 557 | 4,144 | 206,584 | 365,833 |
| MutexQueue | 16 | 1,230 | 12,505 | 26,000 | 56,761 | 237,624 |
| NaiveAtomic | 16 | 2,697 | 3,300 | 23,495 | 332,154 | 428,158 |
| SpscRing | 64 | 837 | 1,118 | 3,563 | 125,672 | 134,420 |
| NaiveAtomic | 64 | 8,779 | 10,469 | 32,102 | 273,864 | 400,370 |
| MutexQueue | 64 | 12,800 | 15,109 | 41,336 | 264,796 | 397,653 |

End to end latency, measured from item creation, includes the wait for a batch to fill (with batch 64 at one item per 2 microseconds, the first item waits 126 microseconds). Median and p99 for the ring: batch 1 376 and 5,127; batch 16 16,494 and 44,140; batch 64 64,993 and 140,196. The other queues are within 10 to 15 percent of those at batch 16 and 64 because the fill time dominates; they differ at batch 1 (mutex 1,580 median, naive 473). The full tables are in docs/results/phase3/run2_latency_and_interleaved.txt. Batching trades latency for throughput; a pipeline that needs low latency per message should push single items or small batches.

Reading the tails: p99 of 4 to 45 microseconds and p99.9 of 100 to 340 microseconds are not queue behaviour. A busy-polling consumer and producer on a VM get descheduled by the host, and the tails look the same for all three queues. The medians and p90 values are the part of this table that describes the queues.

## 5. What each design decision is worth (ablation)

bench_interleaved also runs a single-item copy of the ring with each decision switched off in turn (AblationRing in bench/queues.hpp), batch 1, 8 byte payload, 21 interleaved rounds, median in million items per second (min to max). The production ring is included as a control and agrees with the ablation baseline. Run 1 and rerun:

| Variant | Run 1 | Rerun | Verdict |
|---|---|---|---|
| Control: production SpscRing | 100 (50 to 133) | 103 (71 to 124) | |
| Padded to 128, cached index, acquire/release (the design) | 104 (46 to 177) | 110 (49 to 131) | baseline |
| Padded to 64 | 105 (49 to 171) | 102 (41 to 138) | no difference from 128 |
| No padding (counters share a line) | 11.9 (8.9 to 62) | 15.5 (12 to 19) | about 7 to 9 times slower: padding matters |
| No cached opposing index | 36 (21 to 57) | 30 (17 to 58) | about 3 to 4 times slower: caching matters |
| seq_cst everywhere | 8.9 (6.9 to 52) | 8.4 (6.7 to 15) | about 12 to 13 times slower: acquire/release matters |
| Naive layout (no padding, no cache, seq_cst) | 6.1 (4.9 to 7.0) | 5.1 (4.4 to 6.1) | the worst case |

An earlier attempt at this ablation, run as sequential Google Benchmark cases, produced nonsense (the baseline 2 times slower than the production ring, padding appearing not to matter) because the host drifted between cases. That is why the comparison tool interleaves its variants. The sequential output is kept in docs/results/phase3/run1_gbench_sequential.txt for the record and should not be used.

## 6. Environment of every number above

    cpu: Intel Xeon (virtualized, model string reports only Intel(R) Xeon(R) Processor @ 2.10GHz)
    hypervisor: KVM, 4 vCPUs, 1 thread per core as seen by the guest, L1d 48 KiB, L2 2 MiB per vCPU
    kernel: Linux 6.18.44-fc-v114
    governor: not exposed (VM), TSC: constant_tsc, nonstop_tsc
    compiler: GCC 13.3.0, Release (-O3 -DNDEBUG), no -march=native
    kCacheLine: 128 unless stated, threads pinned with pthread_setaffinity_np to vCPU 1 and 2
    date: 2026-10-12

Complete output of bench/env.sh is in docs/results/phase3/environment.txt. Nothing was isolated: no isolcpus, no nohz_full, no frequency control (none is available in this sandbox), and other processes of this session were idle but present.

## 7. Limitations of these measurements

- This is a virtual machine. The guest cannot see whether its four vCPUs are on different physical cores, on SMT siblings, or move between them while it runs. The single-item ring throughput ranged from 30 to 184 million items per second across rounds on the same code, which looks like the producer and consumer vCPUs landing on different host placements. Interleaving makes comparisons fair; it does not remove the variance. Do not quote one ring number without its range.
- Tail latencies (p99 and above) are dominated by host scheduling and are not properties of the queues.
- Absolute numbers will differ on bare metal, on other CPUs and across NUMA nodes. The ordering of the designs (ring ahead of naive atomic and mutex, padding and caching and acquire/release each mattering) is the transferable result, and even that was measured on one machine.
- The latency benchmark measures a lightly loaded, busy-polling, pinned pair. It says nothing about a blocking consumer, about more than one producer, or about queues that are often full.
- The comparison queues are mine. A well tuned mutex queue or a different lock-free design (for example one that uses a futex when idle) could differ.

## 8. Reproducing

    cmake --preset bench && cmake --build --preset bench
    ./bench/env.sh                                  # record the environment first
    build/bench/bench/bench_false_sharing           # about 1 minute
    build/bench/bench/bench_interleaved             # about 4 minutes
    build/bench/bench/bench_queue_latency           # about 2 minutes
    build/bench/bench/bench_queues                  # about 8 minutes (Google Benchmark)

Environment variables: LOBKIT_BENCH_CPUS=a,b chooses the cores, LOBKIT_ROUNDS and LOBKIT_LAT_ITEMS and LOBKIT_PERIOD_NS shorten or retarget the runs. On a machine you control, run on isolated cores (isolcpus, nohz_full and rcu_nocbs on the two benchmark cores, performance governor, SMT siblings kept idle) and pin with LOBKIT_BENCH_CPUS. This was not possible in this sandbox. TODO: re-run all four on such a machine and add the results beside these, labelled with that machine.
