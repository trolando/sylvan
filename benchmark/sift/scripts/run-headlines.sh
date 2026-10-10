#!/usr/bin/env bash
# run-headlines.sh -- the headline experiments, sections H1..H5 of
# EXPERIMENTS.md, on the builds that build-matrix.sh made, one JSONL file per
# stanza.
#
#   run-headlines.sh --builds DIR --inputs DIR --out DIR [--reps N]
#                    [--workers 1,2,4,8] [--only h1,h2,h3,h5] [--quick]
#                    [--timeout S] [--cudd-bench PATH] [--append] [--dry-run]
#
# Every stanza calls one of the run-*.sh scripts, so every run is one row with
# everything those rows carry, variants interleaved A,B,A,B within each
# repetition, plus "headline", "stanza" and "batch" (SB_ROW_EXTRA), the
# configuration name and knob settings from the manifest (BINARY.row.json),
# and the THP mode, load average and timestamps of the run (lib.sh).
#
#   h1-timing    H1 which unique table: rel-{list,trie,llcht,llchst} and CUDD,
#                every worker count, the four C880 orders (n=7) and queens12
#                (n=5): sift s, swaps, final size, refusals
#   h1-stats     H1 counters: rel-*-stats with sylvan_bench --stats at the
#                lowest and highest worker count, once; the sylvan_stats_report
#                text is in each row's "stats_report"
#   h2a          H2 shared region cursor (rel-B) against striped starts
#                (rel-B-striped), B in list trie, C880, every worker count
#   h2b          H2 batched table primitives against --no-batch, same cells
#   h2c          H2 per-phase timing (rel-B-phases): SIFT-PHASES in each row
#   h3-quality   H3 final size and swaps against CUDD, raised and default CUDD
#                caps, --verify 64 so every row has an eval_hash: bryant6..14,
#                C880, queens10, queens12 at P1 and P4
#   h3-soundness H3 run-soundness.sh: 5000 vectors, downstream ops (n=5)
#   h3-seeds     H3 run-seeds.sh: 30 random orders of C880 against CUDD
#   h3-fulllegs  H3 --full-legs against the interaction bound (n=3)
#   h5           H5 rel-B against rel-B-nohuge, B in list trie: C880 and
#                queens12 build and sift time at the lowest and highest worker
#                count, with the THP mode in every row
# H4 (when sifting pays) runs in the bdd-benchmark harness; see EXPERIMENTS.md.
#
# A stanza whose configuration is not "ok" in the manifest -- knob-missing,
# or not built -- and a cell whose runtime flag (--no-batch, --stats) the
# build's sylvan_bench --help does not list, is skipped with one line saying
# so, which also goes to OUT/skips.jsonl for headlines.py. It is never run as
# if the knob were there.
#
# Options:
#   --builds DIR    a build-matrix.sh directory (holding manifest.json)
#   --inputs DIR    the directory fetch-inputs.sh filled
#   --out DIR       one JSONL (+ .log) per stanza, skips.jsonl, run-headlines.log,
#                   and a copy of manifest.json; refuses a directory that
#                   already has rows unless --append
#   --reps N        repetitions of every timed stanza (default 7 for C880,
#                   5 for queens12 and soundness, 3 for full-legs); the
#                   deterministic passes (h1-stats, h3-quality, h3-seeds) run once
#   --workers LIST  worker counts (default 1,2,4,8,16, without those above nproc)
#   --only LIST     headlines (h1 h2 h3 h5) or stanzas (h2b, h3-seeds, ...)
#   --quick         pipeline check: reps 1, workers 1,2, small cases (queens10
#                   for queens12, 3 seeds, C880-file only for soundness and
#                   full legs, 500 vectors)
#   --timeout S     wall clock cap per run (default 1800, 600 with --quick)
#   --cudd-bench P  cudd_bench to use (default: the manifest's first one)
#   --append        add to an OUT that already has rows
#   --dry-run       print the commands, run nothing
#
# Then: headlines.py --in OUT --out OUT/tables
set -uo pipefail
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

die() { echo "run-headlines.sh: $*" >&2; exit 2; }

