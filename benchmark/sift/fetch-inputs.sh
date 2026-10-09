#!/usr/bin/env bash
# fetch-inputs.sh -- install the circuits the sift scripts run on, every one
# checksummed, and say where the other input families of the evaluation come
# from.
#
#   fetch-inputs.sh [--all | --sets LIST] [--src NAME=DIR]... [DEST]
#   fetch-inputs.sh --check [--all | --sets LIST] [DEST]
#   fetch-inputs.sh --list
#   fetch-inputs.sh --families
#
# Installs into DEST (default ./inputs), which is what the run-*.sh scripts
# take as --inputs. The sets, by name (LIST is comma-separated):
#
#   c880       C880.blif: CUDD's own copy, from nanotrav/ at the CUDD commit
#              the evaluation pins (f54f533). The default, as it always was.
#   iscas85    iscas85/C*.blif: the ISCAS'85 circuits as distributed with
#              LGSynth'91 (Brglez and Fujiwara 1985)
#   lgsynth91  lgsynth91/*.blif: MCNC / LGSynth'91 combinational circuits
#              (Yang 1991)
#   iscas89    iscas89/s*.blif: the combinational part of ISCAS'89 circuits
#              as distributed with LGSynth'91 (Brglez, Bryan, Kozminski 1989)
#   epfl       epfl/*.blif: the EPFL random/control circuits (Amaru,
#              Gaillardon, De Micheli 2015), lsils/benchmarks
#
# --all installs every set: every circuit ../cases.tsv names, and those its
# screening left out (see README.md, "The case table"). The LGSynth'91,
# ISCAS and MCNC files are Petr Fiser's collection (Czech Technical University,
# https://ddd.fit.cvut.cz/www/prj/Benchmarks/, MCNC.7z and LGSynth91.7z),
# fetched from the LF-only copy in ispras/hdl-benchmarks at a pinned commit;
# the EPFL files from lsils/benchmarks at a pinned commit; C880 from CUDD.
# --list prints every file with its source URL and checksums.
#
# Every download has its carriage returns stripped and must then have the
# recorded MD5 before anything else happens to it. Some files are then
# converted to the BLIF subset blif.c reads (see COMBINATIONAL PART below),
# and the result must have its own recorded MD5 before it is moved into
# place. Nothing is ever overwritten: a file that exists is checked and left
# alone, and the script fails if it does not match.
#
# --src NAME=DIR reads the files from a local copy instead of downloading
# them: cudd=DIR a CUDD checkout (--cudd-src DIR is the same), ispras=DIR a
# checkout of ispras/hdl-benchmarks, lsils=DIR a checkout of lsils/benchmarks
# (each at the commit below), or fiser=DIR the directory MCNC.7z and
# LGSynth91.7z were extracted into (so DIR/MCNC/Combinational/blif and
# DIR/LGSynth91/blif exist). Any of them give the same bytes.
#
# --check verifies what is installed in DEST against the recorded checksums
# and lists every file of the chosen sets as ok, MISSING or BAD; it exits
# non-zero unless all are ok.
#
# COMBINATIONAL PART. blif.c reads .model .inputs .outputs .latch .names
# .end. It cuts a latch into a free variable and builds only .outputs, so a
# sequential circuit's next-state functions would not be built at all. The
# files marked "comb" are therefore converted, by the awk program below, into
# their combinational part: ".latch IN OUT" becomes ".inputs OUT" and
# ".outputs IN" in the same place (so the variable order is the one blif.c
# would give the original: inputs, then latch outputs in latch order), the
# ".wire_load_slope" delay annotations are dropped, an ".exdc" external
# don't-care network is dropped with everything after it (the care network
# is kept), and .outputs names that nothing drives are dropped (blif.c
# rejects them; s13207.1 has five, s15850.1 one). Continuation lines are
# joined and comments and blank lines removed.
#
# --families prints the sources of the other families (McNet, relprod, QBF,
# and the EPFL arithmetic circuits) with the commands that fetch them. They
# are large, they are inputs to other harnesses than sylvan_bench, and this
# script does not download them.
set -euo pipefail

