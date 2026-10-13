#!/usr/bin/env python3
"""
compare-traces.py -- align two sifting swap traces and explain where they differ.

    compare-traces.py [options] SYLVAN_TRACE CUDD_TRACE > report.md

The traces are what `sylvan_bench --trace` and `cudd_bench --trace` write
(see README.md, "Swap traces"): one line per adjacent swap,

    POS UPPER LOWER SCORE NODES LEG

POS the level of the upper of the two variables swapped, UPPER and LOWER the
variables at POS and POS+1 before the swap, SCORE the size the sifting driver
compares after it, NODES the internal nodes of the forest after it, and LEG u
or d for a search leg up or down, b for the return to the best position, r for
reuniting a pair. On Sylvan SCORE = NODES = the session's forest size; on CUDD
SCORE = keys - isolated (what cuddSwapInPlace returns) and NODES = keys -
constants.keys.

Both tools sift one variable at a time: a first leg toward the nearer end of
the order, a second leg toward the other end, then a return to a best
position. The script cuts each trace into those excursions (the variable that
moves in a u swap is the lower one, in a d swap the upper one), and reports:

  * totals per side, the starting order each trace implies (they must agree),
    and the longest common prefix of the two swap sequences;
  * the first divergence, explained from the two traces;
  * the candidate order on both sides (the sequence of sifted variables);
  * per variable: up/down/back swaps on each side, where each leg stopped and
    why (end of the order, growth bound, or interaction bound), and whether
    the start and final positions agree;
  * the return rule of each tool replayed on both traces: Sylvan returns to
    the first position (over both legs) where the excursion's smallest SCORE
    was seen, CUDD's ddSiftingBackward to the last position its second leg
    moved to with the smallest of the excursion's initial SCORE and the
    second leg's SCOREs (or, when none has it, to where the second leg
    started); a check that each trace follows its own rule; and how many
    return swaps the other rule would have made;
  * the residual (difference in swaps) split into its causes.

The leg rules are per side: --first-rules and --second-rules say which
direction tie-break and return rule each trace follows: cudd, which
sylvan_sift follows too, or sylvan, the rules sylvan_sift followed before it
took CUDD's (down first from the exact middle; return to the first level its
legs reached with the smallest SCORE), for traces made then. Output is
Markdown on stdout.
"""

import argparse
import struct
import sys
from collections import Counter


class Swap:
    __slots__ = ("pos", "upper", "lower", "score", "nodes", "leg", "line")

    def __init__(self, fields, line):
        self.pos = int(fields[0])
        self.upper = int(fields[1])
        self.lower = int(fields[2])
        self.score = int(fields[3])
        self.nodes = int(fields[4])
        self.leg = fields[5]
        self.line = line

    def key(self):
        return (self.pos, self.upper, self.lower, self.leg)


def read_trace(path):
    swaps = []
    with open(path) as f:
        for number, text in enumerate(f, 1):
            fields = text.split()
            if not fields:
                continue
            if len(fields) != 6 or fields[5] not in "udbr?":
                sys.exit(f"{path}:{number}: not a trace line: {text.rstrip()}")
            swaps.append(Swap(fields, number))
    return swaps


class Leg:
    """One search leg: direction, moves, where it stopped and why."""

    def __init__(self, direction, start_pos, start_score):
        self.direction = direction      # 'u' or 'd'
        self.start_pos = start_pos
        self.start_score = start_score  # SCORE before its first move
        self.moves = []                 # (position after, score after, nodes)
        self.reason = None

    @property
    def end_pos(self):
        return self.moves[-1][0] if self.moves else self.start_pos


class Excursion:
    def __init__(self, var, index, first):
        self.var = var
        self.index = index            # place in this side's candidate order
        self.first = first            # index of its first swap in the trace
        self.swaps = []
        self.start_pos = None
        self.start_score = None       # SCORE before its first swap, if known
        self.start_state = None       # order before it, as a permutation
        self.legs = []                # [first leg, second leg]
        self.back = []                # return swaps
        self.other = []               # r or ? swaps
        self.end_pos = None
        self.consistent = True


def growth_limit(rules, growth):
    """The growth factor as each tool holds it: Sylvan's config field is a
    float, CUDD's maxGrowth a double."""
    if rules == "sylvan":
        return struct.unpack("f", struct.pack("f", growth))[0]
    return growth


