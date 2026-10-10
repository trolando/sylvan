#!/usr/bin/env bash
# run-soundness.sh -- is a sifted forest still correct, and does it pay?
#
# Soundness: every output is evaluated on --vectors random input vectors, and
# its satisfying assignments are counted, before and after the sift. Vectors
# index variables by identity and a model count ignores the order, so both are
# invariant under any correct reordering; "sound" says whether they were.
# "valid" is mtbdd_is_valid on the Sylvan side and Cudd_DebugCheck on the CUDD
# side. Both drivers hash the same evaluation into "eval_hash", so equal hashes
# across tools show that both built the same functions in the first place.
#
# Downstream: unless --no-ops, every pair of outputs is conjoined and
# exclusive-ored and the results model-counted, before and after the sift. A
# control run per build with --no-sift gives the "after" phase something to
# be compared against other than the cold "before" phase.
#
# Defaults: --reps 5 --workers 1 --timeout 1800
#           --cases "C880-file C880-rev C880-rnd1 C880-rnd2"
# Own options:
#   --vectors N   random input vectors (default 5000)
#   --no-ops      skip the downstream phase and the no-sift control
#
# Example:
#   run-soundness.sh --sylvan-build trie=build-trie \
#     --cudd-bench build-trie/benchmark/sift/cudd_bench \
#     --inputs inputs --out results/soundness.jsonl
# Summarise with: summarize.py soundness results/soundness.jsonl
set -uo pipefail
SB_EXP=soundness
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
sb_parse_common "$@"

vectors=5000
ops=(--ops)
set -- "${SB_REST[@]}"
while [ $# -gt 0 ]; do
  case "$1" in
    --vectors) [ $# -ge 2 ] || sb_die "$1 needs a value"; vectors=$2; shift 2 ;;
    --no-ops)  ops=(); shift ;;
    *) sb_die "unknown option $1" ;;
  esac
done
[[ "$vectors" =~ ^[1-9][0-9]*$ ]] || sb_die "--vectors must be a positive number"

sb_defaults 5 1 1800 "$SB_CASES_C880"
sb_add_sylvan_variants
sb_add_cudd_variant
main=${#SB_V_LABEL[@]}

# The no-sift controls, one per build at the first worker count.
if [ ${#ops[@]} -gt 0 ]; then
  first_w=${SB_WORKERS%% *}
  for i in "${!SB_B_NAME[@]}"; do
    sb_add_variant "${SB_B_NAME[$i]}-p$first_w-nosift" "$i" "$first_w" \
      "${SB_B_BIN[$i]}" --workers "$first_w" --no-sift
  done
fi

sb_header
for c in $SB_CASES; do
  sb_quiet "case $c"
  for rep in $(seq 1 "$SB_REPS"); do
    for ((v = 0; v < main; v++)); do
      sb_run_variant "$v" "$c" "$rep" --verify "$vectors" "${ops[@]}"
    done
  done
  for ((v = main; v < ${#SB_V_LABEL[@]}; v++)); do
    sb_run_variant "$v" "$c" 0 --verify "$vectors" "${ops[@]}"
  done
done
sb_footer
