#!/usr/bin/env bash
# build-matrix.sh -- configure and build every Sylvan configuration the
# headline experiments (sections H1..H5 of EXPERIMENTS.md) need, one Ninja
# Release tree each, and write a manifest.
#
#   build-matrix.sh --src DIR --builds DIR [--cudd-dir DIR] [--cc gcc|clang]
#                   [--only LIST] [--lace-dir DIR] [--src-rev REV]
#                   [--cudd-rev REV] [--jobs N] [--clean] [--dry-run] [--list]
#
# A configuration is a nodes table and a variant, named
#   rel-<backend>              the default configuration
#   rel-<backend>-<variant>    the same with one ablation knob switched
# and is built in BUILDS/<name>, with -DCMAKE_BUILD_TYPE=Release,
# -DSYLVAN_NODES_TABLE=<backend>, -DSYLVAN_BUILD_SIFT_BENCH=ON and, given
# --cudd-dir, -DCUDD_DIR so that cudd_bench is built too. The variants, the
# CMake definition each one switches, and the backends and headlines each is
# for are the OPTION TABLE below: the one place where a knob's name is written.
#
# KNOB-MISSING. A variant exists to switch one CMake option. When the tree
# does not have that option, CMake says so ("Manually-specified variables were
# not used by the project") and leaves it in the cache as UNINITIALIZED; the
# configuration is then marked "knob-missing" in the manifest and NOT built
# (any binary left from an earlier build there is removed), so that nothing
# can measure it as if the ablation were in effect. That is not an error.
#
# THE MANIFEST. BUILDS/manifest.json records the source commit, compiler,
# CMake and Lace and CUDD versions, the option table, and per configuration
# its CMake arguments, the cache values of every knob (null where the tree
# has no such option), its status, its binaries and the flags its sylvan_bench
# --help lists. Each configuration's entry is also kept as
# BUILDS/<name>/build-matrix.json, so --only rebuilds merge into the manifest
# instead of replacing it, and each binary gets a BINARY.row.json whose
# members lib.sh adds to every row it runs (config_name, knobs, ...).
#
# Options:
#   --src DIR        the Sylvan source tree (a checkout or a `git archive` export)
#   --builds DIR     where the build trees and manifest.json go
#   --cudd-dir DIR   a CUDD tree configured and built in place (see README.md)
#   --cc CC          C compiler, gcc (default) or clang; one per --builds DIR
#   --only LIST      configurations to build: names (rel-trie), globs
#                    ('rel-*-stats'), variants (stats), headlines (h1, h2,
#                    h3, h5) or the recommendation-floor sweep (s3), comma-
#                    or space-separated; default all
#   --lace-dir DIR   a Lace 2.4.0 source tree, to build without fetching it
#   --src-rev REV    the commit an exported --src was made from (`git
#                    get-tar-commit-id < export.tar` prints it)
#   --cudd-rev REV   the same for --cudd-dir (the pinned CUDD 3.0.0 import is
#                    f54f533)
#   --jobs N         parallel compile jobs (default: Ninja's choice)
#   --clean          remove each build tree before configuring it
#   --dry-run        print the commands, run nothing
#   --list           print the configurations and exit
set -uo pipefail
SCRIPTS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# lib.sh for sb_git_rev and sb_die; it runs nothing when sourced.
source "$SCRIPTS/lib.sh"

