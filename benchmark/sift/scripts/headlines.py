#!/usr/bin/env python3
"""
headlines.py -- the tables of the headlines, sections H1..H5 of
EXPERIMENTS.md, from the rows that run-headlines.sh wrote, and the reference
check.

    headlines.py --in DIR --out DIR [--no-check]

Reads every *.jsonl in --in (and skips.jsonl, the cells run-headlines.sh
skipped). For each headline it writes OUT/hN.md -- the main table first, then
its supporting tables -- and OUT/hN.csv, the main table in long form with the
median, minimum, maximum and n of every timed quantity in columns of their own;
supporting tables go to OUT/hN-*.csv, and the summarize.py sections H3 reuses
to OUT/h3-detail/. OUT/headlines.md has the reference check and every table.

  H1  which unique table: sift time per backend and worker count, CUDD at P1;
      final size, swaps, refusals; whether every backend and worker count did
      the identical sift; the --stats counters, by whatever names
      sylvan_stats_report gives them
  H2  what made parallel sifting scale: shared cursor against striped starts,
      batched primitives against --no-batch (median deltas), and the
      SIFT-PHASES breakdown
  H3  schedule fidelity and quality against CUDD: final size and swap ratios,
      same order at more workers, evaluation-hash agreement, CUDD's own caps,
      the full-legs ablation, soundness and the 30-seed distribution
  H4  runs in bdd-benchmark; here only the fault trees (h4b-fta): the sift
      from Storm-dft's DFS and TDLR orders against Storm-dft's flat BDD under
      SCRAM's order, and on how many trees it ends at or below that size
  H5  memory system: build and sift time with and without huge-page advice,
      with the THP mode the rows were measured under

REFERENCE CHECK. Final shared node counts are deterministic: under threshold 0,
growth 1.2, no limits, every Sylvan backend at every worker count and CUDD with
its caps raised must end at

    C880-file 5234 (CUDD 7064)    C880-rnd1 31088 (CUDD 23789)    queens10 25330 (25331)
    C880-rev 36533 (36533)        C880-rnd2 26972 (26973)         queens12 427043 (427043)
    bryantN 2N+1 (2N+1)

from initial sizes 346660, 470046, 844442, 1066147, 25945, 435170 and
2^(N+1)-1 (both tools). Within one stanza every row of a case that hashes its
evaluation on the same vectors must have the same eval_hash, whatever tool or
backend produced it, and C880-file on 64 vectors of the default seed must hash
to b63d2a3eade6548e. A row that verified must be sound and valid.

The fault trees (family fta of cases.tsv, rows with prob_before and
prob_after from --probs) have three checks more, against ../fta-reference.tsv:
prob_before and prob_after of every row agree to a relative 1e-12 (the same
terms added in another order); every row of a tree, whatever its tool, order
or settings, has the same probability to that tolerance; and where the
dataset publishes one, it is that probability to 6 significant digits
(within half a unit of the sixth). TREE.dfs-file and TREE.tdlr-file start at
the size Storm-dft reports under that order, unless the row ran with
--auto-build, whose build has sifted already.

Any violation fails the check: it is listed at the top of headlines.md, the
cell is marked in its table, and the exit status is 1. --no-check reports
without failing.
"""

import argparse
import contextlib
import io
import json
import math
import os
import re
import shlex
import statistics
import sys
from collections import OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import summarize as sz  # noqa: E402  (Table, MMM, F and the row helpers)

# ------------------------------------------------------------- references

# case: (initial, Sylvan final, CUDD final)
REFERENCE = OrderedDict([
    ("C880-file", (346660, 5234, 7064)),
    ("C880-rev", (470046, 36533, 36533)),
    ("C880-rnd1", (844442, 31088, 23789)),
    ("C880-rnd2", (1066147, 26972, 26973)),
    ("queens10", (25945, 25330, 25331)),
    ("queens12", (435170, 427043, 427043)),
])
# (case, vectors, seed): eval_hash
HASH_REFERENCE = {("C880-file", 64, 12345): "b63d2a3eade6548e"}
DEFAULT_SEED = 12345


# The fault trees: published top-event probabilities and Storm-dft's sizes.
FTA_REFERENCE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fta-reference.tsv")
FTA_CASE = re.compile(r"(?P<tree>.+)\.(?P<order>dfs|tdlr)-(?P<how>file|rev|rnd\d+)")
PROB_RTOL = 1e-12


def load_fta_reference(path=FTA_REFERENCE):
    """tree: (published probability as written, or None; {order: Storm size}),
    the orders dfs, tdlr and scram (SCRAM's, a comparator only)."""
    ref = {}
    if not os.path.isfile(path):
        return ref
    header = None
    with open(path) as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            cells = line.split("\t")
            if header is None:
                header = cells
                continue
            row = dict(zip(header, cells))

            def val(k):
                v = row.get(k, "-")
                return None if v in ("-", "") else v
            sizes = {o: int(val("storm_" + o)) for o in ("dfs", "tdlr", "scram")
                     if val("storm_" + o)}
            ref[row["tree"]] = (val("probability"), sizes)
    return ref


FTA = load_fta_reference()


def probs_agree(a, b):
    """Two probabilities of one function, added up in different orders."""
    return abs(a - b) <= PROB_RTOL * max(abs(a), abs(b))