builds=""; inputs=""; out=""; reps=""; workers=""; only=""; quick=0
timeout=""; cudd=""; append=0; dry=0
while [ $# -gt 0 ]; do
  case "$1" in
    --builds)     [ $# -ge 2 ] || die "$1 needs a value"; builds=$2; shift 2 ;;
    --inputs)     [ $# -ge 2 ] || die "$1 needs a value"; inputs=$2; shift 2 ;;
    --out)        [ $# -ge 2 ] || die "$1 needs a value"; out=$2; shift 2 ;;
    --reps)       [ $# -ge 2 ] || die "$1 needs a value"; reps=$2; shift 2 ;;
    --workers)    [ $# -ge 2 ] || die "$1 needs a value"; workers=${2//,/ }; shift 2 ;;
    --only)       [ $# -ge 2 ] || die "$1 needs a value"; only+=" ${2//,/ }"; shift 2 ;;
    --quick)      quick=1; shift ;;
    --timeout)    [ $# -ge 2 ] || die "$1 needs a value"; timeout=$2; shift 2 ;;
    --cudd-bench) [ $# -ge 2 ] || die "$1 needs a value"; cudd=$2; shift 2 ;;
    --append)     append=1; shift ;;
    --dry-run)    dry=1; shift ;;
    -h|--help)    sed -n '2,/^[^#]/{/^#/s/^# \{0,1\}//p}' "$0"; exit 0 ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
done
[ -n "$builds" ] || die "--builds DIR is required"
[ -n "$inputs" ] || die "--inputs DIR is required"
[ -n "$out" ] || die "--out DIR is required"
manifest="$builds/manifest.json"
[ -f "$manifest" ] || die "no $manifest; run build-matrix.sh first"
[ -d "$inputs" ] || die "--inputs $inputs is not a directory"
[[ -z "$reps" || "$reps" =~ ^[1-9][0-9]*$ ]] || die "--reps must be a positive number"
command -v python3 > /dev/null || die "python3 not found"
only=${only# }
for t in $only; do
  case "$t" in
    h1|h1-timing|h1-stats|h2|h2a|h2b|h2c|h3|h3-quality|h3-soundness|h3-seeds|h3-fulllegs|h5) ;;
    h4) echo "note: H4 runs in the bdd-benchmark harness, not here; see EXPERIMENTS.md" ;;
    *) die "unknown --only entry '$t' (h1 h2 h3 h5, or a stanza: see --help)" ;;
  esac
done

# ---------------------------------------------------------------- settings

ncpu=$(nproc 2>/dev/null || echo 1)
if [ -z "$workers" ]; then
  if [ "$quick" = 1 ]; then workers="1 2"; else
    workers=""; for w in 1 2 4 8 16; do [ "$w" -le "$ncpu" ] && workers+=" $w"; done
  fi
