#!/usr/bin/env bash
# fetch-inputs.sh -- get the circuit the sift scripts run on, and say where
# the other input families of the evaluation come from.
#
#   fetch-inputs.sh [--cudd-src DIR] [DEST]
#   fetch-inputs.sh --families
#
# Puts C880.blif into DEST (default ./inputs), which is what the run-*.sh
# scripts take as --inputs. The file is CUDD's own copy from the nanotrav
# directory at the CUDD commit the evaluation pins (f54f533, "Import
# cudd-3.0.0.tar.gz"), downloaded from GitHub, or copied from a local CUDD
# checkout with --cudd-src. Carriage returns are stripped, so a copy taken
# from a Windows checkout gives the same bytes, and the result must have the
# recorded MD5 before it is moved into place.
#
# Nothing is ever overwritten: if DEST/C880.blif exists, it is checked and
# left alone, and the script fails if it does not match.
#
# --families prints the sources of the other families (McNet, relprod, QBF,
# and the EPFL arithmetic circuits) with the commands that fetch them. They
# are large, they are inputs to other harnesses than sylvan_bench, and this
# script does not download them.
set -euo pipefail

CUDD_COMMIT=f54f533303640afd5dbe47a05ebeabb3066f2a25
C880_URL="https://raw.githubusercontent.com/cuddorg/cudd/$CUDD_COMMIT/nanotrav/C880.blif"
C880_MD5=82323a515652b47cdbff16cb2e0bc27e   # after stripping CR; 18428 bytes

families() {
  cat <<'EOF'
Other input families (not fetched by this script)
==================================================

McNet: Petri nets from the Model Checking Contest 2024 (PNML archives).
  Used: Anderson/PT/anderson_04.pnml, EisenbergMcGuire/PT/eisenberg-mcguire_03.pnml.

    mkdir -p mcnet/archives mcnet/models && cd mcnet/archives
    for m in Anderson Dekker EisenbergMcGuire RwMutex; do
      curl -fLO "https://mcc.lip6.fr/2024/archives/$m-pnml.tar.gz"
    done
    md5sum -c <<'SUMS'
    79cc05444df3339016a6560b250c293c  Anderson-pnml.tar.gz
    985c65917092b2e5f64675b37108fb6e  Dekker-pnml.tar.gz
    22373f6162527a0534c57a589c64950e  EisenbergMcGuire-pnml.tar.gz
    68f7136d543cea65191f941b210fb698  RwMutex-pnml.tar.gz
    SUMS
    for a in *-pnml.tar.gz; do tar -xzf "$a" -C ../models; done
    # -> mcnet/models/Anderson/PT/anderson_04.pnml, ...
  The checksums are those of the archives used in the evaluation (fetched
  2026-07-28); the index of all models is https://mcc.lip6.fr/2024/models.php

relprod: serialized BDDs (relation and reachable-state sets) from the MCC,
  Soelvsten 2024, doi:10.5281/zenodo.13928216 (record v1.0.0). Only 2023.zip,
  127.4 MB, is used; the other years are 3.5 to 13.8 GB.

    curl -fL -o relprod-2023.zip \
      "https://zenodo.org/records/13928216/files/2023.zip?download=1"
    echo "d7e5cf2c5e6866423123764633c33d96  relprod-2023.zip" | md5sum -c
    python3 -m zipfile -e relprod-2023.zip relprod-2023/
    python3 -m zipfile -e relprod-2023/2023__Anderson__anderson_04.zip relprod-anderson-04/
    python3 -m zipfile -e relprod-2023/2023__EisenbergMcGuire__eisenberg-mcguire_03.zip relprod-eisenberg-03/
    # -> relprod-anderson-04/relation.bdd, states_6.bdd ... states_14.bdd, states_all.bdd

QBF: Q-sage's SAT 2023 GDDL instances, at commit 0817721.

    git clone --filter=blob:none https://github.com/irfansha/Q-sage.git
    git -C Q-sage checkout 08177212dbca5605f84c86d702fc06e4ecce5cc8
    # -> Q-sage/Benchmarks/SAT2023_GDDL/QBF_instances/{B,BSP,C4,D,hex,httt,...}/*.qcir
    # used: B/3x5_11_bwnib.qcir, C4/5x5_11_connect4_bwnib.qcir

EPFL arithmetic (lsils): DOCUMENTED INFEASIBLE, not part of the evaluation.

    git clone --filter=blob:none https://github.com/lsils/benchmarks.git lsils-benchmarks
    git -C lsils-benchmarks checkout 0060e156826e733d69bf5b3322d1bdd0d03a1f9a
    # arithmetic/adder.blif  md5 2e0273f1b5dbda27b34747a27e57ad80  (256 vars, 129 outputs)
    # arithmetic/bar.blif    md5 0f5ed20e1e3e65dbf192f1fa311ee4a1  (135 vars, 128 outputs)

  Neither adder nor bar can be BUILT under its file order by either tool: with
  an 8 GB cap sylvan_bench filled its nodes table (2^27 buckets, a dozen
  collections each reclaiming under 5%), and cudd_bench passed 300 s, or
  13.6 GB resident, without finishing the build. An adder or barrel shifter
  under an unfavourable input order is exponential, and a sift cannot help a
  forest that never fits. The other arithmetic circuits (div, hyp, log2, max,
  multiplier, sin, sqrt, square) were not attempted.
EOF
}

