#!/usr/bin/env bash
# run-timing.sh -- Q2: what does a sift cost, and how does it scale?
#
# The sift wall time of every Sylvan build at every worker count, and of CUDD
# (single-threaded), all at nodes_threshold 0 so that every Sylvan variant
# does the same swaps. Build time is in every row as well, so build and sift
# cost stay separable. Variants are interleaved within each repetition.
#
# C880's sifts take between a third of a second and a few seconds; queens11
# sifts for several seconds over tens of thousands of swaps, which is where a
# parallel swap has room to show what it can do.
#
# Defaults: --reps 7 --workers 1,2,4,8,16 --timeout 1800
#           --cases "C880-file C880-rev C880-rnd1 C880-rnd2 queens11"
#
# Example, comparing two backends on the same cases:
#   run-timing.sh --sylvan-build trie=build-trie --sylvan-build list=build-list \
#     --cudd-bench build-trie/benchmark/sift/cudd_bench \
#     --inputs inputs --out results/timing.jsonl
# Summarise with: summarize.py timing results/timing.jsonl
set -uo pipefail
SB_EXP=timing
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
sb_parse_common "$@"
[ ${#SB_REST[@]} -eq 0 ] || sb_die "unknown option ${SB_REST[0]}"

sb_defaults 7 1,2,4,8,16 1800 "$SB_CASES_C880 queens11"
sb_add_sylvan_variants
sb_add_cudd_variant

sb_header
sb_interleave "$SB_CASES" "$SB_REPS"
sb_footer