class Side:
    def __init__(self, name, path, rules, growth):
        self.name = name
        self.path = path
        self.rules = rules
        self.growth = growth_limit(rules, growth)
        self.swaps = read_trace(path)
        self.errors = []

    def analyse(self, n):
        self.n = n
        perm = list(range(n))            # perm[pos] = initial slot of its var
        initial = [None] * n             # initial[slot] = variable
        self.excursions = []
        self.by_var = {}
        cur = None
        prev_score = None
        for i, s in enumerate(self.swaps):
            # Replay the order and recover the starting one.
            if s.pos + 1 >= n:
                self.errors.append(f"line {s.line}: level {s.pos} outside {n} variables")
                return
            for slot, var in ((perm[s.pos], s.upper), (perm[s.pos + 1], s.lower)):
                if initial[slot] is None:
                    initial[slot] = var
                elif initial[slot] != var:
                    self.errors.append(
                        f"line {s.line}: variable {var} at level {s.pos} contradicts "
                        f"the order replayed so far ({initial[slot]})")
                    return

            if s.leg in "ud":
                mover = s.lower if s.leg == "u" else s.upper
                if cur is None or cur.var != mover or cur.back or cur.other:
                    cur = Excursion(mover, len(self.excursions), i)
                    cur.start_pos = s.pos + 1 if s.leg == "u" else s.pos
                    cur.start_score = prev_score
                    cur.start_state = tuple(perm)
                    self.excursions.append(cur)
                    if mover in self.by_var:
                        self.errors.append(f"line {s.line}: variable {mover} sifted twice")
                    self.by_var.setdefault(mover, cur)
                cur.swaps.append(s)
                new_pos = s.pos if s.leg == "u" else s.pos + 1
                if not cur.legs or cur.legs[-1].direction != s.leg:
                    start = cur.legs[-1].end_pos if cur.legs else cur.start_pos
                    score = (cur.legs[-1].moves[-1][1]
                             if cur.legs and cur.legs[-1].moves else cur.start_score)
                    cur.legs.append(Leg(s.leg, start, score))
                cur.legs[-1].moves.append((new_pos, s.score, s.nodes))
            elif s.leg == "b" and cur is not None and cur.var in (s.upper, s.lower):
                cur.swaps.append(s)
                cur.back.append(s)
            else:
                if cur is not None:
                    cur.other.append(s)
                    cur.swaps.append(s)
                else:
                    self.errors.append(f"line {s.line}: {s.leg} swap outside any excursion")
            perm[s.pos], perm[s.pos + 1] = perm[s.pos + 1], perm[s.pos]
            prev_score = s.score

        self.initial = initial
        self.final_state = tuple(perm)
        for e in self.excursions:
            self._finish(e)

    def _first_down(self, pos):
        if self.rules == "cudd":
            return pos > self.n - 1 - pos
        return pos >= self.n - 1 - pos

    def _finish(self, e):
        n = self.n
        # The end position, from the last swap.
        pos = e.start_pos
        for s in e.swaps:
            if s.leg in "ud":
                pos = s.pos if s.leg == "u" else s.pos + 1
            elif e.var == s.upper:
                pos = s.pos + 1
            elif e.var == s.lower:
                pos = s.pos
        e.end_pos = pos
        if len(e.legs) > 2:
            e.consistent = False
            self.errors.append(f"variable {e.var}: {len(e.legs)} search legs")

        # The start SCORE of the first excursion is not in the trace; the
        # order it describes comes back whenever a leg moves the variable
        # across its start position, and that move's SCORE is the one.
        if e.start_score is None:
            for leg in e.legs:
                for p, score, _ in leg.moves:
                    if p == e.start_pos:
                        e.start_score = score
                        break
                if e.start_score is not None:
                    break
            if e.start_score is not None and e.legs:
                e.legs[0].start_score = e.start_score
                if len(e.legs) > 1 and not e.legs[0].moves:
                    e.legs[1].start_score = e.start_score

        # Which observed leg is the first one: the tool's direction rule.
        first = "d" if self._first_down(e.start_pos) else "u"
        legs = e.legs
        if not legs:
            first_leg, second_leg = Leg(first, e.start_pos, e.start_score), None
        elif legs[0].direction == first:
            first_leg = legs[0]
            second_leg = legs[1] if len(legs) > 1 else None
        else:
            first_leg = Leg(first, e.start_pos, e.start_score)
            second_leg = legs[0]
        if second_leg is None:
            other = "u" if first == "d" else "d"
            score = first_leg.moves[-1][1] if first_leg.moves else e.start_score
            second_leg = Leg(other, first_leg.end_pos, score)
        e.first_leg, e.second_leg = first_leg, second_leg
        for leg in (first_leg, second_leg):
            leg.reason = self._reason(leg)

        e.up = sum(len(l.moves) for l in (first_leg, second_leg) if l.direction == "u")
        e.down = sum(len(l.moves) for l in (first_leg, second_leg) if l.direction == "d")

        # Both return rules, replayed on this excursion's own SCOREs.
        e.target = {"sylvan": self._target_sylvan(e), "cudd": self._target_cudd(e)}
        e.back_under = {r: (abs(second_leg.end_pos - t) if t is not None else None)
                        for r, t in e.target.items()}
        e.follows_rule = (e.target[self.rules] == e.end_pos
                          and e.back_under[self.rules] == len(e.back))

    def _reason(self, leg):
        at_end = (lambda p: p == 0) if leg.direction == "u" else (lambda p: p == self.n - 1)
        if not leg.moves:
            return "end" if at_end(leg.start_pos) else "bound"
        if at_end(leg.end_pos):
            return "end"
        limit = leg.start_score
        if limit is None:
            return "bound?"
        for _, score, _ in leg.moves[:-1]:
            limit = min(limit, score)
        if leg.moves[-1][1] > self.growth * limit:
            return "growth"
        return "bound"

    @staticmethod
    def _target_sylvan(e):
        """The first position, over both legs, with the smallest SCORE; the
        start position wins a tie with any move."""
        best_pos, best = e.start_pos, e.start_score
        for leg in (e.first_leg, e.second_leg):
            for p, score, _ in leg.moves:
                if best is None or score < best:
                    best_pos, best = p, score
        return best_pos

    @staticmethod
    def _target_cudd(e):
        """ddSiftingBackward over the second leg's moves: the smallest of the
        initial SCORE and those moves', the last move that has it, or the
        second leg's start when none does."""
        moves = e.second_leg.moves
        smallest = e.start_score
        for _, score, _ in moves:
            if smallest is None or score < smallest:
                smallest = score
        for p, score, _ in reversed(moves):
            if score == smallest:
                return p
        return e.second_leg.start_pos


