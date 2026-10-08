#!/usr/bin/env bash
# run-queens.sh -- sifting a forest whose natural order is already good.
#
# The N-queens board in row-major order, sifted by every Sylvan build and by
# CUDD. There is little to gain (about 2%), so this measures what sifting
# costs when it cannot find much. The builds dominate the run time: queens10
# builds in seconds, queens12 in several minutes per run on either tool.
#
# Defaults: --reps 3 --workers 1 --timeout 1800 --cases "queens10 queens12"
# Own options:
#   --n LIST   board sizes, e.g. 10,11,12 (the same as --cases queens10 ...)
#
# Example:
#   run-queens.sh --sylvan-build trie=build-trie \
#     --cudd-bench build-trie/benchmark/sift/cudd_bench \
#     --out results/queens.jsonl
# Summarise with: summarize.py queens results/queens.jsonl
set -uo pipefail
SB_EXP=queens
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
sb_parse_common "$@"

set -- "${SB_REST[@]}"
while [ $# -gt 0 ]; do
  case "$1" in
    --n) [ $# -ge 2 ] || sb_die "$1 needs a value"
         SB_CASES=""
         for n in ${2//,/ }; do SB_CASES+=" queens$n"; done
         shift 2 ;;
    *) sb_die "unknown option $1" ;;
  esac
done

sb_defaults 3 1 1800 "queens10 queens12"
sb_add_sylvan_variants
sb_add_cudd_variant

sb_header
sb_interleave "$SB_CASES" "$SB_REPS"
sb_footer
