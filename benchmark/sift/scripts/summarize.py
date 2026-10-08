#!/usr/bin/env python3
"""
summarize.py -- Markdown and CSV tables from the rows the run-*.sh scripts append.

    summarize.py [--csv DIR] SUBCOMMAND FILE.jsonl [FILE.jsonl ...]

    quality    Q1: final sizes against CUDD's, and whether more workers reach
               exactly the same order; threshold sweep and CUDD's own caps
               when those rows are present
    timing     Q2: sift time per variant as median (min-max), scaling against
               the same build at its lowest worker count, load average range
    soundness  Q3: sound/valid per variant, the downstream operation phase,
               and cross-tool agreement of the evaluation hashes
    queens     queens boards: gain, sift time, swaps
    seeds      many random orders: a per-seed table and the distribution of
               the Sylvan/CUDD final-size ratio
    backends   the equality pass across nodes tables, then their timings
    refusals   every Sylvan row whose sift had a swap refused
    all        every section the files have rows for

Markdown goes to stdout. With --csv DIR every table is also written to
DIR/<table>.csv, with medians, minima and maxima in separate columns.

NODE COUNTS. nodes_before and nodes_after are the shared node count of the
output roots on both sides -- mtbdd_shared_node_count and Cudd_SharingSize --
which agree exactly on every unsifted build, so they are compared directly.
Sylvan's own sift_initial/sift_final count the whole reachable forest and run
a little higher; they are not used for any ratio.

Rows written before the scripts recorded "exp" and "build" (the
sift-eval-20260816 batches) are accepted: a row without "exp" is used by any
subcommand, and a row without "build" belongs to a build named after its tool.
"""

import argparse
import csv
import json
import math
import os
import statistics
import sys
from collections import OrderedDict, defaultdict


# --------------------------------------------------------------- rows

def load(paths):
    rows = []
    for path in paths:
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError as e:
                    print(f"{path}:{n}: skipped, not JSON ({e})", file=sys.stderr)
    return rows


def of_exp(rows, *exps):
    """Rows of these experiments, plus rows that do not say."""
    return [r for r in rows if r.get("exp") is None or r.get("exp") in exps]


def ok(rows):
    return [r for r in rows if not r.get("failed")]


def tool(r):
    return r.get("tool", "?")


def build(r):
    return r.get("build") or tool(r)


def cfg(r):
    return r.get("cfg") or f"{build(r)}-p{r.get('workers', '?')}"


def is_sylvan(r):
    return tool(r) == "sylvan"


def ordered(values):
    return list(OrderedDict.fromkeys(values))


def group(rows, key):
    g = OrderedDict()
    for r in rows:
        g.setdefault(key(r), []).append(r)
    return g


def median(vals):
    return statistics.median(vals) if vals else None


def geomean(vals):
    vals = [v for v in vals if v and v > 0]
    return math.exp(sum(math.log(v) for v in vals) / len(vals)) if vals else None


def failures(rows):
    bad = [r for r in rows if r.get("failed")]
    if not bad:
        return
    print(f"*{len(bad)} failed or timed-out runs are not in the tables:*\n")
    for r in bad:
        print(f"- {r.get('case', '?')} {cfg(r)} rep {r.get('rep', '?')}: rc={r.get('rc')}")
    print()


# --------------------------------------------------------------- cells

class F:
    """A float shown to <nd> decimals."""
    def __init__(self, v, nd=3, suffix=""):
        self.v, self.nd, self.suffix = v, nd, suffix

    def md(self):
        return "-" if self.v is None else f"{self.v:.{self.nd}f}{self.suffix}"

    def csv(self):
        return [] if self.v is None else [self.v]


class MMM:
    """median (min-max) of a sample."""
    def __init__(self, vals, nd=4):
        vals = [v for v in vals if v is not None]
        self.n = len(vals)
        self.med = median(vals)
        self.lo = min(vals) if vals else None
        self.hi = max(vals) if vals else None
        self.nd = nd

    def md(self):
        if self.med is None:
            return "-"
        if self.n == 1:
            return f"{self.med:.{self.nd}f}"
        return f"{self.med:.{self.nd}f} ({self.lo:.{self.nd}f}–{self.hi:.{self.nd}f})"


