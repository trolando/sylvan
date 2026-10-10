#!/usr/bin/env python3
"""
mef2blif.py -- a static fault tree as a BLIF circuit for sylvan_bench and
cudd_bench, with the probability of every basic event beside it.

    mef2blif.py IN.xml|IN.dft OUT.blif [--order dfs|tdlr|decl] [--max-fanin N]
                [--no-probs]

IN is an Open-PSA MEF fault tree (XML), or a static Galileo fault tree (.dft,
the format of the BDDs Strike Back artifact). OUT.blif gets one output, the top
event, and one input per basic event the top event reaches; OUT.probs (OUT.blif
with .probs for .blif) gets the probability of each of those basic events:

    # comment lines, then one line per name of .inputs, in that order:
    NAME PROBABILITY

which is what `sylvan_bench --probs` and `cudd_bench --probs` read. The BLIF
stays plain BLIF. A summary line goes to stdout.

VARIABLE ORDER. The order of .inputs is the order the drivers build under
(--var-order file). The two orders are those of Storm-dft as Basgoze, Volk,
Katoen, Khan and Stoelinga, "BDDs Strike Back: Efficient Analysis of Static
and Dynamic Fault Trees" (NFM 2022), compare: their artifact's
tools/translate.py writes the fault tree for Storm-dft with its basic events
in one of these orders, and Storm-dft takes that order as its BDD variable
order (Sect. 3.4 of the paper).

  dfs   translate.py's DFS(): a depth-first walk from the top event that takes
        the arguments of every gate left to right, in file order; a basic
        event's place is its first encounter. (DFS() walks a shared gate again
        each time it meets it; that finds nothing new, so stopping at a gate
        already walked, as here, gives the same order.) The default.
  tdlr  translate.py's TDLR(), top-down left-right: a FIFO walk from the top
        event that appends the arguments of every gate it takes from the
        queue, left to right; a basic event's place is the first time it is
        taken from the queue. Level by level, as here, with every gate
        expanded at its first occurrence only, gives the same order. "bfs" is
        a synonym.
  decl  the order of the define-basic-event elements in the file, then any
        event referenced but never declared, in DFS order.

With these, the shared node count of the unsifted forest (nodes_before) is
the BDD size Storm-dft reports for the same tree and order (the artifact's
results/sft-benchmarks/sft_sizes.csv, BDD-SIZE-STORM-DFS and -TDLR), on every
Aralia tree both tools build.

GATES. Every define-gate the top event reaches becomes one .names block, in
the order of the file; the top event is the one gate no other gate references
(in every Aralia file also the first). Within a formula:

  and (x1..xn)          one cube 11..1
  or (x1..xn)           n cubes, cube i '1' at i and '-' elsewhere
  atleast min=k (x1..xn) for n <= 10 the explicit cover, one cube per
                        k-subset (C(n,k) cubes); for n > 10 a threshold
                        network of auxiliary gates t(i,j) = "at least j of
                        xi..xn" = (xi and t(i+1,j-1)) or t(i+1,j), O(nk) gates;
                        k <= 0 is constant true, k > n constant false
  xor (x1..xn)          cubes 10 and 01; more than two arguments are odd
                        parity, as a chain of two-input xors
  not (x)               around an argument: a negated literal, a '0' in the
                        enclosing gate's cube; as the body of a gate: one
                        cube 0
  nand, nor             the and and or covers with output column 0 (off-set)
  iff (x1, x2)          cubes 11 and 00;  imply (x1, x2): cubes 0- and -1
  a bare reference      a buffer, one cube 1

A formula nested in another, other than not around an argument, becomes an
auxiliary gate (x_<gate>__fK) emitted right before the gate that uses it. A
gate of more than --max-fanin arguments (default 250; blif.c reads at most
254 fanins) is split into a balanced tree of auxiliary gates of its own
operator. Signal names are e_<event> and g_<gate>, with every character
outside [A-Za-z0-9_-[]] replaced by '_' and ~2, ~3, ... added on a clash.
House events, parameters and constants are refused.

PROBABILITIES. A basic event's probability is the value of its <float> in
the MEF file. In a Galileo file it is prob=P, or for lambda=L the probability
of failure by time 1, 1 - exp(-L): translate.py turns MEF probabilities into
rates that way, and Storm-dft's unreliability at time 1 turns them back. Every
value is written as Python's shortest repr of the double, which C's strtod
reads back exactly. A reachable event without a probability is an error
unless --no-probs is given, which writes the BLIF alone.

LICENCES. The Aralia fault trees in Open-PSA MEF come from
openpra-org/aralia-fault-tree-dataset (doi:10.5281/zenodo.20160659), which
distributes them under CC BY-SA 4.0. A BLIF or .probs file written from one
of them is an adaptation: it carries the same licence and that attribution.
fetch-inputs.sh --sets aralia fetches the XML at a pinned commit and converts
it as it installs it, so this repository holds neither the trees nor their
conversions, and an artifact that ships the XML ships this script to convert
it. The Galileo files and random trees of the BDDs Strike Back artifact
(doi:10.5281/zenodo.6390998) are GPL-3.0; this script reads them, but they are
documented in README.md ("Fault trees"), not fetched or distributed.
"""
import argparse
import itertools
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