# ------------------------------------------------------------ OPTION TABLE
#
# One line per variant (a variant may take several lines, for backends that
# serve different headlines):
#   VARIANT | CMAKE DEFINITIONS | KNOB | BACKENDS | HEADLINES | WHAT
# KNOB is the CMake cache variable the variant exists to switch (empty for
# the base). If a knob is renamed in the library, change it here and nowhere
# else: run-headlines.sh refers to variants by VARIANT, and every row records
# the knob's name and value from the manifest.
MATRIX_TABLE='
base       |                                     |                             | list trie              | h1 h2 h3 h5 | the configuration the paper measures
base       |                                     |                             | llcht llchst           | h1          | the configuration the paper measures
base       |                                     |                             | llht                   | h6          | the table that cannot reorder, for the build-only comparison (sylvan_bench --no-sift)
stats      | -DSYLVAN_STATS=ON                   | SYLVAN_STATS                | list trie llcht llchst | h1          | per-step index counters (sylvan_bench --stats)
striped    | -DSYLVAN_REGION_STRIPED_START=ON    | SYLVAN_REGION_STRIPED_START | list trie llcht llchst | h2          | striped per-worker region starts instead of the shared cursor
phases     | -DSYLVAN_SIFT_PHASE_TIMING=ON       | SYLVAN_SIFT_PHASE_TIMING    | list trie llcht llchst | h2          | a SIFT-PHASES {json} line on stderr after each sift
nohuge     | -DSYLVAN_HUGEPAGES=OFF              | SYLVAN_HUGEPAGES            | list trie llcht llchst | h5          | no MADV_HUGEPAGE advice on the tables
profile | -DSYLVAN_STATS=ON -DSYLVAN_SIFT_PHASE_TIMING=ON | SYLVAN_STATS | list trie llcht llchst | h1 | counters and SIFT-PHASES in one run, for ns per index step (never timed)
floor1024  | -DSYLVAN_SIFT_RECOMMEND_FLOOR=1024  | SYLVAN_SIFT_RECOMMEND_FLOOR | list trie              | s3          | sift recommendation floor 1024 live nodes instead of 4096
floor16384 | -DSYLVAN_SIFT_RECOMMEND_FLOOR=16384 | SYLVAN_SIFT_RECOMMEND_FLOOR | list trie              | s3          | sift recommendation floor 16384 live nodes instead of 4096
hwloc      | -DLACE_USE_HWLOC=ON                 | LACE_USE_HWLOC              | list trie              | h2          | Lace pins worker i to hwloc core i; overrides an outer taskset, so never combine with SIFT_PIN; needs libhwloc-dev
'
# Runtime knobs: sylvan_bench flags the headlines switch. A stanza checks the
# build's `sylvan_bench --help` for the flag and skips its cell when it is not
# listed.
#   NAME | FLAG | HEADLINES | WHAT
RUNTIME_TABLE='
no_batch | --no-batch | h2 | single-node table primitives instead of the batched ones (sylvan_reorder_set_batched(0))
stats    | --stats    | h1 | sylvan_stats_report around the build and the sift, on stderr
'
# Cache variables recorded for every configuration besides the knobs above.
RECORD_VARS="CMAKE_BUILD_TYPE CMAKE_C_COMPILER CMAKE_C_FLAGS CMAKE_C_FLAGS_RELEASE
SYLVAN_NODES_TABLE SYLVAN_NATIVE_OPT SYLVAN_USE_MMAP CUDD_DIR FETCHCONTENT_SOURCE_DIR_LACE"
# Every configuration gets these.
BASE_ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DSYLVAN_BUILD_SIFT_BENCH=ON
           -DBUILD_TESTING=OFF -DSYLVAN_BUILD_EXAMPLES=OFF -DSYLVAN_BUILD_CPP=OFF)