# ------------------------------------------------------------------ report

def legs_count(swaps):
    c = Counter(s.leg for s in swaps)
    return c["u"], c["d"], c["b"], c["r"] + c["?"]


def leg_text(leg):
    return f"{leg.direction}{len(leg.moves)} {leg.reason}"


def first_divergence(a, b):
    n = 0
    limit = min(len(a.swaps), len(b.swaps))
    while n < limit and a.swaps[n].key() == b.swaps[n].key():
        n += 1
    return n


def excursion_at(side, index):
    for e in side.excursions:
        if e.first <= index < e.first + len(e.swaps):
            return e
    return None


RULE_TEXT = {
    "sylvan": "the first level its legs reached with that size",
    "cudd": "the last level its second leg moved to with that size",
}

REASON_TEXT = {
    "end": "reached the end of the order",
    "growth": "grew past the growth bound",
    "bound": "was stopped by the interaction bound",
    "bound?": "stopped before the end (start size unknown)",
}


def plateau(e):
    """The levels at which an excursion saw its smallest SCORE."""
    seen = []
    if e.start_score is not None:
        seen.append((e.start_pos, e.start_score))
    for leg in (e.first_leg, e.second_leg):
        for p, score, _ in leg.moves:
            seen.append((p, score))
    if not seen:
        return None, []
    least = min(score for _, score in seen)
    return least, sorted({p for p, score in seen if score == least})


def changes_no_node(side, e):
    """Whether no swap of an excursion changes NODES: the variable it moves
    has no node in the forest, or none that crossing anything can change."""
    nodes = {s.nodes for s in e.swaps}
    if e.first > 0:
        nodes.add(side.swaps[e.first - 1].nodes)
    return len(nodes) == 1


def levels(ps):
    return ", ".join(map(str, ps[:12])) + (", ..." if len(ps) > 12 else "")


