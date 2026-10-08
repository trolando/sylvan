#!/usr/bin/env bash
# run-seeds.sh -- Sylvan against CUDD from many random starting orders.
#
# C880 under --seeds random variable orders (case C880-rndK for seed K), each
# sifted once by every Sylvan build and once by CUDD, with --verify so that
# every pair of rows can be checked to have built the same functions. Four
# orders say little about a heuristic; thirty give a distribution of the
# final-size ratio.
#
# Defaults: --reps 1 --workers 1 --timeout 1800, seeds 1..30
# Own options:
#   --seeds SPEC   N for 1..N, A-B for a range, or a comma list (default 30)
#   --vectors N    random input vectors for --verify (default 2000)
#   --stem NAME    the circuit, INPUTS/NAME.blif (default C880)
#
# Example:
#   run-seeds.sh --sylvan-build list=build-list \
#     --cudd-bench build-list/benchmark/sift/cudd_bench \
#     --inputs inputs --out results/seeds.jsonl
# Summarise with: summarize.py seeds results/seeds.jsonl
set -uo pipefail
SB_EXP=seeds
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
sb_parse_common "$@"

seeds=30
vectors=2000
stem=C880
set -- "${SB_REST[@]}"
while [ $# -gt 0 ]; do
  case "$1" in
    --seeds)   [ $# -ge 2 ] || sb_die "$1 needs a value"; seeds=$2; shift 2 ;;
    --vectors) [ $# -ge 2 ] || sb_die "$1 needs a value"; vectors=$2; shift 2 ;;
    --stem)    [ $# -ge 2 ] || sb_die "$1 needs a value"; stem=$2; shift 2 ;;
    *) sb_die "unknown option $1" ;;
  esac
done
[[ "$vectors" =~ ^[0-9]+$ ]] || sb_die "--vectors must be a number"

if [[ "$seeds" =~ ^[0-9]+$ ]]; then
  list=$(seq 1 "$seeds")
elif [[ "$seeds" =~ ^([0-9]+)-([0-9]+)$ ]]; then
  list=$(seq "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}")
elif [[ "$seeds" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
  list=${seeds//,/ }
else
  sb_die "bad --seeds '$seeds'"
fi
cases=""
for k in $list; do cases+=" $stem-rnd$k"; done

sb_defaults 1 1 1800 "$cases"
verify=()
[ "$vectors" -gt 0 ] && verify=(--verify "$vectors")
sb_add_sylvan_variants
sb_add_cudd_variant

sb_header
sb_interleave "$SB_CASES" "$SB_REPS" "${verify[@]}"
sb_footer
