# The cases the measurement scripts run, by name. Sourced by lib.sh.
#
# A case name says everything about how the forest is built, so that a row's
# "case" field is enough to rebuild it:
#
#   STEM-file      every .outputs signal of INPUTS/STEM.blif, under the file's
#                  own variable order (inputs, then latch outputs)
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
# Why these. Of the picotrav circuits only C880 is both feasible and
# non-degenerate: the EPFL arithmetic circuits (adder, bar) could not be built
# under their file order by either tool with an 8 GB table (see README.md),
# and the sequential ones (mult32a, rcn25) have almost no logic reachable from
# .outputs once latches are cut. So C880 is run from four starting orders -- its own,
# reversed, and two seeded shuffles -- which is closer to the question "does
# sifting recover from a bad order" than the file order alone. bryant has a
# known optimum. queens is a case whose natural order is already good, so it
# measures what sifting costs when there is little to find.

SB_CASES_C880="C880-file C880-rev C880-rnd1 C880-rnd2"
SB_CASES_BRYANT="bryant6 bryant8 bryant10 bryant12 bryant14"

SB_CASE_ARGV=()

# sb_case_args NAME -- set SB_CASE_ARGV to the harness arguments that build
# case NAME, or complain and return 1. BLIF cases need SB_INPUTS.
sb_case_args() {
  local name=$1 stem order seed file
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
  if [ -z "${SB_INPUTS:-}" ]; then
    echo "case $name needs --inputs DIR (holding $stem.blif)" >&2
    return 1
  fi
  file="$SB_INPUTS/$stem.blif"
  [ -f "$file" ] || { echo "case $name: no $file (see fetch-inputs.sh)" >&2; return 1; }
  SB_CASE_ARGV=(--mode blif --file "$file" --case "$name"
                --var-order "$order" --order-seed "$seed")
}