LEAF = ("gate", "basic-event", "event", "house-event")


def read_galileo(path):
    """A static Galileo fault tree as the MEF element tree the rest reads.

        toplevel "T";  "G" and|or|KofN "a" "b" ...;  "E" lambda=L [dorm=D];
        "E" prob=P;   (and // comments)
    """
    with open(path) as f:
        text = re.sub(r"//[^\n]*", "", f.read())
    top, gates, bes = None, [], []
    for stmt in text.split(";"):
        t = [x.strip('"') for x in stmt.split()]
        if not t:
            continue
        if t[0] == "toplevel":
            top = t[1]
        elif len(t) > 1 and "=" in t[1]:
            attrs = dict(a.split("=", 1) for a in t[1:])
            if "prob" in attrs:
                p = float(attrs["prob"])
            else:
                p = 1.0 - math.exp(-float(attrs["lambda"]))
            bes.append((t[0], p))
        else:
            gates.append((t[0], t[1], t[2:]))
    gate_names = set(g[0] for g in gates)
    root = ET.Element("opsa-mef")
    ft = ET.SubElement(root, "define-fault-tree", {"name": "galileo"})
    gates.sort(key=lambda g: g[0] != top)   # the top gate first, the others in file order
    for name, typ, args in gates:
        dg = ET.SubElement(ft, "define-gate", {"name": name})
        m = re.fullmatch(r"(\d+)of(\d+)", typ)
        if m:
            f = ET.SubElement(dg, "atleast", {"min": m.group(1)})
        elif typ in ("and", "or"):
            f = ET.SubElement(dg, typ)
        else:
            raise SystemExit("unsupported Galileo gate type " + typ)
        for a in args:
            ET.SubElement(f, "gate" if a in gate_names else "basic-event", {"name": a})
    md = ET.SubElement(root, "model-data")
    for name, p in bes:
        be = ET.SubElement(md, "define-basic-event", {"name": name})
        ET.SubElement(be, "float", {"value": repr(p)})
    return root