CUDD_COMMIT=f54f533303640afd5dbe47a05ebeabb3066f2a25
ISPRAS_COMMIT=10c3fb51d0810ce862659211862660dcc270a6af
LSILS_COMMIT=0060e156826e733d69bf5b3322d1bdd0d03a1f9a

url_of() {  # SOURCE PATH
  case "$1" in
    cudd)   echo "https://raw.githubusercontent.com/cuddorg/cudd/$CUDD_COMMIT/$2" ;;
    ispras) echo "https://raw.githubusercontent.com/ispras/hdl-benchmarks/$ISPRAS_COMMIT/$2" ;;
    lsils)  echo "https://raw.githubusercontent.com/lsils/benchmarks/$LSILS_COMMIT/$2" ;;
  esac
}

ALL_SETS="c880 iscas85 lgsynth91 iscas89 epfl"

# One line per file: set, installed path, source, path in the source, MD5 of
# the download after stripping CRs, conversion ("-" or "comb"), MD5 of the
# installed file.
MANIFEST=$(cat <<'EOF'
c880       C880.blif                 cudd    nanotrav/C880.blif                      82323a515652b47cdbff16cb2e0bc27e  -     82323a515652b47cdbff16cb2e0bc27e
iscas85    iscas85/C432.blif         ispras  mcnc/Combinational/blif/C432.blif       aa41bd507afa43654d6656eb0caa6f8e  -     aa41bd507afa43654d6656eb0caa6f8e
iscas85    iscas85/C499.blif         ispras  mcnc/Combinational/blif/C499.blif       32c2a54be3349c73cccb64287e0f454c  -     32c2a54be3349c73cccb64287e0f454c
iscas85    iscas85/C1355.blif        ispras  mcnc/Combinational/blif/C1355.blif      11f5cc694c0f0d3c7ca658a942c86793  -     11f5cc694c0f0d3c7ca658a942c86793
iscas85    iscas85/C1908.blif        ispras  mcnc/Combinational/blif/C1908.blif      8a83d1a7fbc9879df52cbc2d9537de99  -     8a83d1a7fbc9879df52cbc2d9537de99
iscas85    iscas85/C2670.blif        ispras  mcnc/Combinational/blif/C2670.blif      fb7d83bc65251ab4984e800b565f9fe3  -     fb7d83bc65251ab4984e800b565f9fe3
iscas85    iscas85/C3540.blif        ispras  mcnc/Combinational/blif/C3540.blif      9ffada2399d817f85f87747709e0ba7b  -     9ffada2399d817f85f87747709e0ba7b
iscas85    iscas85/C5315.blif        ispras  mcnc/Combinational/blif/C5315.blif      df6118e8671586e227198d86534b38bb  -     df6118e8671586e227198d86534b38bb
iscas85    iscas85/C6288.blif        ispras  mcnc/Combinational/blif/C6288.blif      0512ed473295818364592a76d620ad61  -     0512ed473295818364592a76d620ad61
iscas85    iscas85/C7552.blif        ispras  mcnc/Combinational/blif/C7552.blif      d8c113b14baaba4d64a4fd6b1f90bf33  -     d8c113b14baaba4d64a4fd6b1f90bf33
lgsynth91  lgsynth91/apex2.blif      ispras  mcnc/Combinational/blif/apex2.blif      e6690f96bd3632734ca19c269a91bd0c  -     e6690f96bd3632734ca19c269a91bd0c
lgsynth91  lgsynth91/bc0.blif        ispras  mcnc/Combinational/blif/bc0.blif        8c5b403510b66be7ec575bc0f273da31  -     8c5b403510b66be7ec575bc0f273da31
lgsynth91  lgsynth91/bigkey.blif     ispras  lgsynth91/blif/bigkey.blif              4b028c6d92d6c026d96b709a848e5065  comb  789420c30a8c2a9fde5d3c57894af131
lgsynth91  lgsynth91/clma.blif       ispras  lgsynth91/blif/clma.blif                0304cfd40c5ad3f9b8cd492302dd42bc  comb  845b944bea2f83005a40be594bd478c2
lgsynth91  lgsynth91/clmb.blif       ispras  lgsynth91/blif/clmb.blif                d492f326a04b210d27f725e8a5c483ac  comb  d4156594a301193594ff5a8463d9b7d5
lgsynth91  lgsynth91/cm150a.blif     ispras  mcnc/Combinational/blif/cm150a.blif     1bc2ee5abd24ac780eeb6236c0db96f8  -     1bc2ee5abd24ac780eeb6236c0db96f8
lgsynth91  lgsynth91/comp.blif       ispras  mcnc/Combinational/blif/comp.blif       f29b443ec1d966781cb78afb28ea1ea9  -     f29b443ec1d966781cb78afb28ea1ea9
lgsynth91  lgsynth91/dalu.blif       ispras  mcnc/Combinational/blif/dalu.blif       5b29f88b89422f844f7b00be68cf6c3f  -     5b29f88b89422f844f7b00be68cf6c3f
lgsynth91  lgsynth91/des.blif        ispras  mcnc/Combinational/blif/des.blif        8c30b3ae7bc401c4fd4f9c9c5194eb4c  -     8c30b3ae7bc401c4fd4f9c9c5194eb4c
lgsynth91  lgsynth91/dsip.blif       ispras  lgsynth91/blif/dsip.blif                e90e4181bc8468813e13ddc2c7bd42e1  comb  23f0f6699cfe51b1270e53e254f68fdf
lgsynth91  lgsynth91/frg1.blif       ispras  mcnc/Combinational/blif/frg1.blif       a88f3400b8371aad8e9aa74e4aca62c8  -     a88f3400b8371aad8e9aa74e4aca62c8
lgsynth91  lgsynth91/frg2.blif       ispras  mcnc/Combinational/blif/frg2.blif       2e8f31604b970938d9d866f48f1c8466  -     2e8f31604b970938d9d866f48f1c8466
lgsynth91  lgsynth91/i2.blif         ispras  mcnc/Combinational/blif/i2.blif         6ea37a7875de69a93e9b12c6ecc910d9  -     6ea37a7875de69a93e9b12c6ecc910d9
lgsynth91  lgsynth91/i3.blif         ispras  mcnc/Combinational/blif/i3.blif         92e78279ae419875ede1644266ee52cb  -     92e78279ae419875ede1644266ee52cb
lgsynth91  lgsynth91/i4.blif         ispras  mcnc/Combinational/blif/i4.blif         03518cf24bfe25a02b9df6b5541685b2  -     03518cf24bfe25a02b9df6b5541685b2
lgsynth91  lgsynth91/i8.blif         ispras  mcnc/Combinational/blif/i8.blif         b9e5f101a7a33a6ca86896257c649d1d  -     b9e5f101a7a33a6ca86896257c649d1d
lgsynth91  lgsynth91/i9.blif         ispras  mcnc/Combinational/blif/i9.blif         31476b3e516f5fec0803932e2de11e0f  -     31476b3e516f5fec0803932e2de11e0f
lgsynth91  lgsynth91/i10.blif        ispras  mcnc/Combinational/blif/i10.blif        aeb56d0ef05ec98598dba99800cfe0b2  -     aeb56d0ef05ec98598dba99800cfe0b2
lgsynth91  lgsynth91/k2.blif         ispras  mcnc/Combinational/blif/k2.blif         68b66ed62866e26399a15e3f453210e2  -     68b66ed62866e26399a15e3f453210e2
lgsynth91  lgsynth91/mainpla.blif    ispras  mcnc/Combinational/blif/mainpla.blif    229407a6cdc87acea9957ba31bcae22a  -     229407a6cdc87acea9957ba31bcae22a
lgsynth91  lgsynth91/mm9a.blif       ispras  lgsynth91/blif/mm9a.blif                4bfc53d0f0e3c5a7440e7b0ee3e6ea7c  comb  bb116a19ccd554829220bbf2aba71736
lgsynth91  lgsynth91/mm9b.blif       ispras  lgsynth91/blif/mm9b.blif                199a5ba4f54f35272ad9db08fbbf0158  comb  8f086b51cbe8e6c33a4619f65f4c8cee
lgsynth91  lgsynth91/mm30a.blif      ispras  lgsynth91/blif/mm30a.blif               15cb7c682cae592d9242157ae4853c70  comb  4d6cf1b0060cac927c2d53cdfb099bbb
lgsynth91  lgsynth91/mult16a.blif    ispras  lgsynth91/blif/mult16a.blif             21c7ea3392c7193c991d65428365bcce  comb  fed657ad9392778c3983507a311c47ed
lgsynth91  lgsynth91/mult32a.blif    ispras  lgsynth91/blif/mult32a.blif             4dd07440ed2874c2600f40a2d3a2900a  comb  58cf1c967cd1a04d079bbe7177c643d6
lgsynth91  lgsynth91/mux.blif        ispras  mcnc/Combinational/blif/mux.blif        01804d524242f2cc316b6c8aa4a89eb5  -     01804d524242f2cc316b6c8aa4a89eb5
lgsynth91  lgsynth91/my_adder.blif   ispras  mcnc/Combinational/blif/my_adder.blif   b149a089cfb1b0400a6dc3f6e20c720f  -     b149a089cfb1b0400a6dc3f6e20c720f
lgsynth91  lgsynth91/o64.blif        ispras  mcnc/Combinational/blif/o64.blif        d8ff980ac05a028d65e44a900d3ae15e  -     d8ff980ac05a028d65e44a900d3ae15e
lgsynth91  lgsynth91/pair.blif       ispras  mcnc/Combinational/blif/pair.blif       ecaa413bfe8ff4b3cb54bbfa14335a8e  -     ecaa413bfe8ff4b3cb54bbfa14335a8e
lgsynth91  lgsynth91/rot.blif        ispras  mcnc/Combinational/blif/rot.blif        6f8db77cee2540b963ad230f2692f8bc  -     6f8db77cee2540b963ad230f2692f8bc
lgsynth91  lgsynth91/seq.blif        ispras  mcnc/Combinational/blif/seq.blif        442487a78a30c6e2c4d6bd62b47637d8  -     442487a78a30c6e2c4d6bd62b47637d8
lgsynth91  lgsynth91/signet.blif     ispras  mcnc/Combinational/blif/signet.blif     70f6b793832244d00a379ede9f64a6e5  -     70f6b793832244d00a379ede9f64a6e5
lgsynth91  lgsynth91/ti.blif         ispras  mcnc/Combinational/blif/ti.blif         5fb879aec696ab462b67a440079d4580  -     5fb879aec696ab462b67a440079d4580
lgsynth91  lgsynth91/too_large.blif  ispras  mcnc/Combinational/blif/too_large.blif  0572f9e30e46f108c94684dbfa0cfed9  -     0572f9e30e46f108c94684dbfa0cfed9
lgsynth91  lgsynth91/x6dn.blif       ispras  mcnc/Combinational/blif/x6dn.blif       10c6886d7127cbcf084bbc8ca76933ad  -     10c6886d7127cbcf084bbc8ca76933ad
lgsynth91  lgsynth91/xparc.blif      ispras  mcnc/Combinational/blif/xparc.blif      882a3535f07329e9f1f37c583ca30818  -     882a3535f07329e9f1f37c583ca30818
iscas89    iscas89/s420.1.blif       ispras  lgsynth91/blif/s420.1.blif              d54e84f3565cf78fdd7659980e92309a  comb  9b791d5050bc3093f80d395c2522e6b7
iscas89    iscas89/s510.blif         ispras  lgsynth91/blif/s510.blif                4469529c7c64020186c8065cc273d067  comb  e3df1ab0781cfcd1049555d0a8666203
iscas89    iscas89/s838.1.blif       ispras  lgsynth91/blif/s838.1.blif              0d70836bc39d32273b09d39e09b4935a  comb  5cef461dce0a8aba8dd04f64d53a1439
iscas89    iscas89/s1423.blif        ispras  lgsynth91/blif/s1423.blif               be19d73f81669602a9549e12ae0492e1  comb  12259ee64228b1bfdf14caffe120e028
iscas89    iscas89/s5378.blif        ispras  lgsynth91/blif/s5378.blif               faab6268aa9d7c00c808f218fceead6b  comb  fd33dcab3382fa3eda7c0b90c7e82c40
iscas89    iscas89/s9234.1.blif      ispras  lgsynth91/blif/s9234.1.blif             97a8b43ba0902021a5cb7929f898dea9  comb  f3fb817bc23f8b3b170407425946c5d5
iscas89    iscas89/s13207.1.blif     ispras  lgsynth91/blif/s13207.1.blif            b6e452bbd88be939b3df0892cb7aa28b  comb  83098664af44595caf6a541b8988f96d
iscas89    iscas89/s15850.1.blif     ispras  lgsynth91/blif/s15850.1.blif            35715680c76fbf218020835ee0f5e515  comb  304d5a1a229e4ad752af2d4fa7cfb7f2
iscas89    iscas89/s38417.blif       ispras  lgsynth91/blif/s38417.blif              efcf93e1518e7633ff2ea9eadd9ba148  comb  e70ec663b71a948e01425df8e30f3530
iscas89    iscas89/s38584.1.blif     ispras  lgsynth91/blif/s38584.1.blif            b8df569f9180ee0d5f815827e67771df  comb  29a4ca3643a4665d53eb7c1ec92ee6ef
epfl       epfl/arbiter.blif         lsils   random_control/arbiter.blif             b35cdb5b33cd9d191784a603bd97f20d  -     b35cdb5b33cd9d191784a603bd97f20d
epfl       epfl/mem_ctrl.blif        lsils   random_control/mem_ctrl.blif            877e87c42e0420e5ebbe3a0b0e6b646f  -     877e87c42e0420e5ebbe3a0b0e6b646f
epfl       epfl/priority.blif        lsils   random_control/priority.blif            58438316102d3f434a16327d02d5f783  -     58438316102d3f434a16327d02d5f783
epfl       epfl/voter.blif           lsils   random_control/voter.blif               d744c8a56318bceeb7ecfb4669673008  -     d744c8a56318bceeb7ecfb4669673008
EOF
)

