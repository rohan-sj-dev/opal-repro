// Benchmark driver for the Opal reproduction.
//
// Mirrors the setup of Section 5 of the paper: a B+ Tree over 8-byte key/value
// pairs, YCSB-style operation mixes (Table 1), and a self-similar key
// distribution with a configurable skew factor.
#include "baseline_locks.h"
#include "btree.h"
#include "opal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <malloc.h>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lk {
thread_local int t_tid = 0;
thread_local uint64_t t_batched = 0;
thread_local uint64_t t_batches = 0;
QNode g_qnodes[kMaxThreads];
MCSNode g_mcs_nodes[kMaxThreads * kMaxNesting];
thread_local uint64_t t_mcs_inuse = 0;
void register_thread(int id) { t_tid = id; }
} // namespace lk

namespace idx {
thread_local uint64_t t_read_retries = 0;
thread_local uint64_t t_write_retries = 0;
} // namespace idx

// ---------------------------------------------------------------- utilities

struct Rng {  // xorshift128+
  uint64_t s0, s1;
  explicit Rng(uint64_t seed) {
    s0 = seed * 0x9E3779B97F4A7C15ull + 1;
    s1 = seed ^ 0xD1B54A32D192ED03ull;
    for (int i = 0; i < 8; ++i) next();
  }
  uint64_t next() {
    uint64_t x = s0, y = s1;
    s0 = y;
    x ^= x << 23;
    s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return s1 + y;
  }
  double unit() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
};

// Self-similar distribution as used by PiBench and by the paper (Section 5):
// with skew h, a fraction 1-h of accesses fall in the first h of the key space,
// recursively. h = 0.5 is uniform.
static inline uint64_t self_similar(double u, double skew, uint64_t n) {
  if (skew >= 0.5) return static_cast<uint64_t>(u * n);
  const double e = std::log(skew) / std::log(1.0 - skew);
  uint64_t k = static_cast<uint64_t>(n * std::pow(u, e));
  return k < n ? k : n - 1;
}

static double g_tsc_ghz = 1.0;
static void calibrate_tsc() {
  auto t0 = std::chrono::steady_clock::now();
  uint64_t c0 = rdtsc();
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  uint64_t c1 = rdtsc();
  auto t1 = std::chrono::steady_clock::now();
  double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
  g_tsc_ghz = (c1 - c0) / ns;
}

static void pin_thread(int i, int ncpu) {
#ifdef _WIN32
  // Spread over distinct physical cores before doubling up on SMT siblings.
  int slot = i % ncpu;
  int cpu = (slot < (ncpu + 1) / 2) ? slot * 2 : (slot - (ncpu + 1) / 2) * 2 + 1;
  SetThreadAffinityMask(GetCurrentThread(), 1ull << cpu);
#endif
}

// ----------------------------------------------------------------- workloads

struct Mix {
  const char* name;
  int lookup, update, insert;  // percentages
};
static const Mix kMixes[] = {
    {"lookup-only", 100, 0, 0},
    {"lookup-heavy", 95, 5, 0},
    {"balanced", 50, 50, 0},
    {"update-heavy", 20, 80, 0},
    {"update-only", 0, 100, 0},
    {"insert-only", 0, 0, 100},
    {"insert-lookup-heavy", 95, 0, 5},
    {"insert-lookup-balanced", 50, 0, 50},
    {"insert-update-heavy", 0, 80, 20},
    {"insert-update-balanced", 0, 50, 50},
};

// Repeatedly builds a tree with a concurrent load and checks its structure.
// Used to shake out races in the insert/SMO path before trusting any timing.
template <class Lock>
int stress(int rounds, int threads, uint64_t records);

struct Config {
  std::string lock = "opal";
  std::string workload = "update-only";
  int threads = 16;
  uint64_t records = 2000000;
  double skew = 0.1;
  double secs = 2.0;
  bool pin = true;
  bool verify = false;
  int load_threads = 8;
  int stress_rounds = 0;
  const char* csv = nullptr;
};

struct Result {
  double mops = 0, p999_us = 0;
  uint64_t ops = 0, read_retries = 0, write_retries = 0;
  uint64_t batched = 0, batches = 0;
};