def six_digits(p, published):
    """Whether P is PUBLISHED (written to 6 significant digits) to within half
    a unit of its sixth digit."""
    q = float(published)
    if q == 0:
        return p == 0
    half = 0.5 * 10 ** (math.floor(math.log10(abs(q))) - 5)
    return abs(p - q) <= half * (1 + 1e-9)


def fta_case(case):
    """(tree, order, how) of a fault-tree case name TREE.ORDER-file|rev|rndK, or None."""
    m = FTA_CASE.fullmatch(case or "")
    return (m.group("tree"), m.group("order"), m.group("how")) if m else None


def reference(case):
    """(initial or None, sylvan final, cudd final) for a case, or None."""
    if case in REFERENCE:
        return REFERENCE[case]
    m = re.fullmatch(r"bryant(\d+)", case or "")
    if m:
        n = int(m.group(1))
        return (2 ** (n + 1) - 1, 2 * n + 1, 2 * n + 1)
    return None


# ------------------------------------------------------------- rows

def load_dir(d):
    rows, skips = [], []
    for name in sorted(os.listdir(d)):
        if not name.endswith(".jsonl"):
            continue
        path = os.path.join(d, name)
        target = skips if name == "skips.jsonl" else rows
        for r in sz.load([path]):
            r.setdefault("_file", name)
            target.append(r)
    return rows, skips


def num(r, k, default=None):
    v = r.get(k, default)
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def argv_opt(r, opt, default=None):
    """The value of the last OPT in the row's driver arguments."""
    try:
        words = shlex.split(r.get("argv") or "")
    except ValueError:
        words = (r.get("argv") or "").split()
    val = default
    for i, w in enumerate(words[:-1]):
        if w == opt:
            val = words[i + 1]
    return val


def where(r):
    return f"{r.get('stanza') or r.get('exp', '?')} {sz.cfg(r)} rep {r.get('rep', '?')}"


def config_name(r):
    return r.get("config_name") or sz.build(r)


def backend(r):
    return r.get("build_backend") or r.get("backend") or sz.tool(r)


def sylvan_settings_ok(r):
    """Settings under which the reference final sizes hold."""
    return (r.get("sift", 1) == 1 and not r.get("auto")
            and num(r, "threshold", 0) == 0
            and abs(num(r, "growth", 1.2) - 1.2) < 1e-6
            and num(r, "max_swaps", 0) == 0 and num(r, "max_vars", 0) == 0
            and num(r, "time_limit", 0) == 0 and num(r, "sift_status", 0) == 0)


def cudd_settings_ok(r):
    return (r.get("sift", 1) == 1 and r.get("unlimited", 1) == 1
            and abs(num(r, "growth", 1.2) - 1.2) < 1e-6 and r.get("sift_ok", 1) == 1)


# ------------------------------------------------------------- the check