# The combinational part (see COMBINATIONAL PART above). The installed MD5s
# depend on every byte this prints; do not change it without them.
COMB_AWK=$(cat <<'EOF'
function strip(s) { sub(/#.*/, "", s); return s }
{
  sub(/\r$/, "")
  line = pending $0
  if (line ~ /\\[ \t]*$/) { sub(/\\[ \t]*$/, " ", line); pending = line; next }
  pending = ""
  L[++n] = line
}
END {
  if (pending != "") L[++n] = pending
  for (i = 1; i <= n; i++) {
    k = split(strip(L[i]), t, " ")
    if (k == 0) continue
    if (t[1] == ".exdc") break
    if (t[1] == ".inputs") { for (j = 2; j <= k; j++) drv[t[j]] = 1 }
    else if (t[1] == ".latch" && k >= 3) drv[t[3]] = 1
    else if (t[1] == ".names") drv[t[k]] = 1
  }
  for (i = 1; i <= n; i++) {
    k = split(strip(L[i]), t, " ")
    if (k == 0) continue
    d = t[1]
    if (d == ".exdc") { print ".end"; exit }
    if (d == ".wire_load_slope") continue
    if (d == ".latch") {
      print ".inputs " t[3]
      if (t[2] in drv) print ".outputs " t[2]
      continue
    }
    if (d == ".outputs") {
      o = ""
      for (j = 2; j <= k; j++) if (t[j] in drv) o = o " " t[j]
      if (o != "") print ".outputs" o
      continue
    }
    s = t[1]
    for (j = 2; j <= k; j++) s = s " " t[j]
    print s
  }
}
EOF
)

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
  (The EPFL random/control circuits are: fetch-inputs.sh --sets epfl.)

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

src_cudd=""; src_ispras=""; src_lsils=""; src_fiser=""
sets=""
mode=install
dest=""
while [ $# -gt 0 ]; do
  case "$1" in
    --families) families; exit 0 ;;
    --list)     mode=list; shift ;;
    --check)    mode=check; shift ;;
    --all)      sets=$ALL_SETS; shift ;;
    --sets)     [ $# -ge 2 ] || die "$1 needs a list"; sets=${2//,/ }; shift 2 ;;
    --cudd-src) [ $# -ge 2 ] || die "$1 needs a directory"; src_cudd=$2; shift 2 ;;
    --src)      [ $# -ge 2 ] || die "$1 needs NAME=DIR"
                case "$2" in
                  cudd=?*)   src_cudd=${2#*=} ;;
                  ispras=?*) src_ispras=${2#*=} ;;
                  lsils=?*)  src_lsils=${2#*=} ;;
                  fiser=?*)  src_fiser=${2#*=} ;;
                  *) die "--src wants cudd=DIR, ispras=DIR, lsils=DIR or fiser=DIR" ;;
                esac
                shift 2 ;;
    -h|--help)  sed -n '2,/^[^#]/{/^#/s/^# \{0,1\}//p}' "$0"; exit 0 ;;
    -*)         die "unknown option $1" ;;
    *)          [ -z "$dest" ] || die "one destination only"; dest=$1; shift ;;
  esac