die() { echo "fetch-inputs.sh: $*" >&2; exit 1; }

md5_of() {
  if command -v md5sum > /dev/null 2>&1; then
    md5sum "$1" | cut -d' ' -f1
  elif command -v md5 > /dev/null 2>&1; then
    md5 -q "$1"
  else
    die "need md5sum or md5"
  fi
}

cudd_src=""
dest=""
while [ $# -gt 0 ]; do
  case "$1" in
    --families) families; exit 0 ;;
    --cudd-src) [ $# -ge 2 ] || die "$1 needs a directory"; cudd_src=$2; shift 2 ;;
    -h|--help)  sed -n '2,/^[^#]/{/^#/s/^# \{0,1\}//p}' "$0"; exit 0 ;;
    -*)         die "unknown option $1" ;;
    *)          [ -z "$dest" ] || die "one destination only"; dest=$1; shift ;;
  esac
done
dest=${dest:-inputs}

target="$dest/C880.blif"
if [ -e "$target" ]; then
  got=$(md5_of "$target")
  if [ "$got" = "$C880_MD5" ]; then
    echo "$target already exists and matches md5 $C880_MD5; left as it is"
    exit 0
  fi
  die "$target already exists with md5 $got, expected $C880_MD5; refusing to overwrite it"
fi

mkdir -p "$dest"
raw=$(mktemp "$dest/.C880.raw.XXXXXX")
clean=$(mktemp "$dest/.C880.blif.XXXXXX")
trap 'rm -f "$raw" "$clean"' EXIT

if [ -n "$cudd_src" ]; then
  [ -f "$cudd_src/nanotrav/C880.blif" ] || die "no $cudd_src/nanotrav/C880.blif"
  echo "copying C880.blif from $cudd_src/nanotrav"
  cp "$cudd_src/nanotrav/C880.blif" "$raw"
elif command -v curl > /dev/null 2>&1; then
  echo "downloading $C880_URL"
  curl -fsSL -o "$raw" "$C880_URL" || die "download failed"
elif command -v wget > /dev/null 2>&1; then
  echo "downloading $C880_URL"
  wget -q -O "$raw" "$C880_URL" || die "download failed"
else
  die "need curl or wget, or --cudd-src DIR"
fi

tr -d '\r' < "$raw" > "$clean"
got=$(md5_of "$clean")
[ "$got" = "$C880_MD5" ] || die "C880.blif has md5 $got, expected $C880_MD5; not installed"

# No-clobber move, so that a file which appeared meanwhile is not replaced.
chmod 644 "$clean"
mv -n "$clean" "$target" 2>/dev/null || true
[ ! -e "$clean" ] || die "$target appeared meanwhile; refusing to overwrite it"
echo "installed $target (md5 $got)"
echo "for the other input families, see: $0 --families"