class FaultTree:
    """The gates, the basic events and the top event of an MEF element tree."""

    def __init__(self, root):
        self.gates = {}          # name -> formula element, in file order
        for ft in root.iter("define-fault-tree"):
            for g in ft.findall("define-gate"):
                body = [k for k in g if k.tag not in ("label", "attributes")]
                if len(body) != 1:
                    raise SystemExit("gate %s: %d formulas" % (g.get("name"), len(body)))
                if g.get("name") in self.gates:
                    raise SystemExit("gate %s defined twice" % g.get("name"))
                self.gates[g.get("name")] = body[0]
        self.probs, self.decl = {}, []
        for be in root.iter("define-basic-event"):
            self.decl.append(be.get("name"))
            fl = be.find("float")
            if fl is not None:
                self.probs[be.get("name")] = float(fl.get("value"))
        for tag in ("define-house-event", "constant", "parameter"):
            if root.find(".//" + tag) is not None:
                raise SystemExit("unsupported element " + tag)
        referenced = set()
        for f in self.gates.values():
            for r in f.iter():
                if r.tag in LEAF and self.kind(r) == "gate":
                    referenced.add(r.get("name"))
        tops = [g for g in self.gates if g not in referenced]
        if len(tops) != 1:
            raise SystemExit("expected one top gate, found %s" % tops[:5])
        self.top = tops[0]
        self.dfs_bes, self.reach = self._dfs()
        self.tdlr_bes = self._tdlr()

    def kind(self, ref):
        """'gate' or 'be' for a reference."""
        t = ref.tag
        if t == "gate":
            return "gate"
        if t == "basic-event":
            return "be"
        if t == "event":   # an untyped reference: by its type, else by name
            if ref.get("type") == "gate":
                return "gate"
            if ref.get("type") == "basic-event":
                return "be"
            return "gate" if ref.get("name") in self.gates else "be"
        raise SystemExit("unsupported reference " + t)

    def refs(self, f):
        """The references of a formula, left to right, nested formulas inlined."""
        if f.tag in LEAF:
            return [f]
        res = []
        for c in f:
            res.extend(self.refs(c))
        return res

    def _dfs(self):
        """The basic events in DFS order, and the gates the top event reaches."""
        bes, seen_g, seen_b = [], {self.top}, set()
        stack = [iter(self.refs(self.gates[self.top]))]
        while stack:
            try:
                r = next(stack[-1])
            except StopIteration:
                stack.pop()
                continue
            n = r.get("name")
            if self.kind(r) == "gate":
                if n not in self.gates:
                    raise SystemExit("undefined gate " + n)
                if n not in seen_g:
                    seen_g.add(n)
                    stack.append(iter(self.refs(self.gates[n])))
            elif n not in seen_b:
                seen_b.add(n)
                bes.append(n)
        return bes, seen_g

    def _tdlr(self):
        """The basic events in TDLR order: level by level, left to right."""
        bes, seen_b, seen_g, level = [], set(), {self.top}, [self.top]
        while level:
            nxt = []
            for g in level:
                for r in self.refs(self.gates[g]):
                    n = r.get("name")
                    if self.kind(r) == "gate":
                        if n not in seen_g:
                            seen_g.add(n)
                            nxt.append(n)
                    elif n not in seen_b:
                        seen_b.add(n)
                        bes.append(n)
            level = nxt
        return bes

    def order(self, name):
        if name == "dfs":
            bes = self.dfs_bes
        elif name in ("tdlr", "bfs"):
            bes = self.tdlr_bes
        elif name == "decl":
            reach = set(self.dfs_bes)
            dset = set(self.decl)
            bes = [b for b in self.decl if b in reach] + [b for b in self.dfs_bes if b not in dset]
        else:
            raise SystemExit("unknown order " + name)
        assert sorted(bes) == sorted(self.dfs_bes)
        return bes