class Check:
    def __init__(self):
        self.items = []          # (ok, kind, case, where, expected, got)
        self.bad_size = set()    # id() of rows whose node count is wrong
        self.bad_any = set()     # id() of rows that failed any check
        self.warnings = []

    def add(self, ok, kind, case, at, expected, got, row=None, size=False):
        self.items.append((ok, kind, case, at, expected, got))
        if not ok and row is not None:
            self.bad_any.add(id(row))
            if size:
                self.bad_size.add(id(row))

    @property
    def failures(self):
        return [i for i in self.items if not i[0]]

    def run(self, rows):
        good = sz.ok(rows)
        for r in good:
            ref = reference(r.get("case"))
            if ref is None:
                continue
            init, syl, cu = ref
            if init is not None and r.get("nodes_before") is not None:
                self.add(r["nodes_before"] == init, "initial size", r["case"], where(r),
                         init, r["nodes_before"], r, size=True)
            if sz.is_sylvan(r) and sylvan_settings_ok(r):
                self.add(r.get("nodes_after") == syl, "Sylvan final size", r["case"],
                         where(r), syl, r.get("nodes_after"), r, size=True)
            elif sz.tool(r) == "cudd" and cudd_settings_ok(r):
                self.add(r.get("nodes_after") == cu, "CUDD final size", r["case"],
                         where(r), cu, r.get("nodes_after"), r, size=True)

        # Evaluation hashes: one per (stanza, case, vectors, seed).
        groups = OrderedDict()
        for r in good:
            h = r.get("eval_hash")
            if h in (None, "", "0"):
                continue
            vec = argv_opt(r, "--verify")
            seed = argv_opt(r, "--seed", str(DEFAULT_SEED))
            key = (r.get("stanza") or r.get("_file"), r.get("case"), vec, seed)
            groups.setdefault(key, []).append(r)
        for (st, case, vec, seed), rs in groups.items():
            hs = sz.ordered(r["eval_hash"] for r in rs)
            tools = sz.ordered(sz.tool(r) for r in rs)
            agree = len(hs) == 1
            self.add(agree, "eval_hash agreement", case,
                     f"{st}, {len(rs)} rows of {'/'.join(tools)}, {vec} vectors",
                     "one hash", " ".join(hs))
            if not agree:
                self.bad_any.update(id(r) for r in rs)
            try:
                want = HASH_REFERENCE.get((case, int(vec), int(seed)))
            except (TypeError, ValueError):
                want = None
            if want is not None:
                self.add(hs == [want], "eval_hash reference", case,
                         f"{st}, {vec} vectors, seed {seed}", want, " ".join(hs))

        # Fault trees: the probability does not move with the sift, every row
        # of a tree has the same one, and it is the published one; and the
        # unsifted size under Storm-dft's order is the size Storm-dft reports.
        trees = OrderedDict()
        for r in good:
            if "prob_before" not in r and "prob_after" not in r:
                continue
            pb, pa = r.get("prob_before"), r.get("prob_after")
            if not isinstance(pb, (int, float)) or not isinstance(pa, (int, float)):
                self.add(False, "probability computed", r.get("case"), where(r), "two numbers",
                         f"{pb} / {pa}", r)
                continue
            self.add(probs_agree(pb, pa), "probability invariant", r.get("case"), where(r),
                     f"{pb:.17g}", f"{pa:.17g}", r)
            fc = fta_case(r.get("case"))
            tree = fc[0] if fc else r.get("case")
            trees.setdefault(tree, []).append(r)
            published = FTA.get(tree, (None, {}))[0]
            if published is not None:
                self.add(six_digits(pa, published), "published probability", r.get("case"), where(r),
                         published, f"{pa:.6E}", r)
        for tree, rs in trees.items():
            ps = [r["prob_after"] for r in rs]
            lo, hi = min(ps), max(ps)
            tools = sz.ordered(sz.tool(r) for r in rs)
            ok = probs_agree(lo, hi)
            self.add(ok, "probability agreement", tree, f"{len(rs)} rows of {'/'.join(tools)}",
                     "one probability", f"{lo:.17g}" if ok else f"{lo:.17g} to {hi:.17g}")
            if not ok:
                self.bad_any.update(id(r) for r in rs)
        for r in good:
            fc = fta_case(r.get("case"))
            if not fc or fc[2] != "file" or r.get("auto_build") or r.get("nodes_before") is None:
                continue
            storm = FTA.get(fc[0], (None, {}))[1].get(fc[1])
            if storm is not None:
                self.add(r["nodes_before"] == storm, "Storm-dft size", r["case"], where(r),
                         storm, r["nodes_before"], r, size=True)

        # Soundness of every row that verified.
        for r in good:
            if r.get("sound", -1) == -1:
                continue
            self.add(r.get("sound") == 1 and r.get("valid") == 1, "sound and valid",
                     r.get("case"), where(r), "sound=1 valid=1",
                     f"sound={r.get('sound')} valid={r.get('valid')}", r)

        for r in good:
            if sz.is_sylvan(r) and r.get("refusals"):
                self.warnings.append(f"{where(r)} {r.get('case')}: {r['refusals']} swaps refused")
        for r in rows:
            if r.get("failed"):
                self.warnings.append(f"{where(r)} {r.get('case')}: run failed (rc={r.get('rc')})")

    def markdown(self, enforce):
        n = len(self.items)
        bad = self.failures
        out = ["## REFERENCE CHECK\n"]
        if not n:
            out.append("**NO DETERMINISTIC VALUE TO CHECK** -- no row of a reference case.\n")
            return "\n".join(out)
        kinds = OrderedDict()
        for ok, kind, *_ in self.items:
            k = kinds.setdefault(kind, [0, 0])
            k[0] += 1
            k[1] += not ok
        verdict = "PASS" if not bad else ("**FAIL**" if enforce else "**FAIL (not enforced)**")
        out.append(f"{verdict}: {n - len(bad)} of {n} checks agree.\n")
        out.append("| check | checked | failed |")
        out.append("|---|---:|---:|")
        for kind, (c, f) in kinds.items():
            out.append(f"| {kind} | {c} | {f if not f else f'**{f}**'} |")
        out.append("")
        if bad:
            out.append("| check | case | where | expected | got |")
            out.append("|---|---|---|---|---|")
            for _, kind, case, at, exp, got in bad[:200]:
                out.append(f"| {kind} | {case} | {at} | {exp} | **{got}** |")
            if len(bad) > 200:
                out.append(f"| ... | {len(bad) - 200} more | | | |")
            out.append("")
        if self.warnings:
            out.append(f"Not failing the check, but look at them ({len(self.warnings)}):\n")
            for w in self.warnings[:50]:
                out.append(f"- {w}")
            out.append("")
        return "\n".join(out)


# ------------------------------------------------------------- output