done
dest=${dest:-inputs}
sets=${sets:-c880}
for s in $sets; do
  case " $ALL_SETS " in *" $s "*) ;; *) die "unknown set $s (sets: $ALL_SETS)" ;; esac
done

# The manifest lines of the chosen sets.
selected() {
  local s
  while read -r set path src spath umd5 conv imd5; do
    [ -n "$set" ] || continue
    for s in $sets; do
      [ "$s" = "$set" ] && echo "$set $path $src $spath $umd5 $conv $imd5"
    done
  done <<< "$MANIFEST"
}

if [ "$mode" = list ]; then
  printf '%-10s %-26s %-32s %-5s %-32s %s\n' set file "md5 (installed)" conv "md5 (download, CR stripped)" url
  while read -r set path src spath umd5 conv imd5; do
    [ -n "$set" ] || continue
    printf '%-10s %-26s %-32s %-5s %-32s %s\n' "$set" "$path" "$imd5" "$conv" "$umd5" "$(url_of "$src" "$spath")"
  done <<< "$MANIFEST"
  exit 0
fi

if [ "$mode" = check ]; then
  bad=0; n=0
  while read -r set path src spath umd5 conv imd5; do
    n=$((n + 1))
    if [ ! -e "$dest/$path" ]; then
      echo "MISSING $dest/$path"; bad=$((bad + 1))
    elif [ "$(md5_of "$dest/$path")" != "$imd5" ]; then
      echo "BAD     $dest/$path (md5 $(md5_of "$dest/$path"), expected $imd5)"; bad=$((bad + 1))
    else
      echo "ok      $dest/$path"
    fi
  done < <(selected)
  echo "$n files of sets [$sets]: $((n - bad)) ok, $bad missing or bad"
  [ "$bad" -eq 0 ]
  exit
