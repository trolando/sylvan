#!/usr/bin/env bash
# The cases the measurement scripts run, by name. Sourced by lib.sh; run it
# directly to list cases from the case table (see "As a command" below).
#
# A case name says everything about how the forest is built, so that a row's
# "case" field is enough to rebuild it:
#
#   STEM-file      every .outputs signal of STEM's circuit, under the file's
#                  own variable order (inputs, then latch outputs); for a
#                  fault tree, STEM is TREE.dfs or TREE.tdlr, the file
#                  aralia/STEM.blif, whose order is Storm-dft's
#   STEM-rev       the same under the reversed order
#   STEM-rndK      the same under a random order, shuffled with seed K
#   bryantN        x0.y0 + ... + x(N-1).y(N-1) under the split order, whose
#                  optimum, the paired order, has about 2N nodes
#   queensN        the N-queens board in its natural row-major order
#
# Both drivers compute the starting order with the same function
# (blif_var_perm in blif.c) from the same seed, so a case starts from the
# identical order on both sides.
#
# THE CASE TABLE. ../cases.tsv (or $SB_CASES_TSV) lists every case of the
# main grid, one row per circuit and starting order, with a header line and
# these tab-separated columns:
#
#   name        the case name, as above
#   mode        blif | queens | bryant
#   file        for blif, the circuit relative to --inputs (fetch-inputs.sh
#               --all installs it); "-" for queens and bryant, whose size is
#               the number in the name
#   var_order   file | reverse | random ("file" for queens and bryant: their
#               one built-in order)
#   seed        the --order-seed (0 unless random)
#   family      iscas85 | iscas89 | lgsynth91 | epfl | queens | bryant | fta
#               (fault trees)
#   size_class  small (sift < 1 s at P1) | medium (1-30 s) | large (> 30 s)
#   quick       1 for the quick subset (each case under 5 s per run), else 0
#   timeout_s   a per-run wall-clock cap for this case, in seconds
#   notes       free text
#   probs       the --probs file of the circuit, relative to --inputs
#               (fetch-inputs.sh installs one next to every fault tree), or
#               "-": every driver run of the case gets --probs with it
#
# A table without the probs column still reads.
#
# Where STEM is in the table, STEM-rndK for any K builds from that row's file
# (and its probs) too, so the seed sweeps work on every circuit. A name that
# is not in the table falls back to INPUTS/STEM.blif, or else to the one
# INPUTS/*/STEM.blif there is (so every circuit fetch-inputs.sh installs can
# be run by name, including those screened out of the table).
#
# As a command:
#
#   cases.sh [--family LIST] [--size LIST] [--order LIST] [--mode LIST]
#            [--quick] [--table]       the matching names on one line
#                                      (--table: the matching rows instead)
#   cases.sh --args NAME --inputs DIR  the driver arguments for NAME, one per line
#   cases.sh --check [--inputs DIR]    check the table (and that every file is
#                                      installed, with --inputs)
#
# LIST is comma-separated, e.g. --family iscas85,epfl --size small,medium. So
#   run-timing.sh ... --cases "$(scripts/cases.sh --quick)"
#
# Why these. Of the picotrav circuits only C880 is both feasible and
# non-degenerate (adder and bar, the EPFL arithmetic circuits tried, do not
# build under their file order with an 8 GB table; see README.md), so its
# four orders are the scripts' default cases. The table widens that to the
# ISCAS'85, ISCAS'89 (combinational part), LGSynth'91 and EPFL random/control
# circuits that build within 600 s and 8 GiB from at least two of the three
# starting orders on both tools, under the orders whose Sylvan run takes at
# most 60 s at P1 (README.md, "The case table", says how they were screened).
# bryant has a known optimum. queens is a case whose natural order is already
# good, so it measures what sifting costs when there is little to find.

SB_CASES_TSV=${SB_CASES_TSV:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/cases.tsv}

SB_CASES_C880="C880-file C880-rev C880-rnd1 C880-rnd2"
SB_CASES_BRYANT="bryant6 bryant8 bryant10 bryant12 bryant14"

SB_CASE_ARGV=()

# sb_cases_select [--family LIST] [--size LIST] [--order LIST] [--mode LIST]
#                 [--quick] [--table] -- print the matching case names, in
# table order, on one line (or, with --table, the matching rows).
sb_cases_select() {
  local family="" size="" order="" mode="" quick=0 table=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --family|--size|--order|--mode)
        [ $# -ge 2 ] || { echo "cases.sh: $1 needs a value" >&2; return 2; }
        case "$1" in
          --family) family=$2 ;; --size) size=$2 ;;
          --order)  order=$2 ;;  --mode) mode=$2 ;;
        esac
        shift 2 ;;
      --quick) quick=1; shift ;;
      --table) table=1; shift ;;
      *) echo "cases.sh: unknown selector $1" >&2; return 2 ;;
    esac
  done
  [ -f "$SB_CASES_TSV" ] || { echo "cases.sh: no case table $SB_CASES_TSV" >&2; return 1; }
  awk -F'\t' -v fam="$family" -v size="$size" -v ord="$order" -v mode="$mode" \
      -v quick="$quick" -v table="$table" '
    function want(list, v,   n, i, a) {
      if (list == "") return 1
      n = split(list, a, ",")
      for (i = 1; i <= n; i++) if (a[i] == v) return 1
      return 0
    }
    NR == 1 { if (table) print; next }
    $0 == "" { next }
    want(fam, $6) && want(size, $7) && want(ord, $4) && want(mode, $2) &&
    (!quick || $8 == 1) {
      if (table) print
      else { printf "%s%s", sep, $1; sep = " " }
    }
    END { if (!table && sep != "") printf "\n" }
  ' "$SB_CASES_TSV"
}