// Insert records 0..N-1 from `nthreads` threads, each over a disjoint range
// walked in a multiplicative (and therefore duplicate-free) order so the tree
// does not degenerate into a sequential append.
template <class Tree>
static void load_tree(Tree* tree, uint64_t records, int nthreads) {
  std::vector<std::thread> ts;
  for (int t = 0; t < nthreads; ++t) {
    ts.emplace_back([=] {
      lk::register_thread(t);
      uint64_t lo = records * t / nthreads;
      uint64_t hi = records * (t + 1) / nthreads;
      for (uint64_t i = lo; i < hi; ++i) {
        uint64_t k = lo + ((i - lo) * 2654435761ull) % (hi - lo);
        tree->insert(k, k + 1);
      }
    });
  }
  for (auto& th : ts) th.join();
}

template <class Lock>
int stress(int rounds, int threads, uint64_t records) {
  using Tree = idx::BTree<Lock>;
  int bad = 0;
  for (int r = 0; r < rounds; ++r) {
    Tree* tree = new Tree();
    load_tree(tree, records, threads);
    std::string err = tree->validate();
    size_t n = tree->count_keys();
    uint64_t v, missing = 0;
    for (uint64_t k = 0; k < records; ++k)
      if (!tree->lookup(k, &v) || v != k + 1) ++missing;
    if (!err.empty() || n != records || missing) {
      ++bad;
      std::printf("round %d FAIL structure=%s keys=%zu missing=%llu\n", r,
                  err.c_str(), n, (unsigned long long)missing);
    }
    std::fflush(stdout);
  }
  std::printf("stress: %d/%d rounds failed\n", bad, rounds);
  return bad;
}

