#!/usr/bin/env bash
# run-quality.sh -- does sylvan_sift find orders as good as CUDD's?
#
# Every case is sifted once by every Sylvan build at every worker count, at
# nodes_threshold 0, and once by CUDD (Cudd_ReduceHeap with CUDD_REORDER_SIFT,
# siftMaxVar and siftMaxSwap raised to INT_MAX so both tools are unlimited,
# maxGrowth 1.2 on both sides). Rows carry the final order (--order), so the
# summary can check that more workers reach exactly the same order as one.
# Final sizes and swap counts are deterministic; one repetition is enough.
#
# Defaults: --reps 1 --workers 1,4 --timeout 1800
#           --cases "bryant6 bryant8 bryant10 bryant12 bryant14
#                    C880-file C880-rev C880-rnd1 C880-rnd2"
# Own options:
#   --thresholds LIST  also sift with these nodes_threshold values, e.g. 0,32
#   --cudd-defaults    also run CUDD with its own siftMaxVar/siftMaxSwap caps
#
# Example:
#   run-quality.sh --sylvan-build trie=build-trie \
#     --cudd-bench build-trie/benchmark/sift/cudd_bench \
#     --inputs inputs --out results/quality.jsonl
# Summarise with: summarize.py quality results/quality.jsonl
set -uo pipefail
SB_EXP=quality
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
sb_parse_common "$@"

thresholds=0
cudd_defaults=0
set -- "${SB_REST[@]}"
while [ $# -gt 0 ]; do
  case "$1" in
    --thresholds)    [ $# -ge 2 ] || sb_die "$1 needs a value"; thresholds=${2//,/ }; shift 2 ;;
    --cudd-defaults) cudd_defaults=1; shift ;;
    *) sb_die "unknown option $1" ;;
  esac
done

sb_defaults 1 1,4 1800 "$SB_CASES_BRYANT $SB_CASES_C880"

for th in $thresholds; do
  [[ "$th" =~ ^[0-9]+$ ]] || sb_die "bad threshold '$th'"
  if [ "$th" = 0 ]; then
    sb_add_sylvan_variants --order
  else
    sb_add_sylvan_variants --tag "-t$th" --order --threshold "$th"
  fi
done
sb_add_cudd_variant cudd --order
if [ "$cudd_defaults" = 1 ]; then
  sb_add_cudd_variant cudd-defaults --order --cudd-defaults
fi

sb_header
sb_interleave "$SB_CASES" "$SB_REPS"
sb_footer