def explain_divergence(a, b, n, out):
    if n == len(a.swaps) == len(b.swaps):
        out.append("The two traces are identical: the same swaps, in the same order.")
        return
    out.append(f"The traces agree on their first {n} swaps. Swap {n + 1}:")
    out.append("")
    for side in (a, b):
        if n >= len(side.swaps):
            out.append(f"* {side.name}: none (its trace ends).")
            continue
        s = side.swaps[n]
        e = excursion_at(side, n)
        out.append(f"* {side.name}: level {s.pos}, variables {s.upper}/{s.lower}, "
                   f"leg {s.leg}, in the excursion of variable {e.var} "
                   f"(candidate {e.index + 1}).")
    out.append("")

    # The excursion the common prefix ends in is the same on both sides.
    at = max(n - 1, 0)
    ea, eb = excursion_at(a, at), excursion_at(b, at)
    if ea is None or eb is None:
        return
    if n == 0 or [s.key() for s in ea.swaps] == [s.key() for s in eb.swaps]:
        # That excursion is identical; the next ones are where it starts.
        na = a.excursions[ea.index + 1] if n and ea.index + 1 < len(a.excursions) else ea
        nb = b.excursions[eb.index + 1] if n and eb.index + 1 < len(b.excursions) else eb
        if na.var != nb.var:
            empty = [(x, y, e) for x, y, e in ((a, b, na), (b, a, nb))
                     if e.var not in y.by_var and changes_no_node(x, e)]
            if empty:
                x, y, e = empty[0]
                out.append(f"The excursion before ends identically; then {a.name} sifts "
                           f"variable {na.var} and {b.name} variable {nb.var}. {x.name} "
                           f"sifts variable {e.var}, which {y.name} never sifts, and no "
                           f"swap of that excursion changes NODES: the variable has no "
                           f"node in the forest (an input no root depends on). CUDD's "
                           f"cuddSifting sifts every variable, and such a one climbs to "
                           f"the top, as ddSiftingUp's bound (L <= limitSize) lets it "
                           f"and ddSiftingDown's (size - R < limitSize) does not, and "
                           f"stays there; Sylvan's driver takes no variable with "
                           f"population 0 as a candidate. Cause: variable with no nodes.")
                return
            out.append(f"The excursion before ends identically; then {a.name} sifts "
                       f"variable {na.var} and {b.name} variable {nb.var}. The candidate "
                       f"orders differ here (see below): two variables with the same "
                       f"population, which the two sorts break differently. Cause: "
                       f"candidate-order tie-break.")
            return
        ea, eb = na, nb
    out.append(f"Both sides are in the excursion of variable {ea.var}, from level "
               f"{ea.start_pos}, from the same order. {a.name}: first leg "
               f"{leg_text(ea.first_leg)}, second leg {leg_text(ea.second_leg)}, "
               f"{len(ea.back)} back to level {ea.end_pos}. {b.name}: first leg "
               f"{leg_text(eb.first_leg)}, second leg {leg_text(eb.second_leg)}, "
               f"{len(eb.back)} back to level {eb.end_pos}.")
    out.append("")
    for which in ("first", "second"):
        la, lb = getattr(ea, which + "_leg"), getattr(eb, which + "_leg")
        if la.direction != lb.direction:
            out.append(f"The {which} legs go different ways ({la.direction} and "
                       f"{lb.direction}): the variable is at the middle of the order and "
                       f"the two sides break that tie differently (the sylvan rules go "
                       f"down first, the cudd rules up). Cause: direction tie-break.")
            return
        if len(la.moves) != len(lb.moves):
            short, long_ = (a, la), (b, lb)
            if len(la.moves) > len(lb.moves):
                short, long_ = (b, lb), (a, la)
            out.append(f"The {which} leg ({la.direction}) of {short[0].name} "
                       f"{REASON_TEXT.get(short[1].reason, short[1].reason)} after "
                       f"{len(short[1].moves)} moves, at level {short[1].end_pos}; "
                       f"{long_[0].name}'s went on for {len(long_[1].moves)} "
                       f"({REASON_TEXT.get(long_[1].reason, long_[1].reason)}). From "
                       f"the same order, so the two sides apply different stop tests: "
                       f"a run with --full-legs has no interaction bound; otherwise the "
                       f"growth tests differ (CUDD's sizes count its 4 constants, which "
                       f"sylvan_bench --growth-offset 4 adds), or the bound's per-level "
                       f"counts differ by the isolated projection functions (CUDD leaves "
                       f"them out; a Sylvan harness that holds them as roots counts "
                       f"them). Cause: leg stop.")
            return
    ma, pa = plateau(ea)
    mb, pb = plateau(eb)
    out.append(f"The search legs are the same; the returns differ. {a.name}'s smallest "
               f"SCORE in this excursion is {ma}, seen at level"
               f"{'s' if len(pa) != 1 else ''} {levels(pa)}; {b.name}'s is {mb}, at "
               f"level{'s' if len(pb) != 1 else ''} {levels(pb)}. {a.name} returns to "
               f"{RULE_TEXT[a.rules]} ({ea.end_pos}), {b.name} to {RULE_TEXT[b.rules]} "
               f"({eb.end_pos}).")
    if pa == pb:
        out.append(f"Both scores single out the same run of levels. A run of equal size "
                   f"is what crossing variables the sifted one does not interact with "
                   f"gives: those swaps change no node. Cause: return-leg tie-break.")
    else:
        out.append(f"The two scores single out different levels: they differ by the "
                   f"isolated projection functions, which CUDD leaves out of its score "
                   f"and a Sylvan harness that holds them as roots scores. Cause: score.")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[1].strip() if __doc__ else None,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sylvan", help="the first trace (sylvan_bench --trace)")
    ap.add_argument("cudd", help="the second trace (cudd_bench --trace)")
    ap.add_argument("--names", default="Sylvan,CUDD", help="names of the two sides")
    ap.add_argument("--first-rules", choices=("sylvan", "cudd"), default="cudd")
    ap.add_argument("--second-rules", choices=("sylvan", "cudd"), default="cudd")
    ap.add_argument("--growth", type=float, default=1.2, help="the growth bound both ran with")
    ap.add_argument("--cudd-constants", type=int, default=4,
                    help="constants in CUDD's table (one, zero, +/-infinity), to "
                         "recover its isolated projection functions from SCORE and NODES")
    ap.add_argument("--vars", type=int, default=0,
                    help="number of variables (default: inferred from the deepest level swapped)")
    ap.add_argument("--title", default=None)
    ap.add_argument("--no-per-variable", action="store_true",
                    help="leave out the per-variable table")
    ap.add_argument("--order-out", metavar="FILE",
                    help="write the second trace's candidate order, one variable per line "
                         "(for sylvan_bench --candidates)")
    args = ap.parse_args()

    names = args.names.split(",")
    a = Side(names[0], args.sylvan, args.first_rules, args.growth)
    b = Side(names[1], args.cudd, args.second_rules, args.growth)
    n = args.vars or max([s.pos + 2 for s in a.swaps + b.swaps] or [0])
    a.analyse(n)
    b.analyse(n)
    errors = a.errors + b.errors
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

    if args.order_out:
        with open(args.order_out, "w") as f:
            for e in b.excursions:
                f.write(f"{e.var}\n")

    out = []
    title = args.title or f"Swap traces: {a.name} against {b.name}"
    out.append(f"# {title}")
    out.append("")
    out.append(f"`{a.path}` ({a.name}, {a.rules} rules) against `{b.path}` ({b.name}, "
               f"{b.rules} rules); {n} variables, growth {args.growth}.")
    out.append("")

    # ---------------------------------------------------------- summary
    prefix = first_divergence(a, b)
    ua, da, ba, oa = legs_count(a.swaps)
    ub, db, bb, ob = legs_count(b.swaps)
    out.append("## Summary")
    out.append("")
    out.append(f"| | {a.name} | {b.name} | difference |")
    out.append("|---|---:|---:|---:|")

    def row(label, x, y):
        diff = f"{x - y:+d}" if isinstance(x, int) and isinstance(y, int) else ""
        out.append(f"| {label} | {x} | {y} | {diff} |")

    row("swaps", len(a.swaps), len(b.swaps))
    row("search swaps up", ua, ub)
    row("search swaps down", da, db)
    row("return swaps", ba, bb)
    if oa or ob:
        row("other swaps (r, ?)", oa, ob)
    row("variables sifted (with a swap)", len(a.excursions), len(b.excursions))
    if a.swaps and b.swaps:
        row("SCORE after the last swap", a.swaps[-1].score, b.swaps[-1].score)
        row("NODES after the last swap", a.swaps[-1].nodes, b.swaps[-1].nodes)
    known_a = {i: v for i, v in enumerate(a.initial) if v is not None}
    known_b = {i: v for i, v in enumerate(b.initial) if v is not None}
    shared = set(known_a) & set(known_b)
    agree = all(known_a[i] == known_b[i] for i in shared)
    out.append("")
    out.append(f"* Starting order: replaying each trace recovers the variable at "
               f"{len(known_a)} and {len(known_b)} of the {n} starting levels; they "
               f"{'agree at all ' + str(len(shared)) + ' levels both recover' if agree else 'DISAGREE'}.")
    out.append(f"* Longest common prefix: {prefix} swaps "
               f"({100.0 * prefix / max(1, min(len(a.swaps), len(b.swaps))):.1f}% of the shorter trace).")
    if prefix:
        dn = Counter(b.swaps[i].nodes - a.swaps[i].nodes for i in range(prefix))
        ds = Counter(b.swaps[i].score - a.swaps[i].score for i in range(prefix))
        out.append(f"* On that prefix, {b.name} NODES minus {a.name} NODES is "
                   f"{', '.join(str(k) for k in sorted(dn))} (0 means the two forests have "
                   f"the same number of nodes after every swap), and {b.name} SCORE minus "
                   f"{a.name} SCORE takes {len(ds)} value{'s' if len(ds) != 1 else ''} "
                   f"from {min(ds)} to {max(ds)}.")
    if b.rules == "cudd" and b.swaps:
        iso = [s.nodes + args.cudd_constants - s.score for s in b.swaps]
        out.append(f"* {b.name}'s isolated projection functions, NODES + "
                   f"{args.cudd_constants} constants - SCORE: {min(iso)} to {max(iso)} "
                   f"over its trace (of {n} variables).")
    same_final = a.final_state == b.final_state
    out.append(f"* Final order: {'the same' if same_final else 'different'} on both sides.")
    out.append("")

    # ---------------------------------------------------------- divergence
    out.append("## First divergence")
    out.append("")
    explain_divergence(a, b, prefix, out)
    out.append("")

    # ---------------------------------------------------------- candidates
    oa_ = [e.var for e in a.excursions]
    ob_ = [e.var for e in b.excursions]
    same_pos = sum(1 for x, y in zip(oa_, ob_) if x == y)
    first_diff = next((i for i, (x, y) in enumerate(zip(oa_, ob_)) if x != y), None)
    only_a = sorted(set(oa_) - set(ob_))
    only_b = sorted(set(ob_) - set(oa_))
    out.append("## Candidate order")
    out.append("")
    out.append(f"The sequence of sifted variables (a variable whose excursion made no "
               f"swap is invisible in a trace). Both sort by node count at the start of "
               f"the sift with CUDD's unstable quicksort (util_qsort; ddUniqueCompare's "
               f"tie-break is compiled out), so ties fall as the sort leaves them. CUDD's "
               f"count includes each variable's projection function, which Sylvan's "
               f"forest holds only where an output uses it, so the two orders can differ "
               f"on equal and nearly equal counts; sylvan_bench --candidates gives "
               f"Sylvan CUDD's order.")
    out.append("")
    out.append(f"* {same_pos} of {min(len(oa_), len(ob_))} places agree"
               + (f"; the first difference is at place {first_diff + 1}." if first_diff is not None else "; the orders are identical."))
    if only_a or only_b:
        out.append(f"* Sifted only by {a.name}: {only_a or 'none'}; only by {b.name}: {only_b or 'none'}.")
    else:
        out.append("* Both sides sift the same set of variables (none skipped by one side).")
    out.append(f"* {a.name}: {' '.join(map(str, oa_))}")
    out.append(f"* {b.name}: {' '.join(map(str, ob_))}")
    out.append("")

    # ---------------------------------------------------------- leg stops
    out.append("## Where the search legs stopped")
    out.append("")
    out.append("`end` reached the end of the order, `growth` the last move grew the "
               "SCORE past growth times the smallest SCORE of the leg, `bound` stopped "
               "before the end without that (the interaction bound: no budget or "
               "refusal applies in these runs). A leg with no moves stopped at the "
               "bound before its first move, or started at its end.")
    out.append("")
    out.append(f"| leg | reason | {a.name} | {b.name} |")
    out.append("|---|---|---:|---:|")
    for which in ("first", "second"):
        ca = Counter(getattr(e, which + "_leg").reason for e in a.excursions)
        cb = Counter(getattr(e, which + "_leg").reason for e in b.excursions)
        for reason in sorted(set(ca) | set(cb)):
            out.append(f"| {which} | {reason} | {ca[reason]} | {cb[reason]} |")
    out.append("")

    # ---------------------------------------------------------- return rule
    out.append("## The return rule, replayed on both traces")
    out.append("")
    out.append(f"{a.name}'s rule (`sylvan`): the first level, over both legs, at which "
               f"the excursion's smallest SCORE was seen, the start winning a tie. "
               f"{b.name}'s rule (`cudd`, ddSiftingBackward): the last level the second "
               f"leg moved to with the smallest of the start SCORE and the second leg's "
               f"SCOREs, else the second leg's start.")
    out.append("")
    out.append(f"| | {a.name}'s trace | {b.name}'s trace |")
    out.append("|---|---:|---:|")
    out.append(f"| return swaps made | {ba} | {bb} |")
    for rule in ("sylvan", "cudd"):
        xa = sum(e.back_under[rule] or 0 for e in a.excursions)
        xb = sum(e.back_under[rule] or 0 for e in b.excursions)
        out.append(f"| return swaps under the `{rule}` rule | {xa} | {xb} |")
    fa = sum(1 for e in a.excursions if e.follows_rule)
    fb = sum(1 for e in b.excursions if e.follows_rule)
    out.append(f"| excursions that follow their own rule | {fa} of {len(a.excursions)} | "
               f"{fb} of {len(b.excursions)} |")
    ta = sum(1 for e in a.excursions if e.target["sylvan"] != e.target["cudd"])
    tb = sum(1 for e in b.excursions if e.target["sylvan"] != e.target["cudd"])
    out.append(f"| excursions where the two rules pick different levels | {ta} | {tb} |")
    out.append("")
    if fa != len(a.excursions) or fb != len(b.excursions):
        out.append("An excursion that does not follow its own rule means the trace and "
                   "this script's model of the tool disagree; they are listed in the "
                   "per-variable table with `!`.")
        out.append("")

    # ---------------------------------------------------------- residual
    total = len(a.swaps) - len(b.swaps)
    tie_a = sum(len(e.back) - (e.back_under[b.rules] or 0) for e in a.excursions)
    by_reason = Counter()
    same_state_delta = 0
    same_state_count = 0
    rest_return = 0
    missing = 0
    for e in a.excursions:
        f = b.by_var.get(e.var)
        if f is None:
            missing += len(e.swaps)
            continue
        for direction in ("u", "d"):
            la = [l for l in (e.first_leg, e.second_leg) if l.direction == direction][0]
            lb = [l for l in (f.first_leg, f.second_leg) if l.direction == direction][0]
            delta = len(la.moves) - len(lb.moves)
            if delta:
                by_reason[(direction, la.reason, lb.reason)] += delta
        rest_return += (e.back_under[b.rules] or 0) - len(f.back)
        if e.start_state == f.start_state:
            same_state_count += 1
            same_state_delta += len(e.swaps) - len(f.swaps)
    for f in b.excursions:
        if f.var not in a.by_var:
            missing -= len(f.swaps)

    same_rule = sum(d for (_, ra, rb), d in by_reason.items() if ra == rb)
    by_pair = Counter()
    for (_, ra, rb), d in by_reason.items():
        if ra != rb:
            by_pair[(ra, rb)] += d

    out.append("## The residual by cause")
    out.append("")
    out.append(f"{a.name} makes {len(a.swaps)} swaps and {b.name} {len(b.swaps)}: "
               f"{total:+d}. Matched by variable, the difference splits exactly into "
               f"the rows below. The first is the return-leg tie-break measured on "
               f"{a.name}'s own legs: its return swaps minus those {b.name}'s rule would "
               f"have made from the same legs. The search rows are the differences in "
               f"leg length, by the test that stopped each side's leg. The last return "
               f"row is the rest of the return difference, both sides measured under "
               f"{b.name}'s rule: returns from legs that differ.")
    out.append("")
    out.append("| cause | swaps |")
    out.append("|---|---:|")
    out.append(f"| return leg: tie-break among equal sizes ({a.name}'s rule against "
               f"{b.name}'s, on {a.name}'s legs) | {tie_a:+d} |")
    out.append(f"| search legs stopped by the same test on both sides, at different "
               f"levels | {same_rule:+d} |")
    for (ra, rb), delta in sorted(by_pair.items()):
        out.append(f"| search legs stopped by different tests: {a.name} `{ra}`, "
                   f"{b.name} `{rb}` | {delta:+d} |")
    out.append(f"| return leg: from search legs that differ (both under {b.name}'s "
               f"rule) | {rest_return:+d} |")
    out.append(f"| variables only one side sifted | {missing:+d} |")
    check = tie_a + sum(by_reason.values()) + rest_return + missing
    out.append(f"| **total** | **{check:+d}** |")
    out.append("")
    out.append(f"{same_state_count} of {len(a.excursions)} excursions start from the "
               f"same order on both sides; together they differ by {same_state_delta:+d} "
               f"swaps. Every other one starts from an order an earlier decision already "
               f"made different, so its legs differ for that reason as well as for their "
               f"own: past the first divergence, this table says where the swaps went, "
               f"not which rule sent them there. An exact attribution needs runs that "
               f"differ in one rule only.")
    out.append("")
    if by_reason:
        out.append(f"| search leg | {a.name} stopped at | {b.name} stopped at | swaps |")
        out.append("|---|---|---|---:|")
        for (direction, ra, rb), delta in sorted(by_reason.items()):
            out.append(f"| {'up' if direction == 'u' else 'down'} | `{ra}` | `{rb}` | {delta:+d} |")
        out.append("")

    # ---------------------------------------------------------- per variable
    if not args.no_per_variable:
        out.append("## Per variable")
        out.append("")
        out.append(f"In {a.name}'s candidate order. Legs are direction, moves and where "
                   f"the leg stopped; `back` the return swaps; `same` whether the "
                   f"excursion starts from the same order on both sides (`=`) and ends "
                   f"at the same level (`=`), `x` where it does not. `!` marks an excursion that does not "
                   f"follow its own tool's return rule.")
        out.append("")
        out.append(f"| var | # {a.name[:1]}/{b.name[:1]} | start | {a.name} first, second | "
                   f"{b.name} first, second | up | down | back | end | same | diff |")
        out.append("|---:|---|---|---|---|---|---|---|---|---|---:|")
        for e in a.excursions + [f for f in b.excursions if f.var not in a.by_var]:
            f = b.by_var.get(e.var)
            if e.var not in a.by_var:
                e, f = None, e
            def cell(x, y, fmt=lambda v: str(v)):
                return f"{fmt(x) if x is not None else '-'}/{fmt(y) if y is not None else '-'}"
            ga = (lambda attr: getattr(e, attr)) if e else (lambda attr: None)
            gb = (lambda attr: getattr(f, attr)) if f else (lambda attr: None)
            legs_a = (f"{leg_text(e.first_leg)}, {leg_text(e.second_leg)}"
                      + ("" if e.follows_rule else " !")) if e else "-"
            legs_b = (f"{leg_text(f.first_leg)}, {leg_text(f.second_leg)}"
                      + ("" if f.follows_rule else " !")) if f else "-"
            same = ""
            if e and f:
                same = ("=" if e.start_state == f.start_state else "x") + \
                       ("=" if e.end_pos == f.end_pos else "x")
            delta = (len(e.swaps) if e else 0) - (len(f.swaps) if f else 0)
            var = e.var if e else f.var
            out.append(
                f"| {var} | {cell(e.index + 1 if e else None, f.index + 1 if f else None)} | "
                f"{cell(ga('start_pos'), gb('start_pos'))} | {legs_a} | {legs_b} | "
                f"{cell(ga('up'), gb('up'))} | {cell(ga('down'), gb('down'))} | "
                f"{cell(len(e.back) if e else None, len(f.back) if f else None)} | "
                f"{cell(ga('end_pos'), gb('end_pos'))} | {same} | {delta:+d} |")
        out.append("")

    print("\n".join(out))


if __name__ == "__main__":
    main()