die() { echo "build-matrix.sh: $*" >&2; exit 2; }
trim() { local s=$1; s=${s#"${s%%[![:space:]]*}"}; s=${s%"${s##*[![:space:]]}"}; printf '%s' "$s"; }

# ------------------------------------------------------------ the matrix

CFG_NAME=(); CFG_BACKEND=(); CFG_VARIANT=(); CFG_DEFS=(); CFG_KNOB=(); CFG_HEAD=(); CFG_WHAT=()
ALL_KNOBS=""
while IFS='|' read -r variant defs knob backends heads what; do
  variant=$(trim "$variant"); [ -n "$variant" ] || continue
  defs=$(trim "$defs"); knob=$(trim "$knob"); heads=$(trim "$heads"); what=$(trim "$what")
  [ -z "$knob" ] || ALL_KNOBS+=" $knob"
  for b in $backends; do
    if [ "$variant" = base ]; then n="rel-$b"; else n="rel-$b-$variant"; fi
    CFG_NAME+=("$n"); CFG_BACKEND+=("$b"); CFG_VARIANT+=("$variant"); CFG_DEFS+=("$defs")
    CFG_KNOB+=("$knob"); CFG_HEAD+=("$heads"); CFG_WHAT+=("$what")
  done
done <<< "$MATRIX_TABLE"

# ------------------------------------------------------------ options

src=""; builds=""; cudd=""; cc=gcc; only=""; lace=""; src_rev=""; cudd_rev=""
jobs=""; clean=0; dry=0; list=0
while [ $# -gt 0 ]; do
  case "$1" in
    --src)      [ $# -ge 2 ] || die "$1 needs a value"; src=$2; shift 2 ;;
    --builds)   [ $# -ge 2 ] || die "$1 needs a value"; builds=$2; shift 2 ;;
    --cudd-dir) [ $# -ge 2 ] || die "$1 needs a value"; cudd=$2; shift 2 ;;
    --cc)       [ $# -ge 2 ] || die "$1 needs a value"; cc=$2; shift 2 ;;
    --only)     [ $# -ge 2 ] || die "$1 needs a value"; only+=" ${2//,/ }"; shift 2 ;;
    --lace-dir) [ $# -ge 2 ] || die "$1 needs a value"; lace=$2; shift 2 ;;
    --src-rev)  [ $# -ge 2 ] || die "$1 needs a value"; src_rev=$2; shift 2 ;;
    --cudd-rev) [ $# -ge 2 ] || die "$1 needs a value"; cudd_rev=$2; shift 2 ;;
    --jobs)     [ $# -ge 2 ] || die "$1 needs a value"; jobs=$2; shift 2 ;;
    --clean)    clean=1; shift ;;
    --dry-run)  dry=1; shift ;;
    --list)     list=1; shift ;;
    -h|--help)  sed -n '2,/^[^#]/{/^#/s/^# \{0,1\}//p}' "$0"; exit 0 ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
done

selected() {   # selected INDEX -- whether --only picks configuration INDEX
  local i=$1 t
  [ -n "$(trim "$only")" ] || return 0
  for t in $only; do
    # shellcheck disable=SC2053  # a glob is intended
    [[ "${CFG_NAME[$i]}" == $t ]] && return 0
    [ "${CFG_VARIANT[$i]}" = "$t" ] && return 0
    if [[ "$t" =~ ^[hs][0-9]$ ]] && [[ " ${CFG_HEAD[$i]} " == *" $t "* ]]; then return 0; fi
  done
  return 1
}

SEL=()
for i in "${!CFG_NAME[@]}"; do selected "$i" && SEL+=("$i"); done

if [ "$list" = 1 ]; then
  printf '%-22s %-8s %-10s %-37s %s\n' NAME BACKEND VARIANT DEFINITIONS HEADLINES
  for i in "${SEL[@]}"; do
    printf '%-22s %-8s %-10s %-37s %s\n' "${CFG_NAME[$i]}" "${CFG_BACKEND[$i]}" \
      "${CFG_VARIANT[$i]}" "${CFG_DEFS[$i]:--}" "${CFG_HEAD[$i]}"
  done
  exit 0
fi