class Section:
    """One headline: Markdown text and the tables it wrote."""

    def __init__(self, key, title, out_dir):
        self.key, self.title, self.out_dir = key, title, out_dir
        self.parts = [f"# {title}\n"]
        self.files = []

    def text(self, s):
        self.parts.append(s)

    def table(self, t, csv_name=None, csv_only=False):
        """Emit a summarize.Table: Markdown here, CSV as OUT/<csv_name>.csv."""
        if not t.rows:
            return
        if csv_only:
            self.parts.append(f"*{t.title}: {len(t.rows)} rows, in "
                              f"{csv_name or t.name}.csv.*\n")
        else:
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                t.emit(None)
            self.parts.append(buf.getvalue())
        name = csv_name or t.name
        t.name = name
        t.write_csv(self.out_dir)
        self.files.append(os.path.join(self.out_dir, name + ".csv"))

    def reuse(self, fn, rows, sub):
        """A summarize.py section, its CSVs under OUT/<sub>/."""
        buf = io.StringIO()
        d = os.path.join(self.out_dir, sub)
        with contextlib.redirect_stdout(buf):
            fn(rows, d)
        if buf.getvalue().strip():
            self.parts.append(buf.getvalue())

    def skips(self, skips):
        mine = [s for s in skips if s.get("headline") == self.key]
        if not mine:
            return
        self.parts.append("### Skipped cells\n")
        for s in mine:
            self.parts.append(f"- **{s.get('stanza')}/{s.get('cell')}** "
                              f"({s.get('reason')}): {s.get('detail')}")
        self.parts.append("")

    def failures(self, rows):
        bad = [r for r in rows if r.get("failed")]
        if bad:
            self.parts.append(f"*{len(bad)} failed or timed-out runs are not in the tables:* "
                              + ", ".join(f"{r.get('case')} {sz.cfg(r)} rep {r.get('rep')} "
                                          f"(rc={r.get('rc')})" for r in bad[:20]) + "\n")

    def flag(self, check, rows):
        """Put a failed reference check at the top of this headline."""
        mine = [r for r in rows if r.get("headline") == self.key]
        bad = [r for r in mine if id(r) in check.bad_any]
        if bad:
            self.parts.insert(1, f"**REFERENCE CHECK FAILED on {len(bad)} rows of this "
                                 "headline; the cells are marked, the list is in "
                                 "headlines.md.**\n")

    def write(self):
        path = os.path.join(self.out_dir, f"{self.key}.md")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(self.parts).rstrip() + "\n")
        self.files.insert(0, path)
        return "\n".join(self.parts)


def mark(value, rows, check):
    """A deterministic value, flagged when any of its rows failed the check."""
    if any(id(r) in check.bad_size for r in rows):
        return f"{value} **≠REF**"
    return value


def det(rows, key):
    """The value of a deterministic field: the one value, or 'a..b' if several."""
    vals = sz.ordered(r.get(key) for r in rows if r.get(key) is not None)
    if not vals:
        return None
    if len(vals) == 1:
        v = vals[0]
        return int(v) if isinstance(v, float) and v == int(v) else v
    return f"{min(vals)}..{max(vals)}"


def mmm(rows, key, nd=4):
    return sz.MMM([num(r, key) for r in rows if num(r, key) is not None
                   and num(r, key) >= 0], nd)


def pct(a, b):
    """(a/b - 1) in percent."""
    return sz.F(100.0 * (a / b - 1.0), 1, "%") if a is not None and b else sz.F(None)


def stanza_rows(rows, *stanzas):
    return [r for r in rows if r.get("stanza") in stanzas]


def workers_of(rows):
    return sorted({int(r.get("workers", 1)) for r in rows})


# ------------------------------------------------------------- H1

def h1(rows, skips, check, out_dir):
    sec = Section("h1", "H1 Which unique table", out_dir)
    tim = stanza_rows(rows, "h1-timing")
    good = sz.ok(tim)
    sec.failures(tim)
    if not good:
        sec.text("No H1 timing rows.\n")
    syl = [r for r in good if sz.is_sylvan(r)]
    ws = workers_of(syl) or [1]
    cases = sz.ordered(r["case"] for r in good)

    # The pivot: one row per case and configuration, a column per worker count.
    t = sz.Table("h1-pivot", "Sift wall time (s) per backend and worker count",
                 ["case", "config", "final", "swaps", "refusals"] + [f"P{w}" for w in ws]
                 + [f"speedup P{ws[0]}→P{ws[-1]}", "vs CUDD at best P", "n"],
                 ["Median (min–max) of sift_s over the repetitions; n is the "
                  "repetitions per cell. Speed-up columns are ratios of medians "
                  "(above 1 is faster). CUDD runs single-threaded and is shown "
                  "in the P1 column. ≠REF marks a final size that differs "
                  "from the reference."])
    for case in cases:
        cu = [r for r in good if r["case"] == case and sz.tool(r) == "cudd"]
        cu_med = sz.median([r["sift_s"] for r in cu])
        for name in sz.ordered(config_name(r) for r in syl if r["case"] == case):
            mine = [r for r in syl if r["case"] == case and config_name(r) == name]
            cells, meds = [], {}
            for w in ws:
                rs = [r for r in mine if int(r.get("workers", 1)) == w]
                m = mmm(rs, "sift_s")
                cells.append(m)
                meds[w] = m.med
            lo, hi = meds.get(ws[0]), meds.get(ws[-1])
            best = min((v for v in meds.values() if v), default=None)
            t.add(case, name, mark(det(mine, "nodes_after"), mine, check),
                  det(mine, "swaps"), max((r.get("refusals", 0) for r in mine), default=0),
                  *cells, sz.F(sz.ratio(lo, hi), 2, "x"),
                  sz.F(sz.ratio(cu_med, best), 2, "x"),
                  min((m.n for m in cells if m.n), default=0))
        if cu:
            t.add(case, "cudd", mark(det(cu, "nodes_after"), cu, check), det(cu, "swaps"), "-",
                  mmm(cu, "sift_s"), *["" for _ in ws[1:]], None, None, len(cu))
    sec.table(t, "h1-pivot")

    # The long form, OUT/h1.csv: one row per cell, for plotting.
    t = sz.Table("h1", "Every cell",
                 ["case", "config", "backend", "workers", "n", "sift s", "build s",
                  "speedup vs own P1", "CUDD/this", "final", "swaps", "refusals",
                  "load1", "thp"])
    for case in cases:
        cu_med = sz.median([r["sift_s"] for r in good if r["case"] == case
                            and sz.tool(r) == "cudd"])
        for (name, w), rs in sz.group([r for r in good if r["case"] == case],
                                      lambda r: (config_name(r), int(r.get("workers", 1)))).items():
            base = [r for r in good if r["case"] == case and config_name(r) == name
                    and int(r.get("workers", 1)) == min(ws or [1])]
            s = mmm(rs, "sift_s")
            t.add(case, name, backend(rs[0]), w, len(rs), s, mmm(rs, "build_s", 3),
                  sz.F(sz.ratio(sz.median([r["sift_s"] for r in base]), s.med), 3)
                  if sz.is_sylvan(rs[0]) else None,
                  sz.F(sz.ratio(cu_med, s.med), 3), mark(det(rs, "nodes_after"), rs, check),
                  det(rs, "swaps"), max((r.get("refusals", 0) or 0 for r in rs)),
                  mmm(rs, "load1", 2), " ".join(sz.ordered(str(r.get("thp")) for r in rs)))
    sec.table(t, "h1")

    # Identical sifts: every backend and worker count must agree.
    t = sz.Table("h1-agreement", "Same sift on every backend and worker count",
                 ["case", "verdict", "final", "swaps", "configs x workers", "differing"],
                 ["The nodes tables and the worker count may change the time and "
                  "nothing else: final size and swap count must be identical."])
    for case in cases:
        rs = [r for r in syl if r["case"] == case]
        keys = OrderedDict()
        for r in rs:
            keys.setdefault((r.get("nodes_after"), r.get("swaps")), []).append(
                f"{config_name(r)}@P{r.get('workers')}")
        ref = max(keys, key=lambda k: len(keys[k])) if keys else (None, None)
        differ = sorted({c for k, v in keys.items() if k != ref for c in v})
        t.add(case, "identical" if len(keys) == 1 else "**DIFFER**", ref[0], ref[1],
              len({c for v in keys.values() for c in v}), " ".join(differ) or "-")
    sec.table(t, "h1-agreement")

    h1_stats(rows, sec)
    sec.skips(skips)
    sec.flag(check, rows)
    return sec