fi

# fetch_raw SOURCE PATH OUT -- the file as published, from a local copy or
# the pinned URL.
fetch_raw() {
  local src=$1 spath=$2 out=$3 dir="" lpath=$2
  case "$src" in
    cudd) dir=$src_cudd ;; ispras) dir=$src_ispras ;; lsils) dir=$src_lsils ;;
  esac
  if [ -z "$dir" ] && [ "$src" = ispras ] && [ -n "$src_fiser" ]; then
    dir=$src_fiser
    case "$spath" in
      mcnc/Combinational/blif/*) lpath=MCNC/Combinational/blif/${spath##*/} ;;
      lgsynth91/blif/*)          lpath=LGSynth91/blif/${spath##*/} ;;
      *) return 1 ;;
    esac
  fi
  if [ -n "$dir" ]; then
    [ -f "$dir/$lpath" ] || { echo "no $dir/$lpath" >&2; return 1; }
    cp "$dir/$lpath" "$out"
    return
  fi
  local url
  url=$(url_of "$src" "$spath")
  if command -v curl > /dev/null 2>&1; then
    curl -fsSL --retry 3 -o "$out" "$url"
  elif command -v wget > /dev/null 2>&1; then
    wget -q -O "$out" "$url"
  else
    echo "need curl or wget, or --src $src=DIR" >&2
    return 1
  fi
}

