# Shared plumbing for the sift benchmark scripts. Source it; do not run it.
#
# Every script takes the same options, parsed by sb_parse_common:
#
#   --sylvan-build NAME=DIR  a Sylvan build tree configured with
#                            -DSYLVAN_BUILD_SIFT_BENCH=ON; repeatable. A bare
#                            DIR is named after the nodes table it was built with.
#   --cudd-bench PATH        the cudd_bench binary (omit to run Sylvan only)
#   --inputs DIR             the directory holding C880.blif (fetch-inputs.sh)
#   --out FILE               JSONL file the rows are appended to
#   --reps N                 repetitions of every variant
#   --workers LIST           worker counts for Sylvan, e.g. 1,4,8
#   --timeout S              wall clock cap per run, 0 for none
#   --cases LIST             cases to run instead of the script's defaults
#   --sylvan-args STR        extra arguments for every sylvan_bench run
#   --cudd-args STR          extra arguments for every cudd_bench run
#
# Options the script does not know are left in SB_REST for it to parse.
#
# THE ROWS. Each run appends one JSON object per line to --out: the harness's
# own RESULT object, prefixed with what the harness cannot know about itself --
# the experiment and variant, the repetition, the Sylvan build and the git
# commit of its source tree, the host, the load average just before the run,
# and UTC timestamps around it. A run that fails or times out still appends a
# row, with "failed":1 and the exit code, so that a gap in the data is visible.
# Every command and everything it printed on stderr goes to FILE.log.
#
# INTERLEAVING. sb_interleave runs every variant once per repetition, in the
# same order, before any variant runs again: A,B,A,B rather than A,A,B,B. If
# the machine drifts during a batch, it drifts across all variants together
# instead of biasing whichever happened to run last.

SB_LIB_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=cases.sh
source "$SB_LIB_DIR/cases.sh"

SB_BUILDS=()        # NAME=DIR, as given
SB_CUDD=""
SB_INPUTS=""
SB_OUT=""
SB_REPS=""
SB_WORKERS=""
SB_TIMEOUT=""
SB_CASES=""
SB_SYLVAN_ARGS=""
SB_CUDD_ARGS=""
SB_REST=()
SB_EXP=${SB_EXP:-unnamed}

sb_die() { echo "$(basename "$0"): $*" >&2; exit 2; }

# sb_parse_common "$@" -- see the top of this file.
sb_parse_common() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --sylvan-build) [ $# -ge 2 ] || sb_die "$1 needs a value"; SB_BUILDS+=("$2"); shift 2 ;;
      --cudd-bench)   [ $# -ge 2 ] || sb_die "$1 needs a value"; SB_CUDD=$2; shift 2 ;;
      --inputs)       [ $# -ge 2 ] || sb_die "$1 needs a value"; SB_INPUTS=$2; shift 2 ;;
      --out)          [ $# -ge 2 ] || sb_die "$1 needs a value"; SB_OUT=$2; shift 2 ;;
      --reps)         [ $# -ge 2 ] || sb_die "$1 needs a value"; SB_REPS=$2; shift 2 ;;
      --workers)      [ $# -ge 2 ] || sb_die "$1 needs a value"; SB_WORKERS=$2; shift 2 ;;
      --timeout)      [ $# -ge 2 ] || sb_die "$1 needs a value"; SB_TIMEOUT=$2; shift 2 ;;
      --cases)        [ $# -ge 2 ] || sb_die "$1 needs a value"; SB_CASES=$2; shift 2 ;;
      --sylvan-args)  [ $# -ge 2 ] || sb_die "$1 needs a value"; SB_SYLVAN_ARGS=$2; shift 2 ;;
      --cudd-args)    [ $# -ge 2 ] || sb_die "$1 needs a value"; SB_CUDD_ARGS=$2; shift 2 ;;
      -h|--help)      sb_usage; exit 0 ;;
      *)              SB_REST+=("$1"); shift ;;
    esac
  done
}

# The script's own usage text is the comment block at its top.
sb_usage() {
  sed -n '2,/^[^#]/{/^#/s/^# \{0,1\}//p}' "$0"
  echo
  sed -n '3,/^$/{s/^# \{0,1\}//p}' "$SB_LIB_DIR/lib.sh"
}

