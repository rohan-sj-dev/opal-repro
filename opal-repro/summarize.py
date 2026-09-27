"""Median-of-runs tables from results/*.csv (stdlib only)."""
import csv
import statistics
import sys
from collections import defaultdict

LOCKS = ["stdrw", "mcsrw", "optiql", "opal", "opal-nor"]


def load(path):
    groups = defaultdict(list)
    with open(path) as f:
        for r in csv.DictReader(f):
            key = (r["lock"], r["workload"], int(r["threads"]), float(r["skew"]))
            groups[key].append(r)
    med = {}
    for k, rows in groups.items():
        med[k] = {
            "mops": statistics.median(float(r["mops"]) for r in rows),
            "p999": statistics.median(float(r["p999_us"]) for r in rows),
            "rr": statistics.median(float(r["read_retries"]) for r in rows),
            "batch": statistics.median(float(r["avg_batch"]) for r in rows),
            "n": len(rows),
        }
    return med


def table(med, workloads, xs, xname, xkey, locks, metric="mops"):
    for w in workloads:
        print(f"\n### {w}  ({metric}, median of runs)")
        print(f"| {xname} | " + " | ".join(locks) + " | Opal/OptiQL |")
        print("|---" * (len(locks) + 2) + "|")
        for x in xs:
            row, vals = [], {}
            for l in locks:
                k = xkey(l, w, x)
                v = med.get(k, {}).get(metric)
                vals[l] = v
                row.append("-" if v is None else f"{v:.2f}")
            ratio = ""
            if vals.get("opal") and vals.get("optiql"):
                ratio = f"{vals['opal'] / vals['optiql']:.2f}x"
            print(f"| {x} | " + " | ".join(row) + f" | {ratio} |")


if __name__ == "__main__":
    part = sys.argv[1] if len(sys.argv) > 1 else "all"
    if part in ("all", "threads"):
        m = load("results/threads.csv")
        ws = ["lookup-only", "lookup-heavy", "balanced", "update-heavy", "update-only"]
        ts = [1, 2, 4, 8, 12, 16]
        table(m, ws, ts, "threads", lambda l, w, t: (l, w, t, 0.1), LOCKS)
        table(m, ["update-heavy", "update-only"], ts, "threads",
              lambda l, w, t: (l, w, t, 0.1), LOCKS, "p999")
        print("\n### reader retries / avg batch (balanced)")
        for t in ts:
            o, p, n = (m.get((l, "balanced", t, 0.1), {}) for l in ("optiql", "opal", "opal-nor"))
            print(f"t={t}: retries optiql={o.get('rr')} opal={p.get('rr')} opal-nor={n.get('rr')}"
                  f"  batch opal={p.get('batch')}")
    if part in ("all", "oversub"):
        m = load("results/oversub.csv")
        table(m, ["balanced", "update-heavy"], [16, 32, 48, 64], "threads",
              lambda l, w, t: (l, w, t, 0.1), ["mcsrw", "optiql", "opal"])
    if part in ("all", "skew"):
        m = load("results/skew.csv")
        ks = [0.05, 0.1, 0.2, 0.3, 0.4, 0.5]
        table(m, ["update-only"], ks, "skew",
              lambda l, w, k: (l, w, 16, k), ["mcsrw", "optiql", "opal"])
        for k in ks:
            print(f"skew={k}: opal avg batch = {m.get(('opal','update-only',16,k),{}).get('batch')}")