fi
workers=$(echo $workers)
for w in $workers; do [[ "$w" =~ ^[1-9][0-9]*$ ]] || die "bad worker count '$w'"; done
wmin=${workers%% *}; wmax=${workers##* }
wends=$wmin; [ "$wmax" = "$wmin" ] || wends="$wmin $wmax"
wq="1 4"; [ "$quick" = 0 ] || wq=$workers          # quality: same order at P1 and P4

C880="C880-file C880-rev C880-rnd1 C880-rnd2"
if [ "$quick" = 1 ]; then
  r_c880=${reps:-1}; r_queens=${reps:-1}; r_sound=${reps:-1}; r_legs=${reps:-1}
  QUEENS=queens10
  QUALITY="bryant6 bryant8 $C880 queens10"
  SOUND_CASES=C880-file; SOUND_VECTORS=500
  SEEDS=3; SEED_VECTORS=200
  LEGS_CASES=C880-file
  timeout=${timeout:-600}
else
  r_c880=${reps:-7}; r_queens=${reps:-5}; r_sound=${reps:-5}; r_legs=${reps:-3}
  QUEENS=queens12
  QUALITY="bryant6 bryant8 bryant10 bryant12 bryant14 $C880 queens10 queens12"
  SOUND_CASES=$C880; SOUND_VECTORS=5000
  SEEDS=30; SEED_VECTORS=2000
  LEGS_CASES=$C880
  timeout=${timeout:-1800}
fi
[[ "$timeout" =~ ^[0-9]+$ ]] || die "--timeout must be whole seconds"
batch=$(date -u +%Y%m%dT%H%M%SZ)

# ---------------------------------------------------------------- manifest

declare -A C_STATUS C_BIN C_CUDD C_DETAIL C_KNOB C_DIR C_BY
while IFS=$'\t' read -r name status variant backend knob sb cb detail; do
  [ -n "$name" ] || continue
  C_STATUS[$name]=$status; C_BIN[$name]=$sb; C_CUDD[$name]=$cb
  C_DETAIL[$name]=$detail; C_KNOB[$name]=$knob
  C_DIR[$name]=${sb%/benchmark/sift/sylvan_bench}
  C_BY[$variant/$backend]=$name
done < <(python3 "$S/manifest.py" configs "$manifest")
[ ${#C_STATUS[@]} -gt 0 ] || die "no configurations in $manifest"
flag_of() { python3 "$S/manifest.py" runtime-flag "$manifest" "$1"; }

if [ -z "$cudd" ]; then
  for n in "${!C_CUDD[@]}"; do
    if [ "${C_STATUS[$n]}" = ok ] && [ "${C_CUDD[$n]}" != - ]; then cudd=${C_CUDD[$n]}; break; fi
  done
fi
[ -z "$cudd" ] || [ -x "$cudd" ] || die "--cudd-bench $cudd is not executable"

# ---------------------------------------------------------------- output

if [ "$dry" = 0 ]; then
  mkdir -p "$out" || die "cannot create $out"
  if [ "$append" = 0 ] && compgen -G "$out/*.jsonl" > /dev/null; then
    die "$out already has rows; use another --out, or --append to add to them"
  fi
  out=$(cd "$out" && pwd)
  cp "$manifest" "$out/manifest.json"
fi
LOG="$out/run-headlines.log"

say() {   # say TEXT -- to the terminal and the log
  echo "$*"
  [ "$dry" = 1 ] || echo "$*" >> "$LOG"
}

jstr() { local s=$1; s=${s//\\/\\\\}; s=${s//\"/\\\"}; printf '"%s"' "$s"; }

# skip HEADLINE STANZA CELL CONFIG REASON DETAIL -- one line, and a row in
# skips.jsonl. On stderr, because config() runs in a command substitution.
skip() {
  echo "SKIP $2/$3: $6" >&2
  [ "$dry" = 1 ] || echo "SKIP $2/$3: $6" >> "$LOG"
  [ "$dry" = 1 ] || printf '{"batch":%s,"headline":%s,"stanza":%s,"cell":%s,"config_name":%s,"reason":%s,"detail":%s,"t":"%s"}\n' \
    "$(jstr "$batch")" "$(jstr "$1")" "$(jstr "$2")" "$(jstr "$3")" "$(jstr "$4")" \
    "$(jstr "$5")" "$(jstr "$6")" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$out/skips.jsonl"
}

# want STANZA HEADLINE -- whether --only selects this stanza
want() {
  [ -n "$only" ] || return 0
  [[ " $only " == *" $1 "* || " $only " == *" $2 "* ]]
}

# config HEADLINE STANZA VARIANT BACKEND -- print the configuration's build
# argument NAME=DIR, or say why it cannot run and return 1.
config() {
  local h=$1 st=$2 v=$3 b=$4 name
  name=${C_BY[$v/$b]:-}
  if [ -z "$name" ]; then
    local want_name="rel-$b"; [ "$v" = base ] || want_name="rel-$b-$v"
    skip "$h" "$st" "$b" "$want_name" not-built \
      "$want_name is not in the manifest; build it with build-matrix.sh --only $want_name"
    return 1
  fi
  if [ "${C_STATUS[$name]}" != ok ]; then
    local why=${C_DETAIL[$name]}
    [ "${C_STATUS[$name]}" != knob-missing ] || why="${C_KNOB[$name]} is not an option of this tree (CMake reported it unused)"
    skip "$h" "$st" "$b" "$name" "${C_STATUS[$name]}" "$name is ${C_STATUS[$name]}: $why; not run"
    return 1
  fi
  printf '%s=%s' "$name" "${C_DIR[$name]}"
}

# has_flag NAME FLAG -- whether the configuration's sylvan_bench --help lists FLAG
has_flag() {
  "${C_BIN[$1]}" --help 2>&1 | grep -qE -- "(^|[[:space:]])$2([[:space:]]|$)"
}

failed=0
# stanza HEADLINE STANZA FILE SCRIPT ARGS... -- run one run-*.sh into OUT/FILE
stanza() {
  local h=$1 st=$2 file=$3 script=$4; shift 4
  local -a cmd=("$S/$script" --inputs "$inputs" --timeout "$timeout" --out "$out/$file" "$@")
  local extra="\"headline\":\"$h\",\"stanza\":\"$st\",\"batch\":\"$batch\",\"quick\":$quick"
  say ""
  say "=== $st ($(date -u +%H:%M:%SZ)): $script -> $file"
  say "\$ SB_ROW_EXTRA='$extra' $(printf '%q ' "${cmd[@]}")"
  [ "$dry" = 0 ] || return 0
  if ! SB_ROW_EXTRA=$extra "${cmd[@]}" 2>&1 | tee -a "$LOG"; then
    say "FAILED $st (exit ${PIPESTATUS[0]})"
    failed=1
  fi
}

cudd_args=(); [ -z "$cudd" ] || cudd_args=(--cudd-bench "$cudd")

say "run-headlines $batch: builds $builds, out $out"
say "  workers [$workers], quick=$quick, timeout ${timeout}s, cudd_bench ${cudd:-(none)}"
say "  host $(uname -n) $(uname -sr), $ncpu cpus, load $(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null)"
say "  THP enabled: $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo '?')"
[ -n "$cudd" ] || say "  note: no cudd_bench; the CUDD reference columns will be empty"

# ---------------------------------------------------------------- H1

if want h1-timing h1; then
  bs=()
  for b in trie list llcht llchst; do
    c=$(config h1 h1-timing base "$b") && bs+=(--sylvan-build "$c")
  done
  if [ ${#bs[@]} -gt 0 ]; then
    stanza h1 h1-timing h1-timing-c880.jsonl run-timing.sh "${bs[@]}" "${cudd_args[@]}" \
      --cases "$C880" --reps "$r_c880" --workers "${workers// /,}"
    stanza h1 h1-timing h1-timing-queens.jsonl run-timing.sh "${bs[@]}" "${cudd_args[@]}" \
      --cases "$QUEENS" --reps "$r_queens" --workers "${workers// /,}"
  fi
fi

if want h1-stats h1; then
  sflag=$(flag_of stats)
  bs=()
  for b in trie list llcht llchst; do
    c=$(config h1 h1-stats stats "$b") || continue
    if ! has_flag "${c%%=*}" "$sflag"; then
      skip h1 h1-stats "$b" "${c%%=*}" flag-missing \
        "${c%%=*}: sylvan_bench --help does not list $sflag (runtime knob stats); not run"
      continue
    fi
    bs+=(--sylvan-build "$c")
  done
  if [ ${#bs[@]} -gt 0 ]; then
    stanza h1 h1-stats h1-stats.jsonl run-timing.sh "${bs[@]}" \
      --cases "$C880 $QUEENS" --reps 1 --workers "${wends// /,}" --sylvan-args "$sflag"
  fi
fi

# ---------------------------------------------------------------- H2

for b in list trie; do
  if want h2a h2; then
    if base=$(config h2 h2a base "$b") && alt=$(config h2 h2a striped "$b"); then
      stanza h2 h2a "h2a-region-$b.jsonl" run-timing.sh \
        --sylvan-build "$base" --sylvan-build "$alt" \
        --cases "$C880" --reps "$r_c880" --workers "${workers// /,}"
    fi
  fi
  if want h2b h2; then
    nflag=$(flag_of no_batch)
    if base=$(config h2 h2b base "$b"); then
      if has_flag "${base%%=*}" "$nflag"; then
        stanza h2 h2b "h2b-batch-$b.jsonl" run-ablation.sh --sylvan-build "$base" \
          --variant batched= --variant "nobatch=$nflag" \
          --cases "$C880" --reps "$r_c880" --workers "${workers// /,}"
      else
        skip h2 h2b "$b" "${base%%=*}" flag-missing \
          "${base%%=*}: sylvan_bench --help does not list $nflag (runtime knob no_batch); not run"
      fi
    fi
  fi
  if want h2c h2; then
    if ph=$(config h2 h2c phases "$b"); then
      stanza h2 h2c "h2c-phases-$b.jsonl" run-timing.sh --sylvan-build "$ph" \
        --cases "$C880" --reps "$r_c880" --workers "${workers// /,}"
      if [ "$dry" = 0 ] && [ -f "$out/h2c-phases-$b.jsonl" ] &&
         ! grep -q '"sift_phases"' "$out/h2c-phases-$b.jsonl"; then
        say "WARNING h2c/$b: ${ph%%=*} printed no SIFT-PHASES line; the phase table will be empty"
      fi
    fi
  fi
done

# ---------------------------------------------------------------- H3

h3_builds=()
if want h3-quality h3 || want h3-soundness h3 || want h3-fulllegs h3; then
  for b in trie list; do
    c=$(config h3 h3 base "$b") && h3_builds+=(--sylvan-build "$c")
  done
fi
if [ ${#h3_builds[@]} -gt 0 ] || want h3-seeds h3; then
  if want h3-quality h3 && [ ${#h3_builds[@]} -gt 0 ]; then
    stanza h3 h3-quality h3-quality.jsonl run-quality.sh "${h3_builds[@]}" "${cudd_args[@]}" \
      --cudd-defaults --cases "$QUALITY" --reps 1 --workers "${wq// /,}" \
      --sylvan-args "--verify 64" --cudd-args "--verify 64"
  fi
  if want h3-soundness h3 && [ ${#h3_builds[@]} -gt 0 ]; then
    stanza h3 h3-soundness h3-soundness.jsonl run-soundness.sh "${h3_builds[@]}" "${cudd_args[@]}" \
      --cases "$SOUND_CASES" --vectors "$SOUND_VECTORS" --reps "$r_sound" --workers "$wmin"
  fi
  if want h3-seeds h3; then
    # The 30-seed sweep runs on list only.
    sl=$(config h3 h3-seeds base list) &&
      stanza h3 h3-seeds h3-seeds.jsonl run-seeds.sh --sylvan-build "$sl" "${cudd_args[@]}" \
        --seeds "$SEEDS" --vectors "$SEED_VECTORS" --workers "$wmin"
  fi
  if want h3-fulllegs h3 && [ ${#h3_builds[@]} -gt 0 ]; then
    stanza h3 h3-fulllegs h3-fulllegs.jsonl run-ablation.sh "${h3_builds[@]}" "${cudd_args[@]}" \
      --variant bounded= --variant fulllegs=--full-legs \
      --cases "$LEGS_CASES" --reps "$r_legs" --workers "$wmin" \
      --sylvan-args "--verify 64" --cudd-args "--verify 64"
  fi
fi

# ---------------------------------------------------------------- H5

if want h5 h5; then
  thp=$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null)
  if [ "$thp" != madvise ]; then
    say "WARNING h5: transparent_hugepage/enabled is '${thp:-unknown}', not madvise: under 'always' every"
    say "  table gets huge pages and under 'never' none does, so rel-B against rel-B-nohuge measures"
    say "  nothing. Rows record the mode; set madvise for the measurement."
  fi
  for b in list trie; do
    if base=$(config h5 h5 base "$b") && alt=$(config h5 h5 nohuge "$b"); then
      stanza h5 h5 "h5-hugepages-$b.jsonl" run-timing.sh \
        --sylvan-build "$base" --sylvan-build "$alt" \
        --cases "$C880" --reps "$r_c880" --workers "${wends// /,}"
      stanza h5 h5 "h5-hugepages-$b-queens.jsonl" run-timing.sh \
        --sylvan-build "$base" --sylvan-build "$alt" \
        --cases "$QUEENS" --reps "$r_queens" --workers "${wends// /,}"
    fi
  done
fi

say ""
say "run-headlines $batch done; summarise with: $S/headlines.py --in $out --out $out/tables"
exit $failed