UNITS = {"B", "KB", "MB", "GB", "TB", "sec", "s", "ms", "us", "ns"}


def parse_report(text):
    """{counter: value} from sylvan_stats_report text, whatever the names.

    A line ending in numbers is NAME followed by its numbers (an operation
    line has three: count, cache get, cache put); a line with a number in the
    middle ("Unique nodes table 10 of 100 buckets filled.") is NAME and the
    first number. Lines without numbers are headings."""
    out = OrderedDict()
    for line in (text or "").splitlines():
        words = line.split()
        if not words:
            continue
        isnum = [bool(re.fullmatch(r"[0-9]+(\.[0-9]+)?", w)) for w in words]
        if not any(isnum):
            continue
        k = len(words)
        while k > 0 and isnum[k - 1]:
            k -= 1
        if k == len(words):            # number inside the line
            k = isnum.index(True)
            name = " ".join(words[:k])
            vals = [words[k]]
            unit = words[k + 1].rstrip(".") if k + 1 < len(words) else ""
            if unit in UNITS:
                name += f" [{unit}]"
        else:
            name = " ".join(words[:k])
            vals = words[k:]
        if not name:
            continue
        nums = [float(v) for v in vals]
        out[name] = nums[0]
        if len(nums) == 3:
            out[name + " [cache get]"] = nums[1]
            out[name + " [cache put]"] = nums[2]
    # Derived: X steps / X calls, for any such pair of names.
    for name in list(out):
        if name.endswith(" steps"):
            calls = name[:-len(" steps")] + " calls"
            if out.get(calls):
                out[name + " per call"] = out[name] / out[calls]
    return out


def h1_stats(rows, sec):
    st = sz.ok(stanza_rows(rows, "h1-stats"))
    if not st:
        return
    long = sz.Table("h1-stats", "Counters (sylvan_bench --stats)",
                    ["case", "phase", "config", "backend", "workers", "counter", "value"])
    data = OrderedDict()   # (case, phase) -> {(config, P): {counter: value}}
    for r in st:
        for phase, text in (r.get("stats_report") or {}).items():
            vals = parse_report(text)
            data.setdefault((r["case"], phase), OrderedDict())[
                (config_name(r), int(r.get("workers", 1)))] = vals
            for k, v in vals.items():
                long.add(r["case"], phase, config_name(r), backend(r), r.get("workers"), k, v)
    if not data:
        sec.text("H1 stats rows carry no stats_report (a build without --stats?).\n")
        return
    for (case, phase), cols in data.items():
        if phase != "sift":
            continue
        names = sz.ordered(k for v in cols.values() for k in v)
        t = sz.Table(f"h1-stats-{case}", f"Counters during the sift, {case}",
                     ["counter"] + [f"{c}@P{w}" for (c, w) in cols])
        for k in names:
            t.add(k, *[sz.F(v.get(k), 3) if k.endswith("per call") else
                       (int(v[k]) if k in v and v[k] == int(v[k]) else v.get(k))
                       for v in cols.values()])
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            t.emit(None)
        sec.text(buf.getvalue())
    sec.table(long, "h1-stats", csv_only=True)