# sb_defaults REPS WORKERS TIMEOUT CASES -- fill in what was not given, then
# check the common options and resolve every build.
sb_defaults() {
  [ -n "$SB_REPS" ]    || SB_REPS=$1
  [ -n "$SB_WORKERS" ] || SB_WORKERS=$2
  [ -n "$SB_TIMEOUT" ] || SB_TIMEOUT=$3
  [ -n "$SB_CASES" ]   || SB_CASES=$4
  [ -n "$SB_OUT" ]     || SB_OUT="./$SB_EXP.jsonl"
  local -a words
  read -r -a words <<< "${SB_CASES//,/ }";   SB_CASES="${words[*]}"
  read -r -a words <<< "${SB_WORKERS//,/ }"; SB_WORKERS="${words[*]}"

  [[ "$SB_REPS" =~ ^[0-9]+$ ]] || sb_die "--reps must be a number"
  [[ "$SB_TIMEOUT" =~ ^[0-9]+$ ]] || sb_die "--timeout must be whole seconds"
  local w
  for w in $SB_WORKERS; do [[ "$w" =~ ^[1-9][0-9]*$ ]] || sb_die "bad worker count '$w'"; done
  [ ${#SB_BUILDS[@]} -gt 0 ] || sb_die "give at least one --sylvan-build NAME=DIR"
  if [ -n "$SB_CUDD" ]; then
    [ -x "$SB_CUDD" ] || sb_die "--cudd-bench $SB_CUDD is not an executable"
  fi
  if [ -n "$SB_INPUTS" ]; then
    [ -d "$SB_INPUTS" ] || sb_die "--inputs $SB_INPUTS is not a directory"
    SB_INPUTS=$(cd "$SB_INPUTS" && pwd)
  fi
  local c
  for c in $SB_CASES; do sb_case_args "$c" || exit 2; done

  mkdir -p "$(dirname "$SB_OUT")"
  SB_LOG="$SB_OUT.log"
  sb_resolve_builds
}

# ---------------------------------------------------------------- the builds

SB_B_NAME=(); SB_B_DIR=(); SB_B_BIN=(); SB_B_BACKEND=(); SB_B_GIT=(); SB_B_DIRTY=()

# sb_git_rev SRC -- prints "<commit> <dirty>" for a source tree, where dirty is
# 1, 0, or null when it cannot be told. A worktree checked out on Windows and
# built under WSL names its git directory by a Windows path, which git under
# WSL cannot follow; that case is translated, and its dirtiness left unknown
# because the two sides disagree about line endings.
sb_git_rev() {
  local src=$1 rev dirty gd
  if rev=$(git -C "$src" rev-parse HEAD 2>/dev/null); then
    if [ -n "$(git -C "$src" status --porcelain --untracked-files=no 2>/dev/null | head -1)" ]; then
      dirty=1
    else
      dirty=0
    fi
    echo "$rev $dirty"
    return
  fi
  gd="$src/.git"
  if [ -f "$gd" ]; then
    gd=$(sed -n 's/^gitdir: //p' "$gd" | tr -d '\r')
    if command -v wslpath > /dev/null 2>&1 && [[ "$gd" =~ ^[A-Za-z]: ]]; then
      gd=$(wslpath -u "$gd" 2>/dev/null)
    fi
  fi
  if rev=$(git --git-dir="$gd" rev-parse HEAD 2>/dev/null); then
    echo "$rev null"
  else
    echo "unknown null"
  fi
}

sb_resolve_builds() {
  local spec name dir cache src backend gitinfo i
  for spec in "${SB_BUILDS[@]}"; do
    if [[ "$spec" == *=* ]]; then name=${spec%%=*}; dir=${spec#*=}; else name=""; dir=$spec; fi
    [ -d "$dir" ] || sb_die "build directory $dir does not exist"
    dir=$(cd "$dir" && pwd)
    cache="$dir/CMakeCache.txt"
    [ -f "$cache" ] || sb_die "$dir is not a CMake build tree (no CMakeCache.txt)"
    backend=$(sed -n 's/^SYLVAN_NODES_TABLE:[A-Z]*=//p' "$cache" | tr 'A-Z' 'a-z')
    src=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache")
    [ -n "$name" ] || name=${backend:-sylvan}
    [[ "$name" =~ ^[A-Za-z0-9._+-]+$ ]] || sb_die "build name '$name' must be [A-Za-z0-9._+-]"
    for i in "${SB_B_NAME[@]}"; do
      [ "$i" != "$name" ] || sb_die "two builds are named '$name'; use NAME=DIR"
    done
    [ -x "$dir/benchmark/sift/sylvan_bench" ] ||
      sb_die "$dir has no benchmark/sift/sylvan_bench; configure it with -DSYLVAN_BUILD_SIFT_BENCH=ON and build"
    gitinfo=$(sb_git_rev "$src")
    SB_B_NAME+=("$name"); SB_B_DIR+=("$dir"); SB_B_BIN+=("$dir/benchmark/sift/sylvan_bench")
    SB_B_BACKEND+=("$backend"); SB_B_GIT+=("${gitinfo% *}"); SB_B_DIRTY+=("${gitinfo#* }")
  done
}

# ----------------------------------------------------------------- variants

SB_V_LABEL=(); SB_V_BUILD=(); SB_V_TOOL=(); SB_V_WORKERS=(); SB_V_CMD=()

# sb_add_variant LABEL BUILD-INDEX|cudd WORKERS COMMAND...
# BUILD-INDEX is an index into the resolved builds; the case arguments and the
# script's common arguments are appended to COMMAND at run time.
sb_add_variant() {
  local label=$1 build=$2 workers=$3; shift 3
  SB_V_LABEL+=("$label"); SB_V_BUILD+=("$build"); SB_V_WORKERS+=("$workers")
  if [ "$build" = cudd ]; then SB_V_TOOL+=(cudd); else SB_V_TOOL+=(sylvan); fi
  SB_V_CMD+=("$(printf '%q ' "$@")")
}

# sb_add_sylvan_variants [--tag TAG] [ARGS...] -- one variant per build and
# worker count, labelled BUILD[TAG]-pW, running
#   sylvan_bench --workers W --threshold 0 ARGS --sylvan-args...
# The harness takes the last of a repeated option, so ARGS may override the
# threshold.
sb_add_sylvan_variants() {
  local i w tag="" extra
  if [ "${1:-}" = --tag ]; then tag=$2; shift 2; fi
  read -r -a extra <<< "$SB_SYLVAN_ARGS"
  for i in "${!SB_B_NAME[@]}"; do
    for w in $SB_WORKERS; do
      sb_add_variant "${SB_B_NAME[$i]}$tag-p$w" "$i" "$w" \
        "${SB_B_BIN[$i]}" --workers "$w" --threshold 0 "$@" "${extra[@]}"
    done
  done
}

# sb_add_cudd_variant [LABEL [ARGS...]] -- the CUDD reference, when given.
sb_add_cudd_variant() {
  [ -n "$SB_CUDD" ] || return 0
  local label=cudd extra
  if [ $# -gt 0 ]; then label=$1; shift; fi
  read -r -a extra <<< "$SB_CUDD_ARGS"
  sb_add_variant "$label" cudd 1 "$SB_CUDD" "$@" "${extra[@]}"
}

# ------------------------------------------------------------------ running

sb_jstr() {
  local s=$1
  s=${s//\\/\\\\}; s=${s//\"/\\\"}
  s=${s//$'\t'/\\t}; s=${s//$'\n'/\\n}; s=${s//$'\r'/\\r}
  printf '"%s"' "$s"
}

sb_now_us() {
  if [ -n "${EPOCHREALTIME:-}" ]; then
    local t=${EPOCHREALTIME/[.,]/}
    echo "$t"
  else
    echo "$(( $(date +%s) * 1000000 ))"
  fi
}

sb_loadavg() {
  if [ -r /proc/loadavg ]; then
    local l1 l5 l15 rest
    read -r l1 l5 l15 rest < /proc/loadavg
    echo "$l1 $l5 $l15"
  else
    uptime | sed 's/.*load averages*: *//; s/,/ /g' | awk '{print $1, $2, $3}'
  fi
}

# sb_quiet LABEL -- note in the log, and on the terminal, how busy the box is.
sb_quiet() {
  {
    echo "### $(date -u +%Y-%m-%dT%H:%M:%SZ) [$SB_EXP] $*"
    echo "    load average: $(sb_loadavg)"
    if ps -eo pcpu=,comm= --sort=-pcpu > /dev/null 2>&1; then
      echo "    busiest: $(ps -eo pcpu=,comm= --sort=-pcpu | head -3 | tr -s ' ' | tr '\n' '|')"
    fi
  } | tee -a "$SB_LOG"
}

# sb_header -- record the batch: the builds, the CUDD binary, the settings.
sb_header() {
  local i
  {
    echo "### $(date -u +%Y-%m-%dT%H:%M:%SZ) [$SB_EXP] $0"
    echo "    host: $(uname -n) $(uname -sr), $(nproc 2>/dev/null || echo '?') cpus"
    for i in "${!SB_B_NAME[@]}"; do
      echo "    build ${SB_B_NAME[$i]}: ${SB_B_DIR[$i]} backend=${SB_B_BACKEND[$i]} git=${SB_B_GIT[$i]} dirty=${SB_B_DIRTY[$i]}"
    done
    echo "    cudd_bench: ${SB_CUDD:-(none)}"
    echo "    reps=$SB_REPS workers=[$SB_WORKERS] timeout=${SB_TIMEOUT}s cases=[$SB_CASES]"
    echo "    out: $SB_OUT"
  } | tee -a "$SB_LOG"
}

# sb_run_variant V CASE REP [ARGS...] -- run variant V on CASE, append its row.
sb_run_variant() {
  local v=$1 case=$2 rep=$3; shift 3
  local -a cmd
  eval "cmd=(${SB_V_CMD[$v]})"
  sb_case_args "$case" || return 0
  cmd+=("${SB_CASE_ARGV[@]}" "$@")

  local b=${SB_V_BUILD[$v]} build git dirty bbackend
  if [ "$b" = cudd ]; then
    build=cudd; git=""; dirty=null; bbackend=cudd
  else
    build=${SB_B_NAME[$b]}; git=${SB_B_GIT[$b]}; dirty=${SB_B_DIRTY[$b]}; bbackend=${SB_B_BACKEND[$b]}
  fi

  local load l1 l5 l15
  load=$(sb_loadavg); read -r l1 l5 l15 <<< "$load"
  local t0 t1 ts0 ts1
  ts0=$(date -u +%Y-%m-%dT%H:%M:%SZ); t0=$(sb_now_us)

  echo "\$ $(printf '%q ' "${cmd[@]}")" >> "$SB_LOG"
  local out rc
  if [ "$SB_TIMEOUT" -gt 0 ] && command -v timeout > /dev/null 2>&1; then
    out=$(timeout "$SB_TIMEOUT" "${cmd[@]}" 2>> "$SB_LOG"); rc=$?
  else
    out=$("${cmd[@]}" 2>> "$SB_LOG"); rc=$?
  fi

  t1=$(sb_now_us); ts1=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  local wall_ms=$(( (t1 - t0) / 1000 ))

  local prefix
  prefix="\"exp\":$(sb_jstr "$SB_EXP"),\"cfg\":$(sb_jstr "${SB_V_LABEL[$v]}"),\"rep\":$rep"
  prefix+=",\"build\":$(sb_jstr "$build"),\"build_backend\":$(sb_jstr "$bbackend")"
  prefix+=",\"git\":$(sb_jstr "$git"),\"git_dirty\":$dirty"
  prefix+=",\"host\":$(sb_jstr "$(uname -n)"),\"load1\":${l1:-null},\"load5\":${l5:-null},\"load15\":${l15:-null}"
  prefix+=",\"t_start\":\"$ts0\",\"t_end\":\"$ts1\",\"wall_ms\":$wall_ms"

  local line
  line=$(printf '%s\n' "$out" | grep -m1 '^RESULT {' || true)
  if [ $rc -ne 0 ] || [ -z "$line" ]; then
    echo "    rc=$rc (FAILED)" >> "$SB_LOG"
    echo "{$prefix,\"failed\":1,\"rc\":$rc,\"tool\":\"${SB_V_TOOL[$v]}\",\"case\":$(sb_jstr "$case"),\"workers\":${SB_V_WORKERS[$v]}}" >> "$SB_OUT"
    printf '  %-12s %-14s rep %-2s FAILED rc=%s\n' "$case" "${SB_V_LABEL[$v]}" "$rep" "$rc"
    return 0
  fi
  line=${line#RESULT \{}
  echo "{$prefix,$line" >> "$SB_OUT"

  local na ss sw
  na=$(grep -o '"nodes_after":[0-9]*' <<< "$line" | cut -d: -f2)
  ss=$(grep -o '"sift_s":[-0-9.]*' <<< "$line" | cut -d: -f2)
  sw=$(grep -o '"swaps":[0-9]*' <<< "$line" | cut -d: -f2)
  printf '  %-12s %-14s rep %-2s nodes_after=%-8s swaps=%-6s sift_s=%s\n' \
    "$case" "${SB_V_LABEL[$v]}" "$rep" "$na" "$sw" "$ss"
}

# sb_interleave CASES REPS [ARGS...] -- for each case, REPS repetitions of
# every variant in turn (A,B,A,B), each run with ARGS appended. Repetitions
# are numbered from 1.
sb_interleave() {
  local cases=$1 reps=$2; shift 2
  local c rep v
  for c in $cases; do
    sb_quiet "case $c"
    for rep in $(seq 1 "$reps"); do
      for v in "${!SB_V_LABEL[@]}"; do
        sb_run_variant "$v" "$c" "$rep" "$@"
      done
    done
  done
}

sb_footer() {
  sb_quiet "done"
  echo "rows in $SB_OUT: $(wc -l < "$SB_OUT")"
}