[ -n "$src" ] || die "--src DIR is required"
[ -n "$builds" ] || die "--builds DIR is required"
[ -f "$src/CMakeLists.txt" ] || die "--src $src has no CMakeLists.txt"
[ ${#SEL[@]} -gt 0 ] || die "--only '$(trim "$only")' selects no configuration (see --list)"
[[ -z "$jobs" || "$jobs" =~ ^[1-9][0-9]*$ ]] || die "--jobs must be a positive number"
command -v cmake > /dev/null || die "cmake not found"
command -v ninja > /dev/null || die "ninja not found"
command -v python3 > /dev/null || die "python3 not found (it writes the manifest)"
cc_path=$(command -v "$cc") || die "compiler '$cc' not found"
src=$(cd "$src" && pwd)
if [ -n "$cudd" ]; then
  [ -d "$cudd" ] || die "--cudd-dir $cudd is not a directory"
  cudd=$(cd "$cudd" && pwd)
  [ -f "$cudd/cudd/.libs/libcudd.a" ] ||
    echo "warning: $cudd/cudd/.libs/libcudd.a is missing; cudd_bench will not be built" >&2
fi
if [ -n "$lace" ]; then
  [ -f "$lace/CMakeLists.txt" ] || die "--lace-dir $lace has no CMakeLists.txt"
  lace=$(cd "$lace" && pwd)
fi

# The source revision: from git, or as given for an exported tree.
read -r src_git src_dirty <<< "$(sb_git_rev "$src")"
src_rev_from=git
if [ "$src_git" = unknown ]; then
  if [ -n "$src_rev" ]; then src_git=$src_rev; src_dirty=null; src_rev_from=given
  else src_rev_from=unknown
    echo "warning: $src is not a git checkout and no --src-rev was given; the commit is recorded as unknown" >&2
  fi
elif [ -n "$src_rev" ] && [[ "$src_git" != "$src_rev"* ]]; then
  die "--src-rev $src_rev, but $src is at $src_git"
fi
cudd_git=""
if [ -n "$cudd" ]; then
  read -r cudd_git _ <<< "$(sb_git_rev "$cudd")"
  [ "$cudd_git" != unknown ] || cudd_git=$cudd_rev
fi

mkdir -p "$builds" || die "cannot create $builds"
builds=$(cd "$builds" && pwd)
cc_version=$("$cc" --version 2>/dev/null | head -1)

export BM_SRC="$src" BM_SRC_GIT="$src_git" BM_SRC_DIRTY="$src_dirty" BM_SRC_REV_FROM="$src_rev_from"
export BM_CC="$cc" BM_CC_PATH="$cc_path" BM_CC_VERSION="$cc_version"
export BM_CUDD_DIR="$cudd" BM_CUDD_GIT="$cudd_git" BM_LACE_DIR="$lace"
export BM_RECORD_VARS="$RECORD_VARS" BM_ALL_KNOBS="$ALL_KNOBS"
export BM_MATRIX_TABLE="$MATRIX_TABLE" BM_RUNTIME_TABLE="$RUNTIME_TABLE"

echo "build-matrix: src $src at $src_git ($src_rev_from), $cc_version"
echo "build-matrix: ${#SEL[@]} configuration(s) into $builds"

# unused_vars LOG -- the variables CMake reported as not used, one per line.
unused_vars() {
  awk '/Manually-specified variables were not used by the project/ { on = 1; next }
       on && /^[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*$/ { gsub(/[[:space:]]/, ""); print; next }
       on && /^[[:space:]]*$/ { next }
       on { on = 0 }' "$1" | sort -u
}

failed=0
for i in "${SEL[@]}"; do
  name=${CFG_NAME[$i]}; dir="$builds/$name"; knob=${CFG_KNOB[$i]}
  args=("${BASE_ARGS[@]}" "-DSYLVAN_NODES_TABLE=${CFG_BACKEND[$i]}" "-DCMAKE_C_COMPILER=$cc")
  [ -z "$cudd" ] || args+=("-DCUDD_DIR=$cudd")
  [ -z "$lace" ] || args+=("-DFETCHCONTENT_SOURCE_DIR_LACE=$lace")
  read -r -a defs <<< "${CFG_DEFS[$i]}"
  args+=("${defs[@]}")
  bargs=(--build "$dir" --target sylvan_bench)
  [ -z "$jobs" ] || bargs+=(-j "$jobs")

  if [ "$dry" = 1 ]; then
    echo "[$name]"
    [ "$clean" = 0 ] || echo "  rm -rf $dir"
    echo "  cmake -S $src -B $dir ${args[*]}"
    echo "  cmake ${bargs[*]} [cudd_bench]"
    continue
  fi

  # A tree configured with another compiler is started afresh.
  if [ -f "$dir/build-matrix.json" ] && ! grep -q "\"cc\": *\"$cc\"" "$dir/build-matrix.json"; then
    echo "  $name: was built with another compiler; starting it afresh"
    rm -rf "$dir"
  fi
  [ "$clean" = 0 ] || rm -rf "$dir"
  mkdir -p "$dir"
  status=ok; detail=""; built_at=""; secs=""
  configured_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  t0=$(date +%s)
  reconf=0; [ ! -f "$dir/CMakeCache.txt" ] || reconf=1
  if ! cmake -S "$src" -B "$dir" "${args[@]}" > "$dir/configure.log" 2>&1; then
    status=configure-failed; detail="cmake failed; see $dir/configure.log"
  fi
  unused=$(unused_vars "$dir/configure.log")
  if [ "$status" = ok ] && [ -n "$knob" ]; then
    # The warning is the test. A reconfigure need not repeat it, so there an
    # entry CMake never typed (no option() claimed it) counts as well.
    if grep -qx "$knob" <<< "$unused" ||
       { [ "$reconf" = 1 ] && grep -q "^$knob:UNINITIALIZED=" "$dir/CMakeCache.txt"; }; then
      status=knob-missing
      detail="CMake did not use $knob: this tree has no such option, so the ablation would not be in effect"
      rm -f "$dir/benchmark/sift/sylvan_bench" "$dir/benchmark/sift/cudd_bench" \
            "$dir/benchmark/sift/"*.row.json
    fi
  fi
  if [ "$status" = ok ]; then
    has_cudd=0
    grep -q "Sift harness: building cudd_bench" "$dir/configure.log" && has_cudd=1
    [ "$has_cudd" = 0 ] || bargs+=(--target cudd_bench)
    if cmake "${bargs[@]}" > "$dir/build.log" 2>&1; then
      built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    else
      status=build-failed; detail="build failed; see $dir/build.log"
    fi
  fi
  secs=$(( $(date +%s) - t0 ))

  BM_NAME=$name BM_DIR=$dir BM_BACKEND=${CFG_BACKEND[$i]} BM_VARIANT=${CFG_VARIANT[$i]} \
  BM_KNOB=$knob BM_DEFS=${CFG_DEFS[$i]} BM_HEADLINES=${CFG_HEAD[$i]} BM_WHAT=${CFG_WHAT[$i]} \
  BM_STATUS=$status BM_DETAIL=$detail BM_UNUSED=$unused BM_ARGS=$(printf '%s\n' "${args[@]}") \
  BM_CONFIGURED_AT=$configured_at BM_BUILT_AT=$built_at BM_SECONDS=$secs \
    python3 "$SCRIPTS/manifest.py" entry || { status=manifest-failed; failed=1; }

  case "$status" in
    ok)           printf '  %-22s ok            %4ss  %s\n' "$name" "$secs" "$dir" ;;
    knob-missing) printf '  %-22s KNOB-MISSING  not built: %s\n' "$name" "$detail" ;;
    *)            printf '  %-22s %-13s %s\n' "$name" "${status^^}" "$detail"; failed=1 ;;
  esac
done

[ "$dry" = 1 ] && exit 0
python3 "$SCRIPTS/manifest.py" assemble "$builds" || failed=1
echo "build-matrix: manifest $builds/manifest.json"
exit $failed