# ------------------------------------------------------------- H2

def ablation_table(name, title, pairs, check, note):
    """pairs: [(label, backend, case, workers, base_rows, alt_rows)]"""
    t = sz.Table(name, title,
                 ["ablation", "backend", "case", "workers", "baseline s", "ablated s",
                  "delta", "n base", "n ablated", "final"],
                 [note])
    for label, b, case, w, base, alt in pairs:
        mb, ma = mmm(base, "sift_s"), mmm(alt, "sift_s")
        t.add(label, b, case, w, mb, ma, pct(ma.med, mb.med), mb.n, ma.n,
              mark(det(base + alt, "nodes_after"), base + alt, check))
    return t


def h2(rows, skips, check, out_dir):
    sec = Section("h2", "H2 What made parallel sifting scale", out_dir)
    pairs = []
    a = sz.ok(stanza_rows(rows, "h2a"))
    for (b, case, w), rs in sz.group(a, lambda r: (backend(r), r["case"],
                                                   int(r.get("workers", 1)))).items():
        base = [r for r in rs if r.get("config_variant", "base") == "base"]
        alt = [r for r in rs if r.get("config_variant") == "striped"]
        pairs.append(("striped starts (vs shared cursor)", b, case, w, base, alt))
    bt = sz.ok(stanza_rows(rows, "h2b"))
    for (b, case, w), rs in sz.group(bt, lambda r: (backend(r), r["case"],
                                                    int(r.get("workers", 1)))).items():
        base = [r for r in rs if r.get("variant") == "batched"]
        alt = [r for r in rs if r.get("variant") == "nobatch"]
        pairs.append(("single-node primitives (vs batched)", b, case, w, base, alt))
    sec.failures(stanza_rows(rows, "h2a", "h2b", "h2c"))
    if pairs:
        sec.table(ablation_table(
            "h2", "Ablations: what each change is worth", pairs, check,
            "Median (min–max) sift_s; delta is ablated/baseline - 1, so a positive "
            "delta is what the landed design saves. Variants were interleaved "
            "within every repetition."), "h2")
    else:
        sec.text("No H2 ablation rows (see the skipped cells below).\n")

    # Phase breakdown.
    ph = [r for r in sz.ok(stanza_rows(rows, "h2c")) if r.get("sift_phases")]
    if ph:
        def flat(d, pre=""):
            out = OrderedDict()
            for k, v in d.items():
                key = f"{pre}{k}"
                if isinstance(v, dict):
                    out.update(flat(v, key + "."))
                elif isinstance(v, (int, float)) and not isinstance(v, bool):
                    out[key] = float(v)
            return out
        recs = [(r, flat(r["sift_phases"][-1])) for r in ph]
        keys = sz.ordered(k for _, f in recs for k in f)
        t = sz.Table("h2-phases", "Per-phase breakdown (SIFT-PHASES, last sift of each run)",
                     ["backend", "case", "workers", "n", "sift s"] + keys,
                     ["Medians over the repetitions of every numeric field of the "
                      "SIFT-PHASES object (nested fields joined with '.'), as the "
                      "library printed them."])
        for (b, case, w), grp in sz.group(recs, lambda x: (backend(x[0]), x[0]["case"],
                                                           int(x[0].get("workers", 1)))).items():
            t.add(b, case, w, len(grp), mmm([x[0] for x in grp], "sift_s"),
                  *[sz.F(sz.median([f[k] for _, f in grp if k in f]), 4) for k in keys])
        sec.table(t, "h2-phases")
    elif stanza_rows(rows, "h2c"):
        sec.text("**The phase-timing rows carry no SIFT-PHASES object**: the build "
                 "printed none, so there is no breakdown.\n")
    sec.skips(skips)
    sec.flag(check, rows)
    return sec


# ------------------------------------------------------------- H3