# sb_case_row NAME -- print NAME's row of the case table, or return 1.
sb_case_row() {
  [ -f "$SB_CASES_TSV" ] || return 1
  awk -F'\t' -v n="$1" 'NR > 1 && $1 == n { print; f = 1; exit } END { exit !f }' \
    "$SB_CASES_TSV"
}

# sb_case_field NAME COLUMN -- print one column (by header name) of NAME's
# row, or return 1.
sb_case_field() {
  [ -f "$SB_CASES_TSV" ] || return 1
  awk -F'\t' -v n="$1" -v col="$2" '
    NR == 1 { for (i = 1; i <= NF; i++) if ($i == col) c = i; next }
    c && $1 == n { print $c; f = 1; exit }
    END { exit !f }
  ' "$SB_CASES_TSV"
}

# sb_case_timeout NAME -- the table's per-run timeout for NAME, in seconds,
# or nothing (and status 1) when NAME is not in the table.
sb_case_timeout() { sb_case_field "$1" timeout_s; }

# sb_stem_file STEM -- the circuit file, relative to --inputs, of any blif row
# STEM-file, STEM-rev or STEM-rndK of the table, or return 1.
sb_stem_file() {
  [ -f "$SB_CASES_TSV" ] || return 1
  awk -F'\t' -v s="$1" '
    NR > 1 && $2 == "blif" && index($1, s "-") == 1 {
      rest = substr($1, length(s) + 2)
      if (rest == "file" || rest == "rev" || rest ~ /^rnd[0-9]+$/) { print $3; f = 1; exit }
    }
    END { exit !f }
  ' "$SB_CASES_TSV"
}

# sb_file_probs FILE -- the probs column of a row of the table whose file is
# FILE (relative to --inputs), or nothing; a --probs file belongs to the
# circuit, whatever order the case starts from.
sb_file_probs() {
  [ -f "$SB_CASES_TSV" ] || return 0
  awk -F'\t' -v file="$1" '
    NR == 1 { for (i = 1; i <= NF; i++) if ($i == "probs") c = i; next }
    c && $3 == file && $c != "" && $c != "-" { print $c; exit }
  ' "$SB_CASES_TSV"
}

