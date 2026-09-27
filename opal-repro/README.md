# Opal reproduction

A from-scratch reimplementation of **Opal**, the operation-aware hybrid lock from

> Vishal Gupta, Martin Sanchez Lopez, Victor Laforet, Jean-Pierre Lozi, Sanidhya Kashyap.
> *Operation-Aware Hybrid Locking for Modern In-Memory Indexes.*
> PVLDB 19(8): 1804–1817, 2026. [doi:10.14778/3811243.3811253](https://doi.org/10.14778/3811243.3811253)

Written only from the paper (Listings 1–3 and Sections 3–5), not from the authors'
artifact at <https://github.com/rs3lab/opal>. The point was to see whether the
described mechanism reproduces, and where the reported effects come from.

## What is here

| File | Contents |
| --- | --- |
| [include/opal.h](include/opal.h) | The Opal lock: 8-byte lock word, one queue node per thread, optimistic read lock, MCS-style write lock, function-pointer combining |
| [include/baseline_locks.h](include/baseline_locks.h) | `STDRW` (`std::shared_mutex`) and `MCSRW` (centralised reader counter + MCS writer queue) |
| [include/btree.h](include/btree.h) | B+ Tree with lock coupling, templated on the lock; Listing 3's default and function-pointer update paths |
| [src/bench.cpp](src/bench.cpp) | YCSB-style driver: Table 1 operation mixes, self-similar key distribution, throughput and p99.9 latency |

`OpalLock` is parameterised on the two knobs the paper ablates, so one
implementation covers three of the evaluated locks:

```cpp
OpalLock<true , false>  // ~ OptiQL:   optimistic reads + MCS writers, no batching
OpalLock<true , true >  //   Opal:     + function-pointer batching for updates
OpalLock<false, true >  //   Opal-NOR: batching, opportunistic reads disabled
```

The index picks its update path from `Lock::kUseFnPtr`, which is exactly the
paper's operation-awareness: lookups take the optimistic read lock, updates go
through `execute_op` so a combiner can batch them, and SMOs take the plain
MCS-style write lock.

## Build and run

Needs a C++20 compiler. On this machine (MSYS2 UCRT64 g++ 16.1):

```sh
make                       # or: g++ -std=c++20 -O3 -march=native -Iinclude src/bench.cpp -o bench -static
./bench --header           # print the CSV column names
./bench --lock opal --workload update-only --threads 16 --records 2000000 --secs 3
```

Flags: `--lock {stdrw,mcsrw,optiql,opal,opal-nor}`, `--workload <name from Table 1>`,
`--threads`, `--records`, `--skew` (self-similar factor; 0.5 is uniform),
`--secs`, `--no-pin`, `--csv <file>`, `--verify`, `--stress <rounds>`.

`--stress` rebuilds the tree under a concurrent load and checks the full
structure (key order, separator ranges, uniform leaf depth, every key
retrievable). Run it after touching any lock or tree code.

## Where the implementation departs from the paper

These are deliberate and they bound what the numbers mean.

**Lock word.** The paper uses 2 status bits, a 10-bit tail and a 52-bit version.
This version keeps the 8-byte word and the 10-bit tail but gives the version 32
bits, so that state and version can be read in one 64-bit load and the
sub-fields can still be CAS'd and swapped independently as the pseudocode does.
A 32-bit version can wrap; at the update rates measured here that takes minutes
of continuous writing to a single leaf, and no run is that long.

**Version bumps inside a batch.** Listing 2 calls an `execute_cs_via_fn_ptr`
helper it never defines. It must increment the version, otherwise a reader that
acquires while opportunistic reads are enabled, and validates after the combiner
re-enables them, can miss a write made earlier in the same batch. That reading is
what Section 3.3 requires, and it is what this implementation does.

**No NUMA policy.** Section 4 describes a dynamic queue-splitting policy that
batches within a socket before passing ownership across sockets. The test
machine is single-socket, so this is not implemented and none of the paper's
cross-socket results can be checked here.

**Inserts.** An insert descends optimistically and takes a plain write lock on
the leaf. If the leaf is full it restarts with a top-down write-coupled descent
that holds every ancestor which could split, rather than the paper's bottom-up
acquisition. Same lock order, same guarantees, simpler to get right; it does
hold ancestor locks slightly longer.

**No deletes and no memory reclamation.** Nodes are never freed, so an optimistic
reader can always dereference a child pointer it read from a node whose version
later turns out to be stale. Table 1 has no delete workload, so the paper's setup
implies the same assumption.

**Node size.** 256-byte nodes with 8-byte keys and values, matching Section 5:
15 slots per leaf and 15 separators per inner node.

## Test machine

Intel Core i5-12500H (12th gen, hybrid: 4 P-cores with SMT + 8 E-cores, 16
logical), single socket, Windows 11, MSYS2 UCRT64 g++ 16.1, `-O3 -march=native`.

This is a much smaller and less uniform machine than the paper's dual-socket
96-core Sapphire Rapids. Two consequences worth stating up front: there is no
NUMA effect to measure, and past 8 threads the scheduler is mixing P-cores and
E-cores, so a throughput curve that bends at 8 threads is partly a property of
the CPU, not of the lock. Threads are pinned to distinct physical cores before
doubling up on SMT siblings.

## Results on this machine

Full median-of-3 tables: [results/summary.md](results/summary.md) (regenerate with
`./run_all.sh && python summarize.py`). Records = 2M, skew 0.1, 2 s per run.

| Claim in the paper | Here (16 threads max) | Reproduced? |
| --- | --- | --- |
| Optimistic locks far ahead of pessimistic on lookups | lookup-only, 16T: Opal 92.7, OptiQL 91.3, MCSRW 5.6, STDRW 3.4 Mops/s | Yes (17–27×; paper reports 150× at 96 cores) |
| Opal beats OptiQL on update-heavy mixes | update-only 16T: 9.61 vs 8.07 Mops/s (1.19×); update-heavy 1.07×; balanced 1.07× | Partly: direction yes, magnitude far below the paper's 2.5–3.2× within a socket |
| Opal lowers tail latency | update-only 16T p99.9: 9.9 vs 44.9 µs (−78%) | Yes (paper: −80%), but at 8T Opal's tail is 2.6× *worse* |
| Batching grows with contention | avg batch 1.4 (4T), 8.7 (8T), 527 (12T), 4,110 (16T) | Yes; Opal is 0.84× OptiQL at 4T, before batching pays off |
| Opportunistic reads matter (Opal vs Opal-NOR) | balanced 16T: 12.81 vs 8.46 Mops/s; 6.8× fewer reader retries | Yes in direction (paper: 3.26× and 107×) |
| Opal reduces reader retries vs OptiQL | balanced 16T: Opal 41.8M vs OptiQL 13.3M retries | **No, reversed** (3.1× more) |
| Batching fades as skew drops | avg batch 7,075 at skew 0.1 → 2.0 at 0.3; speedup → ~1× | Yes (paper: 15,384 → 1.77) |
| Opal resists oversubscription | update-heavy, 64 threads: Opal 3.34 vs OptiQL 5.70 Mops/s | **No** (0.6–0.85× OptiQL at 2–4×; OptiQL does not collapse here) |

Caveats: 16 hardware threads on a hybrid P/E-core laptop versus 96 cores in the
paper; Windows scheduler, not Linux; single socket, so no NUMA results.
