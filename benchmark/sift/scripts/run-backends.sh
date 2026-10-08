#!/usr/bin/env bash
# run-backends.sh -- the same sifts on every nodes table, with identical settings.
#
# Give one --sylvan-build per backend (list, trie, llcht, llchst, ...), each a
# build of the same source tree that differs only in -DSYLVAN_NODES_TABLE.
#
# First an equality pass: every build, at every worker count, sifts every case
# once with --verify, and the summary checks that final size, swap count and
# evaluation hash agree across all of them -- the backends must differ in
# speed and in nothing else. Then the timing pass: --reps repetitions with the
# builds interleaved inside each repetition, so that drift over the batch
# lands on every backend alike.
#
# Defaults: --reps 5 --workers 1,8 --timeout 1800 --cases "C880-rnd2 C880-file"
# Own options:
#   --check-vectors N  vectors for the equality pass (default 64, 0 skips it)
#
# Example:
#   run-backends.sh --sylvan-build list=build-list --sylvan-build trie=build-trie \
#     --sylvan-build llcht=build-llcht --sylvan-build llchst=build-llchst \
#     --inputs inputs --out results/backends.jsonl
# Summarise with: summarize.py backends results/backends.jsonl
set -uo pipefail
SB_EXP=backends
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
sb_parse_common "$@"

check=64
set -- "${SB_REST[@]}"
while [ $# -gt 0 ]; do
  case "$1" in
    --check-vectors) [ $# -ge 2 ] || sb_die "$1 needs a value"; check=$2; shift 2 ;;
    *) sb_die "unknown option $1" ;;
  esac
done
[[ "$check" =~ ^[0-9]+$ ]] || sb_die "--check-vectors must be a number"

sb_defaults 5 1,8 1800 "C880-rnd2 C880-file"
[ ${#SB_BUILDS[@]} -ge 2 ] || echo "note: only one build given; nothing to compare it with" >&2
sb_add_sylvan_variants
sb_add_cudd_variant

sb_header
if [ "$check" -gt 0 ]; then
  SB_EXP=backends-check
  for c in $SB_CASES; do
    sb_quiet "equality pass, case $c"
    for v in "${!SB_V_LABEL[@]}"; do
      sb_run_variant "$v" "$c" 0 --verify "$check"
    done
  done
  SB_EXP=backends
fi
sb_interleave "$SB_CASES" "$SB_REPS"
sb_footer