def h3(rows, skips, check, out_dir):
    sec = Section("h3", "H3 Schedule fidelity and quality against CUDD", out_dir)
    q = stanza_rows(rows, "h3-quality")
    good = sz.ok(q)
    sec.failures(q)
    t = sz.Table("h3", "Final size and swaps against CUDD",
                 ["case", "initial", "config", "final", "CUDD", "ratio", "swaps",
                  "CUDD swaps", "swap ratio", "same order at P>1", "CUDD own caps",
                  "eval_hash"],
                 ["Lowest worker count, threshold 0, growth 1.2, CUDD with "
                  "siftMaxVar/siftMaxSwap raised. A ratio below 1 means Sylvan "
                  "found the smaller order. 'same order at P>1': final size and "
                  "order identical at every higher worker count. 'CUDD own caps': "
                  "CUDD's defaults give the same final size. eval_hash: every row "
                  "of the case, both tools, hashes the same evaluation."])
    for case in sz.ordered(r["case"] for r in good):
        rs = [r for r in good if r["case"] == case]
        cu = [r for r in rs if sz.tool(r) == "cudd" and r.get("unlimited", 1) == 1]
        cd = [r for r in rs if sz.tool(r) == "cudd" and r.get("unlimited") == 0]
        cuf = cu[0]["nodes_after"] if cu else None
        cus = int(cu[0]["swaps"]) if cu and cu[0].get("swaps") is not None else None
        hs = sz.ordered(r.get("eval_hash") for r in rs if r.get("eval_hash") not in (None, "", "0"))
        hv = "-" if not hs else (f"AGREE ({hs[0]})" if len(hs) == 1 else "**DISAGREE** " + " ".join(hs))
        caps = "-"
        if cu and cd:
            caps = "same" if cu[0]["nodes_after"] == cd[0]["nodes_after"] else "**DIFFER**"
        for name in sz.ordered(config_name(r) for r in rs if sz.is_sylvan(r)):
            mine = [r for r in rs if sz.is_sylvan(r) and config_name(r) == name]
            low = min(int(r.get("workers", 1)) for r in mine)
            base = [r for r in mine if int(r.get("workers", 1)) == low]
            b0 = base[0]
            others = [r for r in mine if int(r.get("workers", 1)) != low]
            if not others:
                same = "-"
            elif all(r.get("nodes_after") == b0.get("nodes_after")
                     and r.get("order_after") == b0.get("order_after") for r in others):
                same = "yes" if b0.get("order_after") is not None else "yes (size)"
            else:
                same = "**NO**"
            t.add(case, b0.get("nodes_before"), name,
                  mark(b0.get("nodes_after"), base, check),
                  mark(cuf, cu, check) if cu else None,
                  sz.F(sz.ratio(b0.get("nodes_after"), cuf)),
                  b0.get("swaps"), cus, sz.F(sz.ratio(b0.get("swaps"), cus), 3),
                  same, caps, hv)
    sec.table(t, "h3")

    # Full legs against the interaction bound.
    fl = sz.ok(stanza_rows(rows, "h3-fulllegs"))
    if fl:
        t = sz.Table("h3-fulllegs", "Interaction bound against --full-legs",
                     ["case", "config", "bounded swaps", "full swaps", "pruned",
                      "same final", "bounded s", "full s", "CUDD swaps"],
                     ["The bound only skips positions that cannot win, so the final "
                      "size must be the same; 'pruned' is the share of the full "
                      "walk's swaps it saves."])
        for case in sz.ordered(r["case"] for r in fl):
            rs = [r for r in fl if r["case"] == case]
            cu = [r for r in rs if sz.tool(r) == "cudd"]
            for name in sz.ordered(config_name(r) for r in rs if sz.is_sylvan(r)):
                b = [r for r in rs if config_name(r) == name and r.get("variant") == "bounded"]
                f = [r for r in rs if config_name(r) == name and r.get("variant") == "fulllegs"]
                bs, fs = det(b, "swaps"), det(f, "swaps")
                pr = (1 - bs / fs) * 100 if isinstance(bs, (int, float)) and \
                    isinstance(fs, (int, float)) and fs else None
                t.add(case, name, bs, fs, sz.F(pr, 1, "%"),
                      "yes" if det(b, "nodes_after") == det(f, "nodes_after") else "**NO**",
                      mmm(b, "sift_s"), mmm(f, "sift_s"), det(cu, "swaps"))
        sec.table(t, "h3-fulllegs")

    # The rest: summarize.py's own sections, under OUT/h3-detail/.
    sec.reuse(sz.soundness, stanza_rows(rows, "h3-soundness"), "h3-detail")
    sec.reuse(sz.seeds, stanza_rows(rows, "h3-seeds"), "h3-detail")
    sec.reuse(sz.quality, q, "h3-detail")
    sec.skips(skips)
    sec.flag(check, rows)
    return sec


# ------------------------------------------------------------- H4

def h4(rows, skips, check, out_dir):
    """The H4 section: a pointer to bdd-benchmark, and the fault trees (H4b),
    one sift from Storm-dft's orders against its flat BDD under SCRAM's order.
    Returns the section and the summary lines, one per start order."""
    sec = Section("h4", "H4 When sifting pays", out_dir)
    sec.text("H4 runs in the bdd-benchmark harness, not in this repository; see "
             "EXPERIMENTS.md, section H4. The fault trees (H4b) run here.\n")
    cells = OrderedDict()
    for r in sz.ok(rows):
        fc = fta_case(r.get("case"))
        if not fc or fc[2] != "file" or r.get("auto_build") or r.get("nodes_after") is None:
            continue
        if not (sylvan_settings_ok(r) if sz.is_sylvan(r) else
                sz.tool(r) == "cudd" and cudd_settings_ok(r)):
            continue
        cells.setdefault(fc[:2], []).append(r)
    t = sz.Table("h4b-fta", "Fault trees: one sift from Storm-dft's order against "
                 "Storm-dft's flat BDD under SCRAM's order",
                 ["tree", "start", "before", "Sylvan", "CUDD", "flat SCRAM order",
                  "Sylvan <= it", "ratio"],
                 ["One sift after a plain build (no --auto-build), threshold 0, growth "
                  "1.2, no limits. before: nodes_before, which is Storm-dft's size "
                  "under the start order. flat SCRAM order: Storm-dft's BDD under the "
                  "order extracted from SCRAM (storm_scram of fta-reference.tsv); "
                  "ratio: Sylvan's final size over it. SCRAM's own count is "
                  "modular and not comparable (README.md, Fault trees)."])
    pairs = {"dfs": [], "tdlr": []}
    for (tree, order) in sorted(cells):
        rs = cells[(tree, order)]
        syl = [r for r in rs if sz.is_sylvan(r)]
        cu = [r for r in rs if sz.tool(r) == "cudd"]
        sizes = FTA.get(tree, (None, {}))[1]
        scram = sizes.get("scram")
        # a deterministic size; were they to differ, the largest counts
        after = max((r["nodes_after"] for r in syl), default=None)
        below = ratio = None
        if after is not None and scram:
            below, ratio = after <= scram, after / scram
            pairs[order].append((after, scram))
        t.add(tree, order.upper(), mark(det(rs, "nodes_before"), rs, check),
              mark(det(syl, "nodes_after"), syl, check) if syl else None,
              mark(det(cu, "nodes_after"), cu, check) if cu else None,
              scram, "-" if below is None else ("yes" if below else "no"),
              sz.F(ratio, 2))
    lines = []
    for order, ps in pairs.items():
        if ps:
            lines.append(f"{order.upper()} start, sifted <= flat SCRAM order: "
                         f"{sum(a <= s for a, s in ps)} of {len(ps)} trees, median ratio "
                         f"{statistics.median(a / s for a, s in ps):.2f}")
    if t.rows:
        sec.table(t, "h4b-fta")
        sec.text("".join(f"- {line}\n" for line in lines))
    sec.flag(check, rows)
    return sec, lines