tmpfiles=()
trap 'rm -f ${tmpfiles[@]+"${tmpfiles[@]}"}' EXIT

# install_one SET PATH SOURCE SPATH UMD5 CONV IMD5 -- 0 installed or already
# there and matching, 1 failed.
install_one() {
  local path=$2 src=$3 spath=$4 umd5=$5 conv=$6 imd5=$7
  local target="$dest/$path" got raw clean
  if [ -e "$target" ]; then
    got=$(md5_of "$target")
    if [ "$got" = "$imd5" ]; then
      echo "ok        $target (already there)"
      return 0
    fi
    echo "REFUSED   $target exists with md5 $got, expected $imd5; not overwritten" >&2
    return 1
  fi
  mkdir -p "$(dirname "$target")"
  raw=$(mktemp "$(dirname "$target")/.fetch.raw.XXXXXX")
  clean=$(mktemp "$(dirname "$target")/.fetch.blif.XXXXXX")
  tmpfiles+=("$raw" "$clean")
  if ! fetch_raw "$src" "$spath" "$raw"; then
    echo "FAILED    $target: could not get $spath from $src" >&2
    return 1
  fi
  tr -d '\r' < "$raw" > "$clean"
  got=$(md5_of "$clean")
  if [ "$got" != "$umd5" ]; then
    echo "FAILED    $target: $spath has md5 $got (CRs stripped), expected $umd5" >&2
    return 1
  fi
  if [ "$conv" = comb ]; then
    awk "$COMB_AWK" "$clean" > "$raw"
    cp "$raw" "$clean"
    got=$(md5_of "$clean")
    if [ "$got" != "$imd5" ]; then
      echo "FAILED    $target: the combinational part has md5 $got, expected $imd5" >&2
      return 1
    fi
  fi
  # No-clobber move, so that a file which appeared meanwhile is not replaced.
  chmod 644 "$clean"
  mv -n "$clean" "$target" 2>/dev/null || true
  if [ -e "$clean" ]; then
    echo "REFUSED   $target appeared meanwhile; not overwritten" >&2
    return 1
  fi
  echo "installed $target (md5 $imd5)"
}

failed=0; n=0
while read -r set path src spath umd5 conv imd5; do
  n=$((n + 1))
  install_one "$set" "$path" "$src" "$spath" "$umd5" "$conv" "$imd5" || failed=$((failed + 1))
done < <(selected)
[ "$failed" -eq 0 ] || die "$failed of $n files of sets [$sets] not installed"
echo "$n files of sets [$sets] in $dest"
case " $sets " in
  *" iscas85 "*|*" lgsynth91 "*|*" iscas89 "*|*" epfl "*) ;;
  *) echo "for the cases of cases.tsv: $0 --all $dest" ;;
esac
echo "for the other input families, see: $0 --families"