class Emitter:
    """The .names blocks of the reachable gates, in file order."""

    def __init__(self, tree, bes, max_fanin):
        self.tree = tree
        self.max_fanin = max_fanin
        self.used, self.sig = set(), {}
        for b in bes:
            self.sig[("be", b)] = self.mk("e_", b)
        for g in tree.gates:
            if g in tree.reach:
                self.sig[("gate", g)] = self.mk("g_", g)
        self.lines = []
        self.aux_counter = {}
        self.stats = dict.fromkeys(["and", "or", "atleast", "xor", "not_gate", "not_literal", "aux",
                                    "split", "cover_cubes", "nand", "nor", "iff", "imply", "dup_args",
                                    "buffer"], 0)
        for g, f in tree.gates.items():
            if g in tree.reach:
                self.formula(self.sig[("gate", g)], f, g)

    def mk(self, prefix, name):
        """A signal name for NAME: sanitised, prefixed, unique."""
        s = prefix + re.sub(r"[^A-Za-z0-9_\-\[\]]", "_", name)
        base, k = s, 1
        while s in self.used:
            k += 1
            s = "%s~%d" % (base, k)
        self.used.add(s)
        return s

    def emit(self, outname, fanin, cubes, polarity=1):
        self.lines.append(".names " + " ".join(fanin + [outname]))
        o = "1" if polarity else "0"
        for c in cubes:
            self.lines.append((c + " " + o) if fanin else o)
            self.stats["cover_cubes"] += 1

    def aux_name(self, parent):
        self.aux_counter[parent] = self.aux_counter.get(parent, 0) + 1
        self.stats["aux"] += 1
        return self.mk("x_", "%s__f%d" % (parent, self.aux_counter[parent]))

    @staticmethod
    def ch(pos, v):
        """The cube character asking a literal of polarity POS to have value V."""
        if pos:
            return "1" if v else "0"
        return "0" if v else "1"

    def literal(self, arg, parent):
        """(signal, positive?) of an argument; a nested formula becomes an aux gate."""
        if arg.tag in LEAF:
            return (self.sig[(self.tree.kind(arg), arg.get("name"))], True)
        if arg.tag == "not":
            inner = list(arg)
            if len(inner) != 1:
                raise SystemExit("not with %d args" % len(inner))
            s, pos = self.literal(inner[0], parent)
            self.stats["not_literal"] += 1
            return (s, not pos)
        name = self.aux_name(parent)
        self.formula(name, arg, parent)
        return (name, True)

    def andor(self, outname, op, lits, polarity):
        ch = self.ch
        fan = [s for s, _ in lits]
        if op == "and":
            self.emit(outname, fan, ["".join(ch(p, 1) for _, p in lits)], polarity)
        else:
            n = len(lits)
            self.emit(outname, fan,
                      ["".join(ch(lits[i][1], 1) if i == j else "-" for i in range(n)) for j in range(n)],
                      polarity)

    def threshold(self, outname, lits, k, parent):
        """OUTNAME = at least K of LITS, as t(i,j) = (x_i & t(i+1,j-1)) | t(i+1,j)."""
        ch = self.ch
        n = len(lits)
        memo = {}

        def T(i, j):
            if j == 0:
                return "1"
            if n - i < j:
                return "0"
            if (i, j) in memo:
                return memo[(i, j)]
            hi, lo = T(i + 1, j - 1), T(i + 1, j)
            name = outname if (i, j) == (0, k) else self.aux_name(parent)
            s, p = lits[i]
            fanin = [s] + [x for x in (hi, lo) if x not in ("0", "1")]
            cubes = []
            if hi != "0":
                c = ["-"] * len(fanin)
                c[0] = ch(p, 1)
                if hi != "1":
                    c[fanin.index(hi)] = "1"
                cubes.append("".join(c))
            if lo == "1":
                cubes.append("-" * len(fanin))
            elif lo != "0":
                c = ["-"] * len(fanin)
                c[fanin.index(lo)] = "1"
                cubes.append("".join(c))
            self.emit(name, fanin, cubes)
            memo[(i, j)] = name
            return name

        T(0, k)

    def formula(self, outname, f, parent):
        ch = self.ch
        t = f.tag
        if t in LEAF:                 # the body is a bare reference: a buffer
            s, pos = self.literal(f, parent)
            self.stats["buffer"] += 1
            self.emit(outname, [s], [ch(pos, 1)])
            return
        if t == "not":
            args = list(f)
            if len(args) != 1:
                raise SystemExit("not with %d args" % len(args))
            s, pos = self.literal(args[0], parent)
            self.stats["not_gate"] += 1
            self.emit(outname, [s], [ch(pos, 0)])
            return
        lits = [self.literal(c, parent) for c in f]
        if len(set(s for s, _ in lits)) != len(lits):
            self.stats["dup_args"] += 1
        if t in ("and", "or", "nand", "nor"):
            self.stats[t] += 1
            base = "and" if t in ("and", "nand") else "or"
            pol = 0 if t in ("nand", "nor") else 1
            if len(lits) > self.max_fanin:
                self.stats["split"] += 1
                while len(lits) > self.max_fanin:
                    nl = []
                    for i in range(0, len(lits), self.max_fanin):
                        n = self.aux_name(parent)
                        self.andor(n, base, lits[i:i + self.max_fanin], 1)
                        nl.append((n, True))
                    lits = nl
            self.andor(outname, base, lits, pol)
        elif t == "xor":
            self.stats["xor"] += 1
            acc = lits[0]
            for k, lit in enumerate(lits[1:]):
                n = outname if k == len(lits) - 2 else self.aux_name(parent)
                (x, px), (y, py) = acc, lit
                self.emit(n, [x, y], [ch(px, 1) + ch(py, 0), ch(px, 0) + ch(py, 1)])
                acc = (n, True)
        elif t == "iff":
            self.stats["iff"] += 1
            (x, px), (y, py) = lits
            self.emit(outname, [x, y], [ch(px, 1) + ch(py, 1), ch(px, 0) + ch(py, 0)])
        elif t == "imply":
            self.stats["imply"] += 1
            (x, px), (y, py) = lits
            self.emit(outname, [x, y], [ch(px, 0) + "-", "-" + ch(py, 1)])
        elif t == "atleast":
            self.stats["atleast"] += 1
            k, n = int(f.get("min")), len(lits)
            if k <= 0:
                self.emit(outname, [], [""])       # constant true
            elif k > n:
                self.emit(outname, [], [])         # constant false
            elif n <= 10:
                cubes = []
                for comb in itertools.combinations(range(n), k):
                    cs = set(comb)
                    cubes.append("".join(ch(lits[i][1], 1) if i in cs else "-" for i in range(n)))
                self.emit(outname, [s for s, _ in lits], cubes)
            else:
                self.threshold(outname, lits, k, parent)
        else:
            raise SystemExit("unsupported formula " + t)