# ------------------------------------------------------------- H5

def h5(rows, skips, check, out_dir):
    sec = Section("h5", "H5 Memory-system lessons: huge pages", out_dir)
    rs = stanza_rows(rows, "h5")
    good = sz.ok(rs)
    sec.failures(rs)
    thps = sz.ordered(str(r.get("thp")) for r in good)
    if good and thps != ["madvise"]:
        sec.text(f"**THP mode in these rows: {', '.join(thps)}.** Only 'madvise' makes "
                 "the advice the variable: under 'always' every table gets huge pages "
                 "and under 'never' none does.\n")
    t = sz.Table("h5", "Huge-page advice on (rel-B) against off (rel-B-nohuge)",
                 ["backend", "case", "workers", "build s (huge)", "build s (no huge)",
                  "build delta", "sift s (huge)", "sift s (no huge)", "sift delta",
                  "n", "thp", "final"],
                 ["Median (min–max). delta is no-huge/huge - 1: positive means the "
                  "advice saves that much. Interleaved within every repetition."])
    for (b, case, w), grp in sz.group(good, lambda r: (backend(r), r["case"],
                                                       int(r.get("workers", 1)))).items():
        on = [r for r in grp if r.get("config_variant", "base") == "base"]
        off = [r for r in grp if r.get("config_variant") == "nohuge"]
        bon, boff = mmm(on, "build_s", 3), mmm(off, "build_s", 3)
        son, soff = mmm(on, "sift_s"), mmm(off, "sift_s")
        t.add(b, case, w, bon, boff, pct(boff.med, bon.med), son, soff,
              pct(soff.med, son.med), f"{len(on)}/{len(off)}",
              " ".join(sz.ordered(str(r.get("thp")) for r in grp)),
              mark(det(grp, "nodes_after"), grp, check))
    if t.rows:
        sec.table(t, "h5")
    else:
        sec.text("No H5 rows (see the skipped cells below).\n")
    sec.skips(skips)
    sec.flag(check, rows)
    return sec


# ------------------------------------------------------------- main

def main():
    p = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0].strip(),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="\n\n".join(__doc__.split("\n\n")[1:]))
    p.add_argument("--in", dest="indir", required=True, metavar="DIR",
                   help="run-headlines.sh's --out directory")
    p.add_argument("--out", required=True, metavar="DIR", help="where the tables go")
    p.add_argument("--no-check", action="store_true",
                   help="report the reference check without failing on it")
    a = p.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if not os.path.isdir(a.indir):
        print(f"headlines.py: --in {a.indir} is not a directory", file=sys.stderr)
        return 2
    rows, skips = load_dir(a.indir)
    if not rows and not skips:
        print(f"headlines.py: no rows in {a.indir}", file=sys.stderr)
        return 2
    os.makedirs(a.out, exist_ok=True)

    check = Check()
    check.run(rows)
    secs = [h1(rows, skips, check, a.out), h2(rows, skips, check, a.out),
            h3(rows, skips, check, a.out)]
    h4sec, fta_lines = h4(rows, skips, check, a.out)
    secs += [h4sec, h5(rows, skips, check, a.out)]

    batches = sz.ordered(str(r.get("batch")) for r in rows if r.get("batch"))
    head = ["# Headline tables\n",
            f"From {len(rows)} rows in {a.indir} (batches: {', '.join(batches) or 'unknown'}; "
            f"commits: {', '.join(sz.ordered(r.get('git') or '?' for r in rows if sz.is_sylvan(r)))}).\n",
            check.markdown(not a.no_check)]
    body = [s.write() for s in secs]
    with open(os.path.join(a.out, "headlines.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(head) + "\n" + "\n".join(body))

    print(check.markdown(not a.no_check))
    for line in fta_lines:
        print(line)
    for s in secs:
        for path in s.files:
            print(f"wrote {path}")
    print(f"wrote {os.path.join(a.out, 'headlines.md')}")
    if check.failures and not a.no_check:
        print(f"REFERENCE CHECK FAILED: {len(check.failures)} of {len(check.items)} checks",
              file=sys.stderr)
        return 1
    if not check.items:
        print("REFERENCE CHECK: nothing to check", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
