#!/usr/bin/env bash
# run-ablation.sh -- the same builds under several runtime settings, interleaved.
#
# Every --variant NAME=ARGS is one runtime setting: ARGS go to every
# sylvan_bench run of that setting, and an empty ARGS is the baseline. Each
# build, worker count and setting is one variant, labelled BUILD-NAME-pW, and
# its rows say which setting they ran under in "variant" and "variant_args".
# Within each repetition every setting of a cell runs back to back (A,B,A,B),
# which running another script once per setting would not do. CUDD, when
# given, runs once per repetition as the reference.
#
# Sections H2 and H3 of EXPERIMENTS.md use it for their runtime ablations:
# --full-legs against the interaction bound, and --no-batch (single-node table
# primitives) against the batched default. A setting whose flags a build's
# `sylvan_bench --help` does not list is refused before anything runs, so that
# a missing knob can never pass for a measured one.
#
# Defaults: --reps 5 --workers 1 --timeout 1800
#           --cases "C880-file C880-rev C880-rnd1 C880-rnd2"
# Own options:
#   --variant NAME=ARGS  a runtime setting; repeatable, at least two
#
# Example:
#   run-ablation.sh --sylvan-build list=build-list \
#     --variant batched= --variant nobatch=--no-batch \
#     --workers 1,8 --inputs inputs --out results/ablation-batch.jsonl
# Summarise with: headlines.py (the H2 and H3 tables)
set -uo pipefail
SB_EXP=ablation
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
sb_parse_common "$@"

vnames=(); vargs=()
set -- "${SB_REST[@]}"
while [ $# -gt 0 ]; do
  case "$1" in
    --variant) [ $# -ge 2 ] || sb_die "$1 needs NAME=ARGS"
               [[ "$2" == *=* ]] || sb_die "--variant wants NAME=ARGS, got '$2'"
               vnames+=("${2%%=*}"); vargs+=("${2#*=}"); shift 2 ;;
    *) sb_die "unknown option $1" ;;
  esac
done
[ ${#vnames[@]} -ge 2 ] || sb_die "give at least two --variant NAME=ARGS (one may have empty ARGS)"
for n in "${vnames[@]}"; do
  [[ "$n" =~ ^[A-Za-z0-9._+]+$ ]] || sb_die "variant name '$n' must be [A-Za-z0-9._+]"
done

sb_defaults 5 1 1800 "$SB_CASES_C880"

# Every flag a setting passes must be one the build's driver knows.
for i in "${!SB_B_NAME[@]}"; do
  help=$("${SB_B_BIN[$i]}" --help 2>&1)
  for k in "${!vnames[@]}"; do
    read -r -a words <<< "${vargs[$k]}"
    for t in "${words[@]}"; do
      [[ "$t" == --* ]] || continue
      grep -qE -- "(^|[[:space:]])$t([[:space:]]|$)" <<< "$help" ||
        sb_die "build ${SB_B_NAME[$i]}: sylvan_bench --help does not list $t (variant ${vnames[$k]}); not running it"
    done
  done
done

read -r -a common <<< "$SB_SYLVAN_ARGS"
for i in "${!SB_B_NAME[@]}"; do
  for w in $SB_WORKERS; do
    for k in "${!vnames[@]}"; do
      read -r -a words <<< "${vargs[$k]}"
      SB_VARIANT_EXTRA="\"variant\":$(sb_jstr "${vnames[$k]}"),\"variant_args\":$(sb_jstr "${vargs[$k]}")"
      sb_add_variant "${SB_B_NAME[$i]}-${vnames[$k]}-p$w" "$i" "$w" \
        "${SB_B_BIN[$i]}" --workers "$w" --threshold 0 "${words[@]}" "${common[@]}"
    done
  done
done
SB_VARIANT_EXTRA=""
sb_add_cudd_variant

sb_header
{
  for k in "${!vnames[@]}"; do echo "    variant ${vnames[$k]}: [${vargs[$k]}]"; done
} | tee -a "$SB_LOG"
sb_interleave "$SB_CASES" "$SB_REPS"
sb_footer