def md_cell(c):
    if isinstance(c, (F, MMM)):
        return c.md()
    if c is None:
        return "-"
    if isinstance(c, float):
        return f"{c:.4g}"
    return str(c)


class Table:
    def __init__(self, name, title, headers, notes=()):
        self.name, self.title, self.headers, self.notes = name, title, headers, notes
        self.rows = []

    def add(self, *cells):
        self.rows.append(list(cells))

    def emit(self, csv_dir):
        if not self.rows:
            return
        print(f"### {self.title}\n")
        for n in self.notes:
            print(n)
        if self.notes:
            print()
        print("| " + " | ".join(self.headers) + " |")
        print("|" + "|".join("---" for _ in self.headers) + "|")
        for r in self.rows:
            print("| " + " | ".join(md_cell(c).replace("|", "\\|") for c in r) + " |")
        print()
        if csv_dir:
            self.write_csv(csv_dir)

    def write_csv(self, csv_dir):
        os.makedirs(csv_dir, exist_ok=True)
        heads = []
        mmm_cols = set()
        for i, h in enumerate(self.headers):
            if any(isinstance(r[i], MMM) for r in self.rows):
                mmm_cols.add(i)
                heads += [f"{h} median", f"{h} min", f"{h} max", f"{h} n"]
            else:
                heads.append(h)
        with open(os.path.join(csv_dir, f"{self.name}.csv"), "w", newline="",
                  encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(heads)
            for r in self.rows:
                out = []
                for i, c in enumerate(r):
                    if i in mmm_cols:
                        out += ([c.med, c.lo, c.hi, c.n] if isinstance(c, MMM)
                                else [c, "", "", ""])
                    elif isinstance(c, F):
                        out.append("" if c.v is None else c.v)
                    else:
                        out.append("" if c is None else c)
                w.writerow(out)


def ratio(a, b):
    return a / b if a is not None and b else None


# --------------------------------------------------------------- quality

def cudd_reference(rs):
    """The CUDD row with raised caps among <rs>, if any."""
    for r in rs:
        if tool(r) == "cudd" and r.get("unlimited", 1) == 1:
            return r
    return None


def quality(rows, csv_dir):
    rows = of_exp(rows, "quality")
    good = ok(rows)
    if not good:
        return
    print("## Q1 quality: final shared node count of the output roots\n")
    failures(rows)
    by_case = group(good, lambda r: r["case"])

    # Headline: every Sylvan build at threshold 0 and its lowest worker count.
    heads = []
    for r in good:
        if is_sylvan(r) and r.get("threshold", 0) == 0:
            heads.append(build(r))
    builds = ordered(heads)
    t = Table("quality_headline", "Final size, Sylvan against CUDD",
              ["case", "initial"] + builds + ["cudd"]
              + [f"{b}/cudd" for b in builds]
              + [f"{b} swaps" for b in builds] + ["cudd swaps"],
              ["nodes_threshold 0, lowest worker count. A ratio below 1 means "
               "Sylvan found the smaller order."])
    for case, rs in by_case.items():
        cu = cudd_reference(rs)
        finals, swaps = [], []
        for b in builds:
            cand = [r for r in rs if is_sylvan(r) and build(r) == b
                    and r.get("threshold", 0) == 0]
            if cand:
                low = min(r.get("workers", 1) for r in cand)
                r = next(r for r in cand if r.get("workers", 1) == low)
                finals.append(r["nodes_after"])
                swaps.append(r.get("swaps"))
            else:
                finals.append(None)
                swaps.append(None)
        t.add(case, rs[0].get("nodes_before"), *finals,
              cu["nodes_after"] if cu else None,
              *[F(ratio(f, cu["nodes_after"] if cu else None)) for f in finals],
              *swaps, int(cu["swaps"]) if cu else None)
    t.emit(csv_dir)

    # Detail: every variant.
    t = Table("quality", "Every variant",
              ["case", "variant", "workers", "threshold", "initial", "final",
               "vs cudd", "swaps", "refusals", "vars sifted", "sift s",
               "same as lowest P"],
              ["\"same as lowest P\": final size and final order identical to "
               "the same build and threshold at its lowest worker count."])
    for case, rs in by_case.items():
        cu = cudd_reference(rs)
        for c, vs in group(rs, cfg).items():
            r = vs[0]
            finals = {v["nodes_after"] for v in vs}
            final = r["nodes_after"] if len(finals) == 1 else \
                f"{min(finals)}..{max(finals)}"
            same = "-"
            if is_sylvan(r):
                peers = [p for p in rs if is_sylvan(p) and build(p) == build(r)
                         and p.get("threshold") == r.get("threshold")]
                low = min(p.get("workers", 1) for p in peers)
                if r.get("workers", 1) != low:
                    base = next(p for p in peers if p.get("workers", 1) == low)
                    if r.get("order_after") is None or base.get("order_after") is None:
                        same = "yes" if r["nodes_after"] == base["nodes_after"] else "NO"
                        same += " (size only)"
                    else:
                        same = "yes" if (r["nodes_after"] == base["nodes_after"] and
                                         r["order_after"] == base["order_after"]) else "NO"
            t.add(case, c, r.get("workers"),
                  r.get("threshold", "-") if is_sylvan(r) else "-",
                  r.get("nodes_before"), final,
                  F(ratio(r["nodes_after"], cu["nodes_after"] if cu else None)),
                  int(r["swaps"]) if r.get("swaps") is not None else None,
                  r.get("refusals", "-"), r.get("vars_sifted", "-"),
                  MMM([v.get("sift_s") for v in vs]), same)
    t.emit(csv_dir)

    # Threshold sweep, when there is one.
    if any(is_sylvan(r) and r.get("threshold", 0) != 0 for r in good):
        t = Table("quality_threshold", "nodes_threshold sweep",
                  ["case", "build", "threshold", "final", "vs t=0",
                   "vars sifted", "swaps", "sift s"],
                  ["Lowest worker count. Threshold 0 is the library default and "
                   "the like-for-like setting against CUDD."])
        for case, rs in by_case.items():
            syl = [r for r in rs if is_sylvan(r)]
            for b in ordered(build(r) for r in syl):
                mine = [r for r in syl if build(r) == b]
                low = min(r.get("workers", 1) for r in mine)
                mine = [r for r in mine if r.get("workers", 1) == low]
                base = next((r for r in mine if r.get("threshold", 0) == 0), None)
                for th in sorted({r.get("threshold", 0) for r in mine}):
                    vs = [r for r in mine if r.get("threshold", 0) == th]
                    r = vs[0]
                    t.add(case, b, th, r["nodes_after"],
                          F(ratio(r["nodes_after"], base["nodes_after"] if base else None)),
                          r.get("vars_sifted"), r.get("swaps"),
                          MMM([v["sift_s"] for v in vs]))
        t.emit(csv_dir)

    # CUDD's own caps against the raised ones, when both were run.
    if any(tool(r) == "cudd" and r.get("unlimited", 1) == 0 for r in good):
        t = Table("quality_cudd_caps", "CUDD: raised caps against its own defaults",
                  ["case", "unlimited", "defaults", "verdict", "swaps unlimited",
                   "swaps defaults"],
                  ["The main runs raise siftMaxVar/siftMaxSwap to INT_MAX to match "
                   "Sylvan's unlimited setting; equal columns mean that choice "
                   "changed nothing."])
        for case, rs in by_case.items():
            u = cudd_reference(rs)
            d = next((r for r in rs if tool(r) == "cudd" and r.get("unlimited") == 0), None)
            if u and d:
                t.add(case, u["nodes_after"], d["nodes_after"],
                      "same" if u["nodes_after"] == d["nodes_after"] else "DIFFER",
                      int(u["swaps"]), int(d["swaps"]))
        t.emit(csv_dir)


# --------------------------------------------------------------- timing

def timing(rows, csv_dir, exp="timing", title="Q2 timing: sift wall time (s)"):
    rows = of_exp(rows, exp)
    good = ok(rows)
    if not good:
        return
    print(f"## {title}\n")
    failures(rows)
    by = group(good, lambda r: (r["case"], cfg(r)))

    def base_median(case, b):
        """Median sift time of build b on case at its lowest worker count."""
        mine = [r for r in good if r["case"] == case and is_sylvan(r) and build(r) == b]
        if not mine:
            return None
        low = min(r.get("workers", 1) for r in mine)
        return median([r["sift_s"] for r in mine if r.get("workers", 1) == low])

    def cudd_median(case):
        cu = [r for r in good if r["case"] == case and tool(r) == "cudd"]
        return median([r["sift_s"] for r in cu])

    t = Table(exp, "Per variant",
              ["case", "variant", "workers", "n", "sift s", "vs lowest P",
               "vs cudd", "build s", "swaps", "us/swap", "load1"],
              ["Medians with (min–max) over the repetitions. \"vs lowest P\" is "
               "the speedup over the same build at its lowest worker count; "
               "\"vs cudd\" is CUDD's median over this one, so above 1 means "
               "faster than CUDD. load1 is the 1-minute load average before "
               "each run."])
    for (case, c), rs in by.items():
        r0 = rs[0]
        sift = MMM([r["sift_s"] for r in rs])
        swaps = median([r["swaps"] for r in rs if r.get("swaps") is not None])
        base = base_median(case, build(r0)) if is_sylvan(r0) else None
        cu = cudd_median(case)
        load = [r["load1"] for r in rs if r.get("load1") is not None]
        t.add(case, c, r0.get("workers"), len(rs), sift,
              F(ratio(base, sift.med), 2, "x") if is_sylvan(r0) else None,
              F(ratio(cu, sift.med), 2, "x") if cu is not None else None,
              MMM([r.get("build_s") for r in rs]),
              int(swaps) if swaps is not None else None,
              F(1e6 * sift.med / swaps if swaps else None, 1),
              MMM(load, 2) if load else None)
    t.emit(csv_dir)

    # Scaling: one row per (case, build), one column per worker count.
    syl = [r for r in good if is_sylvan(r)]
    ws = sorted({r.get("workers", 1) for r in syl})
    if len(ws) > 1:
        t = Table(f"{exp}_scaling", "Scaling with workers",
                  ["case", "build"] + [f"P{w}" for w in ws],
                  ["Median sift time (s), and in parentheses the speedup over "
                   "the lowest worker count."])
        for (case, b), rs in group(syl, lambda r: (r["case"], build(r))).items():
            meds = {w: median([r["sift_s"] for r in rs if r.get("workers", 1) == w])
                    for w in ws}
            low = next((meds[w] for w in ws if meds[w] is not None), None)
            t.add(case, b, *[f"{meds[w]:.3f} ({low / meds[w]:.2f}x)"
                             if meds[w] else "-" for w in ws])
        t.emit(csv_dir)


# --------------------------------------------------------------- soundness

def soundness(rows, csv_dir):
    rows = of_exp(rows, "soundness")
    good = ok(rows)
    if not good:
        return
    print("## Q3 soundness and the downstream payoff\n")
    failures(rows)
    t = Table("soundness", "Per variant",
              ["case", "variant", "n", "nodes before", "nodes after",
               "ops before s", "ops after s", "ops speedup", "sound", "valid"],
              ["sound: the random-vector evaluation and the model counts agree "
               "before and after. valid: mtbdd_is_valid (Sylvan) or "
               "Cudd_DebugCheck (CUDD). ops: every pair of outputs conjoined and "
               "exclusive-ored, then model-counted."])
    bad = []
    for (case, c), rs in group(good, lambda r: (r["case"], cfg(r))).items():
        before = median([r["ops_before_s"] for r in rs if r.get("ops_before_s", -1) >= 0])
        after = median([r["ops_after_s"] for r in rs if r.get("ops_after_s", -1) >= 0])
        sound = {r.get("sound") for r in rs}
        valid = {r.get("valid") for r in rs}
        t.add(case, c, len(rs), rs[0].get("nodes_before"), rs[0].get("nodes_after"),
              F(before, 4), F(after, 4), F(ratio(before, after), 2, "x"),
              "OK" if sound == {1} else f"BAD {sorted(sound, key=str)}",
              "OK" if valid == {1} else f"BAD {sorted(valid, key=str)}")
        if sound != {1} or valid != {1}:
            bad.append(f"{case} {c}")
    t.emit(csv_dir)
    if bad:
        print(f"**SOUNDNESS FAILURES: {', '.join(bad)}**\n")
    hashes(good, csv_dir, "soundness_hashes")


def hashes(good, csv_dir, name):
    t = Table(name, "Cross-tool evaluation hashes",
              ["case", "verdict", "variants", "hashes"],
              ["Every driver hashes the same evaluation of every output on the "
               "same vectors; one hash per case means every variant built the "
               "same functions."])
    for case, rs in group(good, lambda r: r["case"]).items():
        hs = [(cfg(r), r.get("eval_hash")) for r in rs
              if r.get("eval_hash") not in (None, "", "0")]
        if not hs:
            continue
        vals = ordered(h for _, h in hs)
        t.add(case, "AGREE" if len(vals) == 1 else "DISAGREE",
              len(ordered(c for c, _ in hs)), " ".join(vals))
    t.emit(csv_dir)


# --------------------------------------------------------------- queens

def queens(rows, csv_dir):
    rows = of_exp(rows, "queens")
    good = [r for r in ok(rows) if str(r.get("case", "")).startswith("queens")]
    if not good:
        return
    print("## Queens\n")
    failures(rows)
    t = Table("queens", "Sifting a good natural order",
              ["case", "variant", "n", "initial", "final", "gain", "sift s",
               "build s", "swaps", "refusals"])
    for (case, c), rs in group(good, lambda r: (r["case"], cfg(r))).items():
        nb, na = rs[0]["nodes_before"], rs[0]["nodes_after"]
        t.add(case, c, len(rs), nb, na,
              F(100 * (1 - na / nb) if nb else None, 1, "%"),
              MMM([r["sift_s"] for r in rs]), MMM([r["build_s"] for r in rs], 2),
              int(median([r["swaps"] for r in rs])), rs[0].get("refusals", "-"))
    t.emit(csv_dir)


# --------------------------------------------------------------- seeds

def seeds(rows, csv_dir):
    rows = of_exp(rows, "seeds")
    good = ok(rows)
    if not good:
        return
    print("## Random starting orders\n")
    failures(rows)

    def seed(r):
        return r.get("order_seed", r.get("seed"))

    cudd = {seed(r): r for r in good if tool(r) == "cudd"}
    for c, syl in group([r for r in good if is_sylvan(r)], cfg).items():
        mine = {seed(r): r for r in syl}
        common = sorted(s for s in mine if s in cudd)
        if not common:
            continue
        t = Table(f"seeds_{c}", f"{c} against CUDD, per seed",
                  ["seed", "initial", "sylvan", "cudd", "ratio", "sylvan swaps",
                   "cudd swaps", "sylvan s", "cudd s", "hash"])
        ratios, sw, tm = [], [], []
        wins = losses = ties = hash_ok = sound_ok = 0
        for s in common:
            a, b = mine[s], cudd[s]
            ra = a["nodes_after"] / b["nodes_after"]
            ratios.append(ra)
            wins += a["nodes_after"] < b["nodes_after"]
            losses += a["nodes_after"] > b["nodes_after"]
            ties += a["nodes_after"] == b["nodes_after"]
            if b.get("swaps"):
                sw.append(a["swaps"] / b["swaps"])
            if b.get("sift_s"):
                tm.append(a["sift_s"] / b["sift_s"])
            h = a.get("eval_hash") == b.get("eval_hash") and a.get("eval_hash") not in (None, "0")
            hash_ok += h
            sound_ok += a.get("sound") == 1 and a.get("valid") == 1
            t.add(s, a["nodes_before"], a["nodes_after"], b["nodes_after"], F(ra),
                  a["swaps"], int(b["swaps"]), F(a["sift_s"], 2), F(b["sift_s"], 2),
                  "OK" if h else "DIFF")
        t.emit(csv_dir)

        n = len(ratios)
        lo, hi = min(ratios), max(ratios)
        bands = [sum(x < 0.99 for x in ratios),
                 sum(0.99 <= x <= 1.01 for x in ratios),
                 sum(1.01 < x <= 1.05 for x in ratios),
                 sum(1.05 < x <= 1.20 for x in ratios),
                 sum(x > 1.20 for x in ratios)]
        outliers = [f"{s} ({r:.3f})" for s, r in zip(common, ratios) if r > 1.10 or r < 0.95]
        t = Table(f"seeds_{c}_summary", f"{c} against CUDD, over {n} seeds",
                  ["measure", "value"])
        t.add("final-size ratio geomean", F(geomean(ratios), 4))
        t.add("final-size ratio median", F(statistics.median(ratios), 4))
        t.add("final-size ratio mean", F(statistics.mean(ratios), 4))
        t.add("final-size ratio min", f"{lo:.4f} (seed {common[ratios.index(lo)]})")
        t.add("final-size ratio max", f"{hi:.4f} (seed {common[ratios.index(hi)]})")
        t.add("wins / losses / ties", f"{wins} / {losses} / {ties}")
        t.add("bands <0.99 / 0.99-1.01 / 1.01-1.05 / 1.05-1.20 / >1.20",
              " / ".join(str(b) for b in bands))
        t.add("swap ratio geomean (median)",
              f"{geomean(sw):.3f} ({statistics.median(sw):.3f})" if sw else "-")
        t.add("sift time ratio geomean (median)",
              f"{geomean(tm):.3f} ({statistics.median(tm):.3f})" if tm else "-")
        t.add("cross-tool hash agreement", f"{hash_ok}/{n}")
        t.add("sylvan sound and valid", f"{sound_ok}/{n}")
        t.add("outliers (ratio >1.10 or <0.95)", ", ".join(outliers) or "none")
        t.emit(csv_dir)


# --------------------------------------------------------------- backends

def backends(rows, csv_dir):
    check = ok([r for r in rows if r.get("exp") == "backends-check"])
    if check:
        print("## Backends: equality pass\n")
        failures([r for r in rows if r.get("exp") == "backends-check"])
        t = Table("backends_check", "Same sift on every nodes table",
                  ["case", "workers", "verdict", "builds", "final", "swaps",
                   "eval hash", "differing"],
                  ["The backends must agree on final size, swap count and "
                   "evaluation hash; they may differ only in time."])
        for (case, w), rs in group([r for r in check if is_sylvan(r)],
                                   lambda r: (r["case"], r.get("workers"))).items():
            keys = {build(r): (r["nodes_after"], r["swaps"], r.get("eval_hash"))
                    for r in rs}
            vals = ordered(keys.values())
            ref = vals[0]
            differ = [b for b, k in keys.items() if k != ref]
            t.add(case, w, "identical" if len(vals) == 1 else "DIFFER",
                  " ".join(keys), ref[0], ref[1], ref[2], " ".join(differ) or "-")
        t.emit(csv_dir)
        hashes(check, csv_dir, "backends_hashes")
        sv = [r for r in check if is_sylvan(r) and (r.get("sound") != 1 or r.get("valid") != 1)]
        if sv:
            print(f"**SOUNDNESS FAILURES: {', '.join(r['case'] + ' ' + cfg(r) for r in sv)}**\n")
    timing(rows, csv_dir, "backends", "Backends: sift wall time (s)")


# --------------------------------------------------------------- refusals

def refusals(rows, csv_dir):
    syl = [r for r in ok(rows) if is_sylvan(r)]
    if not syl:
        return
    print("## Refusals audit\n")
    nonzero = [r for r in syl if r.get("refusals", 0)]
    print(f"{len(syl)} Sylvan runs inspected; "
          f"{len(nonzero) or 'none'} had a swap refused.\n")
    t = Table("refusals", "Runs with refused swaps",
              ["exp", "case", "variant", "rep", "refusals", "swaps"])
    for r in nonzero:
        t.add(r.get("exp", "-"), r.get("case"), cfg(r), r.get("rep"),
              r["refusals"], r.get("swaps"))
    t.emit(csv_dir)


SECTIONS = OrderedDict([
    ("quality", quality),
    ("timing", timing),
    ("soundness", soundness),
    ("queens", queens),
    ("seeds", seeds),
    ("backends", backends),
    ("refusals", refusals),
])


def main():
    p = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0].strip(),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="\n\n".join(__doc__.split("\n\n")[1:]))
    p.add_argument("--csv", metavar="DIR", help="also write every table to DIR/<table>.csv")
    p.add_argument("section", choices=list(SECTIONS) + ["all"])
    p.add_argument("files", nargs="+", metavar="FILE.jsonl")
    a = p.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")  # the en dash in min-max ranges

    rows = load(a.files)
    if not rows:
        sys.exit("no rows")
    if a.section == "all":
        for f in SECTIONS.values():
            f(rows, a.csv)
    else:
        SECTIONS[a.section](rows, a.csv)


if __name__ == "__main__":
    main()