template <class Lock>
Result run(const Config& cfg, const Mix& mix) {
  using Tree = idx::BTree<Lock>;
  Tree* tree = new Tree();

  const int ncpu = std::max(1u, std::thread::hardware_concurrency());
  load_tree(tree, cfg.records, cfg.load_threads);

  if (cfg.verify) {
    std::string err = tree->validate();
    if (!err.empty()) std::fprintf(stderr, "[verify] STRUCTURE: %s\n", err.c_str());
    size_t n = tree->count_keys();
    std::fprintf(stderr, "[verify] loaded keys=%zu expected=%llu height=%d\n", n,
                 (unsigned long long)cfg.records, tree->height());
    uint64_t v, missing = 0;
    for (uint64_t k = 0; k < cfg.records; ++k)
      if (!tree->lookup(k, &v) || v != k + 1) ++missing;
    std::fprintf(stderr, "[verify] lookup mismatches=%llu\n",
                 (unsigned long long)missing);
  }

  // ---- run phase
  std::atomic<bool> go{false}, stop{false};
  std::vector<uint64_t> ops(cfg.threads, 0), rr(cfg.threads, 0),
      wr(cfg.threads, 0), bd(cfg.threads, 0), bt(cfg.threads, 0);
  std::vector<std::vector<uint32_t>> lat(cfg.threads);
  std::vector<std::thread> ts;

  for (int t = 0; t < cfg.threads; ++t) {
    ts.emplace_back([&, t] {
      lk::register_thread(t);
      if (cfg.pin) pin_thread(t, ncpu);
      Rng rng(0x1234567 + t * 7919);
      idx::t_read_retries = idx::t_write_retries = 0;
      lk::t_batched = lk::t_batches = 0;
      std::vector<uint32_t>& L = lat[t];
      L.reserve(1 << 18);
      uint64_t next_insert = cfg.records + t;
      uint64_t n = 0, v;
      while (!go.load(std::memory_order_acquire)) cpu_pause();
      while (!stop.load(std::memory_order_relaxed)) {
        for (int b = 0; b < 64; ++b) {
          int r = static_cast<int>(rng.next() % 100);
          uint64_t key = self_similar(rng.unit(), cfg.skew, cfg.records);
          bool sample = ((n & 15) == 0);
          uint64_t c0 = sample ? rdtsc() : 0;
          if (r < mix.lookup) {
            tree->lookup(key, &v);
          } else if (r < mix.lookup + mix.update) {
            tree->update(key, key + 2);
          } else {
            tree->insert(next_insert, next_insert + 1);
            next_insert += cfg.threads;
          }
          if (sample) {
            uint64_t d = rdtsc() - c0;
            L.push_back(static_cast<uint32_t>(d > 0xFFFFFFFFull ? 0xFFFFFFFFull : d));
          }
          ++n;
        }
      }
      ops[t] = n;
      rr[t] = idx::t_read_retries;
      wr[t] = idx::t_write_retries;
      bd[t] = lk::t_batched;
      bt[t] = lk::t_batches;
    });
  }

  auto start = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::duration<double>(cfg.secs));
  stop.store(true, std::memory_order_relaxed);
  for (auto& th : ts) th.join();
  double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();

  Result res;
  std::vector<uint32_t> all;
  for (int t = 0; t < cfg.threads; ++t) {
    res.ops += ops[t];
    res.read_retries += rr[t];
    res.write_retries += wr[t];
    res.batched += bd[t];
    res.batches += bt[t];
    all.insert(all.end(), lat[t].begin(), lat[t].end());
  }
  res.mops = res.ops / elapsed / 1e6;
  if (!all.empty()) {
    size_t i = static_cast<size_t>(all.size() * 0.999);
    if (i >= all.size()) i = all.size() - 1;
    std::nth_element(all.begin(), all.begin() + i, all.end());
    res.p999_us = all[i] / (g_tsc_ghz * 1000.0);
  }
  return res;
}

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() { return std::string(argv[++i]); };
    if (a == "--lock") cfg.lock = next();
    else if (a == "--workload") cfg.workload = next();
    else if (a == "--threads") cfg.threads = std::stoi(next());
    else if (a == "--records") cfg.records = std::stoull(next());
    else if (a == "--skew") cfg.skew = std::stod(next());
    else if (a == "--secs") cfg.secs = std::stod(next());
    else if (a == "--no-pin") cfg.pin = false;
    else if (a == "--verify") cfg.verify = true;
    else if (a == "--load-threads") cfg.load_threads = std::stoi(next());
    else if (a == "--stress") cfg.stress_rounds = std::stoi(next());
    else if (a == "--csv") cfg.csv = argv[++i];
    else if (a == "--header") {
      std::printf("lock,workload,threads,records,skew,mops,p999_us,read_retries,write_retries,batched_cs,batches,avg_batch\n");
      return 0;
    } else {
      std::fprintf(stderr, "unknown flag %s\n", a.c_str());
      return 1;
    }
  }

  const Mix* mix = nullptr;
  for (const Mix& m : kMixes)
    if (cfg.workload == m.name) mix = &m;
  if (!mix) { std::fprintf(stderr, "unknown workload %s\n", cfg.workload.c_str()); return 1; }
  if (cfg.threads > lk::kMaxThreads) { std::fprintf(stderr, "too many threads\n"); return 1; }

  calibrate_tsc();

  if (cfg.stress_rounds > 0) {
    if (cfg.lock == "stdrw") return stress<lk::StdRWLock>(cfg.stress_rounds, cfg.threads, cfg.records);
    if (cfg.lock == "mcsrw") return stress<lk::MCSRWLock>(cfg.stress_rounds, cfg.threads, cfg.records);
    if (cfg.lock == "optiql") return stress<lk::OptiQLLock>(cfg.stress_rounds, cfg.threads, cfg.records);
    if (cfg.lock == "opal") return stress<lk::OpalHybrid>(cfg.stress_rounds, cfg.threads, cfg.records);
    if (cfg.lock == "opal-nor") return stress<lk::OpalNoR>(cfg.stress_rounds, cfg.threads, cfg.records);
    std::fprintf(stderr, "unknown lock %s\n", cfg.lock.c_str());
    return 1;
  }

  Result r;
  if (cfg.lock == "stdrw") r = run<lk::StdRWLock>(cfg, *mix);
  else if (cfg.lock == "mcsrw") r = run<lk::MCSRWLock>(cfg, *mix);
  else if (cfg.lock == "optiql") r = run<lk::OptiQLLock>(cfg, *mix);
  else if (cfg.lock == "opal") r = run<lk::OpalHybrid>(cfg, *mix);
  else if (cfg.lock == "opal-nor") r = run<lk::OpalNoR>(cfg, *mix);
  else { std::fprintf(stderr, "unknown lock %s\n", cfg.lock.c_str()); return 1; }

  double avg_batch = r.batches ? (double)r.batched / r.batches : 0.0;
  char line[512];
  std::snprintf(line, sizeof(line),
                "%s,%s,%d,%llu,%.2f,%.3f,%.2f,%llu,%llu,%llu,%llu,%.1f\n",
                cfg.lock.c_str(), cfg.workload.c_str(), cfg.threads,
                (unsigned long long)cfg.records, cfg.skew, r.mops, r.p999_us,
                (unsigned long long)r.read_retries,
                (unsigned long long)r.write_retries,
                (unsigned long long)r.batched, (unsigned long long)r.batches,
                avg_batch);
  std::fputs(line, stdout);
  if (cfg.csv) {
    FILE* f = std::fopen(cfg.csv, "a");
    if (f) { std::fputs(line, f); std::fclose(f); }
  }
  return 0;
}