def main():
    ap = argparse.ArgumentParser(description="A static fault tree (Open-PSA MEF or Galileo) as BLIF.")
    ap.add_argument("input", help="IN.xml (Open-PSA MEF) or IN.dft (static Galileo)")
    ap.add_argument("output", help="OUT.blif; OUT.probs is written next to it")
    ap.add_argument("--order", default="dfs", choices=["dfs", "tdlr", "bfs", "decl"])
    ap.add_argument("--max-fanin", type=int, default=250)
    ap.add_argument("--no-probs", action="store_true", help="write the BLIF only")
    a = ap.parse_args()
    if a.max_fanin < 2 or a.max_fanin > 254:
        raise SystemExit("--max-fanin must be 2 to 254")

    root = read_galileo(a.input) if a.input.endswith(".dft") else ET.parse(a.input).getroot()
    tree = FaultTree(root)
    bes = tree.order(a.order)
    em = Emitter(tree, bes, a.max_fanin)
    inputs = [em.sig[("be", b)] for b in bes]
    missing = [b for b in bes if b not in tree.probs]
    if missing and not a.no_probs:
        raise SystemExit("%s: %d basic events have no probability (the first: %s); --no-probs writes "
                         "the BLIF alone" % (a.input, len(missing), missing[0]))

    base = os.path.basename(a.input)
    with open(a.output, "w", newline="\n") as fo:
        fo.write("# %s converted by mef2blif.py (order %s)\n" % (base, a.order))
        fo.write(".model %s\n" % re.sub(r"\W", "_", base.rsplit(".", 1)[0]))
        fo.write(".inputs " + " ".join(inputs) + "\n")
        fo.write(".outputs " + em.sig[("gate", tree.top)] + "\n")
        for line in em.lines:
            fo.write(line + "\n")
        fo.write(".end\n")
    if not a.no_probs:
        probs = a.output[:-5] + ".probs" if a.output.endswith(".blif") else a.output + ".probs"
        with open(probs, "w", newline="\n") as fo:
            fo.write("# %s converted by mef2blif.py (order %s): the probability of each\n" % (base, a.order))
            fo.write("# basic event, one line per name of .inputs, in that order\n")
            for b, s in zip(bes, inputs):
                fo.write("%s %r\n" % (s, tree.probs[b]))
    print("%s: top %s gates %d/%d reachable, events %d/%d reachable, missing prob %d, %s"
          % (base, tree.top, len(tree.reach), len(tree.gates), len(bes),
             len(set(tree.decl) | set(tree.dfs_bes)), len(missing),
             " ".join("%s=%d" % kv for kv in em.stats.items() if kv[1])))


if __name__ == "__main__":
    sys.setrecursionlimit(100000)
    main()