# sb_case_args NAME -- set SB_CASE_ARGV to the harness arguments that build
# case NAME, or complain and return 1. BLIF cases need SB_INPUTS.
sb_case_args() {
  local name=$1 stem order seed file="" row mode tfile torder tseed
  case "$name" in
    bryant[0-9]*)
      SB_CASE_ARGV=(--mode bryant --pairs "${name#bryant}" --case "$name")
      [[ "${name#bryant}" =~ ^[0-9]+$ ]] || { echo "bad case name $name" >&2; return 1; }
      return 0 ;;
    queens[0-9]*)
      SB_CASE_ARGV=(--mode queens --n "${name#queens}" --case "$name")
      [[ "${name#queens}" =~ ^[0-9]+$ ]] || { echo "bad case name $name" >&2; return 1; }
      return 0 ;;
    *-file) stem=${name%-file}; order=file;    seed=0 ;;
    *-rev)  stem=${name%-rev};  order=reverse; seed=0 ;;
    *-rnd*) stem=${name%-rnd*}; order=random;  seed=${name##*-rnd} ;;
    *) echo "unknown case '$name' (want STEM-file, STEM-rev, STEM-rndK, bryantN or queensN)" >&2
       return 1 ;;
  esac
  [[ "$seed" =~ ^[0-9]+$ ]] || { echo "bad seed in case name $name" >&2; return 1; }
  if row=$(sb_case_row "$name"); then
    IFS=$'\t' read -r _ mode tfile torder tseed _ <<< "$row"
    if [ "$mode" != blif ] || [ "$torder" != "$order" ] || [ "$tseed" != "$seed" ]; then
      echo "case $name: its row in $SB_CASES_TSV says $mode $torder seed $tseed" >&2
      return 1
    fi
    file=$tfile
  else
    file=$(sb_stem_file "$stem") || file="$stem.blif"
  fi
  if [ -z "${SB_INPUTS:-}" ]; then
    echo "case $name needs --inputs DIR (holding $file)" >&2
    return 1
  fi
  if [ -z "$row" ] && [ ! -f "$SB_INPUTS/$file" ]; then
    # Not in the table: any one installed circuit of that name will do.
    local -a found=()
    local f
    for f in "$SB_INPUTS"/*/"$stem.blif"; do [ -f "$f" ] && found+=("${f#"$SB_INPUTS"/}"); done
    [ ${#found[@]} -eq 1 ] && file=${found[0]}
  fi
  local probs
  probs=$(sb_file_probs "$file")
  file="$SB_INPUTS/$file"
  [ -f "$file" ] || { echo "case $name: no $file (see fetch-inputs.sh --all)" >&2; return 1; }
  SB_CASE_ARGV=(--mode blif --file "$file" --case "$name"
                --var-order "$order" --order-seed "$seed")
  if [ -n "$probs" ]; then
    [ -f "$SB_INPUTS/$probs" ] ||
      { echo "case $name: no $SB_INPUTS/$probs (see fetch-inputs.sh --all)" >&2; return 1; }
    SB_CASE_ARGV+=(--probs "$SB_INPUTS/$probs")
  fi
}

# sb_cases_check -- check the case table: the header, unique names, every
# field in its range, and that each name says what its row says. With
# SB_INPUTS set, also that every circuit is installed. Prints the problems.
sb_cases_check() {
  [ -f "$SB_CASES_TSV" ] || { echo "no case table $SB_CASES_TSV" >&2; return 1; }
  local bad
  bad=$(awk -F'\t' '
    function err(m) { print "line " NR ": " m; n++ }
    NR == 1 {
      base = "name\tmode\tfile\tvar_order\tseed\tfamily\tsize_class\tquick\ttimeout_s\tnotes"
      if ($0 == base) cols = 10
      else if ($0 == base "\tprobs") cols = 11
      else err("bad header")
      next
    }
    NF != cols && !(cols == 10 && NF == 9) { err(NF " fields, want " cols); next }
    seen[$1]++ { err("duplicate name " $1) }
    $2 !~ /^(blif|queens|bryant)$/ { err("bad mode " $2) }
    $4 !~ /^(file|reverse|random)$/ { err("bad var_order " $4) }
    $5 !~ /^[0-9]+$/ { err("bad seed " $5) }
    $6 !~ /^(iscas85|iscas89|lgsynth91|epfl|queens|bryant|fta)$/ { err("bad family " $6) }
    cols == 11 && $11 != "-" && ($2 != "blif" || $11 ~ /^\// || $11 !~ /\.probs$/) { err("bad probs " $11) }
    $6 == "fta" && (cols != 11 || $11 == "-") { err($1 ": a fault tree without probs") }
    $7 !~ /^(small|medium|large)$/ { err("bad size_class " $7) }
    $8 !~ /^[01]$/ { err("bad quick " $8) }
    $9 !~ /^[1-9][0-9]*$/ { err("bad timeout_s " $9) }
    $2 == "blif" {
      if ($3 == "-" || $3 ~ /^\//) err("bad file " $3)
      want = ($4 == "file") ? "-file" : ($4 == "reverse") ? "-rev" : "-rnd" $5
      if (substr($1, length($1) - length(want) + 1) != want) err($1 " is not " $4 " seed " $5)
      if ($4 != "random" && $5 != 0) err($1 ": seed must be 0")
    }
    $2 != "blif" {
      if ($3 != "-" || $4 != "file" || $5 != 0) err($1 ": want file - order file seed 0")
      if ($1 !~ ("^" $2 "[0-9]+$")) err($1 " is not " $2 "N")
    }
    END { exit n > 0 }
  ' "$SB_CASES_TSV")
  [ -z "$bad" ] || { echo "$bad"; return 1; }
  if [ -n "${SB_INPUTS:-}" ]; then
    local f missing=0
    while read -r f; do
      [ -f "$SB_INPUTS/$f" ] || { echo "not installed: $SB_INPUTS/$f"; missing=1; }
    done < <(awk -F'\t' 'NR > 1 && $2 == "blif" { print $3; if (NF >= 11 && $11 != "-") print $11 }' \
               "$SB_CASES_TSV" | sort -u)
    return $missing
  fi
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  set -uo pipefail
  SB_INPUTS=""; args_of=""; check=0; sel=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --inputs) [ $# -ge 2 ] || { echo "cases.sh: $1 needs a value" >&2; exit 2; }
                SB_INPUTS=$(cd "$2" && pwd) || exit 2; shift 2 ;;
      --args)   [ $# -ge 2 ] || { echo "cases.sh: $1 needs a value" >&2; exit 2; }
                args_of=$2; shift 2 ;;
      --check)  check=1; shift ;;
      -h|--help) sed -n '2,/^[^#]/{/^#/s/^# \{0,1\}//p}' "$0"; exit 0 ;;
      *)        sel+=("$1"); shift ;;
    esac
  done
  if [ "$check" = 1 ]; then
    sb_cases_check || exit 1
    echo "$SB_CASES_TSV: $(($(wc -l < "$SB_CASES_TSV") - 1)) cases, ok"
  elif [ -n "$args_of" ]; then
    sb_case_args "$args_of" || exit 1
    printf '%s\n' "${SB_CASE_ARGV[@]}"
  else
    sb_cases_select "${sel[@]}"
  fi
fi
