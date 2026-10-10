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
#   aralia     aralia/TREE.xml, the 43 Aralia fault trees in Open-PSA MEF
#              (openpra-org/aralia-fault-tree-dataset, CC BY-SA 4.0), and
#              each converted by tools/mef2blif.py under Storm-dft's two
#              orders: aralia/TREE.dfs.blif, aralia/TREE.tdlr.blif and their
#              .probs files (see FAULT TREES below). Needs python3.
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
# checkout of ispras/hdl-benchmarks, lsils=DIR a checkout of lsils/benchmarks,
# aralia=DIR a checkout of openpra-org/aralia-fault-tree-dataset (each at the
# commit below), or fiser=DIR the directory MCNC.7z and LGSynth91.7z were
# extracted into (so DIR/MCNC/Combinational/blif and DIR/LGSynth91/blif
# exist). Any of them give the same bytes.
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
# FAULT TREES. The aralia set is the dataset at commit 9a50e0c (2025-05-13),
# the last before 0101a52 (2026-05-13, the dataset's version 2) inlined the
# passthrough gates of 16 trees -- gates whose body is one basic event or
# gate. That changes no function and no DFS order, but it moves those events
# up a level of the TDLR order, which then differs on 11 trees from the one
# Storm-dft used in BDDs Strike Back (NFM 2022), whose Galileo files keep the
# passthrough gates. At 9a50e0c both orders are Storm-dft's on all 40 trees of
# that evaluation, and nodes_before is the size it published (README.md,
# "Fault trees"). Every XML file must have its recorded MD5; it is installed
# as aralia/TREE.xml and converted by tools/mef2blif.py (as TREE.xml, whose
# name is in the header line it writes) with --order dfs and --order tdlr, and
# every BLIF and .probs file must have its recorded MD5 too. The trees are CC
# BY-SA 4.0, and so is what is converted from them; this script and the
# converter are what is distributed, not the trees.
#
# --families prints the sources of the other families (McNet, relprod, QBF,
# the EPFL arithmetic circuits, and the fault trees of BDDs Strike Back) with
# the commands that fetch them. They are large, they are inputs to other
# harnesses than sylvan_bench, or their licence (GPL-3.0) is not one to
# redistribute inputs under, and this script does not download them.
set -euo pipefail

CUDD_COMMIT=f54f533303640afd5dbe47a05ebeabb3066f2a25
ISPRAS_COMMIT=10c3fb51d0810ce862659211862660dcc270a6af
LSILS_COMMIT=0060e156826e733d69bf5b3322d1bdd0d03a1f9a
ARALIA_COMMIT=9a50e0c2a087ac16fc18342d3f927aa0f9938105
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/tools

url_of() {  # SOURCE PATH
  case "$1" in
    cudd)   echo "https://raw.githubusercontent.com/cuddorg/cudd/$CUDD_COMMIT/$2" ;;
    ispras) echo "https://raw.githubusercontent.com/ispras/hdl-benchmarks/$ISPRAS_COMMIT/$2" ;;
    lsils)  echo "https://raw.githubusercontent.com/lsils/benchmarks/$LSILS_COMMIT/$2" ;;
    aralia) echo "https://raw.githubusercontent.com/openpra-org/aralia-fault-tree-dataset/$ARALIA_COMMIT/$2" ;;
  esac
}

ALL_SETS="c880 iscas85 lgsynth91 iscas89 epfl aralia"

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

# The aralia set, one line per tree: the MD5 of data/openpsa/TREE.xml, then of
# the four files tools/mef2blif.py makes of it: TREE.dfs.blif, TREE.dfs.probs,
# TREE.tdlr.blif, TREE.tdlr.probs. It adds five lines to the manifest per tree,
# the conversion field of a converted file saying mef2blif-ORDER.
ARALIA=$(cat <<'EOF'
baobab1   62b322fc445610cb8797e69abf8ec1d1 78dce8403db84d40418c9ca385637ab6 8c9479a83a142a347c7768e847889c09 9dffe500a127a3d1cbe491e500ebfa7d 10b7ed3269a427da3ef451ca96a654ab
baobab2   35b332c4f559544b4fefe07d3605bcef b1125e4e4f7cc6030c4b25784430b002 46ac77b0ed9f70ef958cfb00fbb31942 f78e95e1147dd8043f902749e79225e0 a9518108d420c7e520720e7094b4d77a
baobab3   4861add461087f66fad90f9722d15fa2 c906ba0e2dff4c920837bc3fbc0787f0 0aa9d01cfb2e01bfeb191f6e422dff8d a4e62760ca3baf42bb84511925e18c1e 6a6c8548191da3681d2f910310aa1a62
cea9601   db456d2d2fcbe2758adf99c851aa20e1 eb79b2c160a0444a4b4f25e515c48008 e9d28d024f9c3b1b3555d1055cbef99f 07945f54ee0f69cab145cd7331c70314 0b11725d4e80b500c7bd025d0eed9d73
chinese   3ad6d61a9898c7a089d7fe62684f7a91 2ee38ab427033197eb55a1d0434bdc85 fdacb2c302a2a39f1f2fff95af609c92 4738362d44c42ef4ecd3ae8c02ded69d fadf1414376d6d5578675018fb56aa26
das9201   e97c66ef4d4556657530d4b6d8863267 9e4b1137ff2e3b9af9b778422d579d80 aa7da594021df5c480af4211cac0f1ef d58bc5b1fe7392645f0eab99d38daa3b 506524c1bec5f34c166501e1b5b96efa
das9202   f582883c0659b02f50e0394b00c24951 8814a25d710921c88a3901f686222dd9 125158478e21780a7ce90fc6566d8acc f6893f277f1e9e5adc06244189a691fe 6dc5d5ce0352833701f78e5c41c69250
das9203   7e0dcff1c4345e3d79e7b2cc19736c02 c62fa304ff93ec1df3483bc5054dc9d8 1f07f6d2bd0e7c8c36b992f280196ef7 8558ddf841da75d9fa939f93289d2b14 5d501d6f1c553dfe0f458bdfef7cd297
das9204   9f0f49f47fa811f240e24b5d1e9f57b1 f275f2835fa0a053312091894a09bf1f 58a73da086f3a1386f9b9a6e85d6a159 f3031d2d146cc8d33b72c3c18cfb1162 2bf9e3ad37dd05045236e7df6f60b7e2
das9205   d714f0816b1a9015a5e3a89dbf43f21a b79224992c8d2d6e416d1c2fa6619109 4965483a61ea39d2adc63581a5e80e53 39438cb3a35c94454c5efd47fb2c63af c4bc1ec572d7f94e5ae3b8ca02ad1d0a
das9206   812c5d6719fd0173b124fe5c1fdf03b9 8fa31949f32140cf7a3310161c1f7c54 3df943ddfbec2785b7d8837d751205cf 1653387dc09e3bbd01852680b87bbe35 777f9e736f9b4bae333455d37884ff0f
das9207   fde7f5c237c3ef6524a68bff42da4496 da0b69341e85d8f1e4d1aeed4a1b9d8d 3f61a59bc32497a18a9f61af04231d81 30475181266b64d3bccf05b5a144cc99 de2ff06237ebf4fd38ad2f4c2e0da76b
das9208   2d1b6364c49529e6bca5bb40f7a272d6 8432a40562a12843a3845cc3197b7936 a7c36472550609acddbfab6bf91e7dbe df7f8c0c878ec96d313cfe119438a110 c1dc6fd0a7edaa80d735b34a8a9a02ed
das9209   ceadfcab39290e5a33981658243f0fec 9880a95d930b13d3fd8d0c967e66db93 40946952ecb7a92f46f9de66b84b1687 3bdde17dd4f7ffb5aa0db393ffbb7abb 0edeaa03698f8d05354efce27bedb697
das9601   07a2dc733484d33ec76765e5caf9c1d6 2e2eed7da560947c2e6066a94fd348e7 0db40e952ea982951396aae862ad4211 f1d06f467e74d94326163f25138c96e6 b0fb8b299dedddc47f990ea02f737352
das9701   0d7465868bab8db34ba5098e99a1382d d8fd2e53b482a8e44a5bb5c706d03373 c63e26cfa4c6c97d5ab5be91e2c1780b ef0cd6be72931e402dfa5f4bf7a50a42 baa0af22c0051863c53101418e3e923e
edf9201   06a1351dfc44e9d7c47ea1cb36d206a4 1fd9c960041ffddf40052dfac6320fac a853b3ee0422738422378be22ca4080b 495f031ffc5b5e9be3bf528e1e2518d3 6ea64c9288f541c5c56110e7878a059d
edf9202   1e1149d9fce04e61dec1d85c1b99ff09 5268f2d7f4fb3d09164e7e6e13d56fca 7681bf3ca87237daf11e55bd87a9566c bccad18f42a0070a56d788151ba6db2a 4a329edb8aaba500e812ef81754d149d
edf9203   aff0336049f93752f5ec9ed86308b42f 16698ab88567a056ca81790ec3551110 9daab874825fb1ce01115c9753a2b36c c27eb193a6b2b03ca683d0cfdca45957 4f1c49f4f2f9ecb529427f7888453c94
edf9204   74db16d21b4f1c43eed40f0c8fe49085 275a7e79c49656113a817ecd01657f24 c1e5fd51d59a7fa53ecd34ee7ce8b444 b8b02deafdaab834afe480da7de2621a dcaab89fb6a0b14b6b1c5c3bde18e848
edf9205   630055f613012751f64aaf98f115c0e5 bdfd043d7fd06b7168f4580ce02793ff 503225c6493ce90055498a724d93e775 85aedcd591f0adf2592c7197b56867ef 4ee18ae6589c865a9365066585ff1bb6
edf9206   1baeae2ed88ff6e80c02bcb2d1ebec4b 2638ab3a563aea1f520e5377103d1896 79c26f12cc2015cc4f37e1eba6194de5 4bb542edf56f3a7d9cd3064efcf85b71 628e9ad457e75353d72e72f8eb5bf801
edfpa14b  45536943b67c0123f6259174b3aea347 c47d4776d36ea62a5a3a57222fa78bc0 62c49eed0fa83af462f01539528e52c0 4301b751ed34fe9a143c0bab278a2e2c bee292db5c6206aa1cb89b25d6955c13
edfpa14o  9032887df67465b76e8db2ed88733b1f 06dcbb33d805478d07ba516e1e68ef53 dfaaea4dec4aa3679097d65cd58f3de8 452f4c4a9143887df0b7ca54279d2d85 a44caaa208349bf8fdae627e1f8d5277
edfpa14p  60b926c6a5c0f294a8bf636ba735649e d0f1b38b177cb9c8c3f2c2d3f802ab7f dd0af9a61775764d2ff30c89d0aa001c 44de0be7c2138e36ad8f6fe06d6b069a 4dd895f96745beb168ab9a1589503041
edfpa14q  78eb58f4b41cd73ff3d20b192f2857ab 3c146b58ef39c4b6556938b96ade6f36 4b1ba095034cd7cfcf8fb4d1aa5d948a 0d6ac489f2cb20629e54d586336c9784 e51381bf53c8404343bf6069b33d0164
edfpa14r  d76e7e3aa2bc946a243769a06d514fb7 7e5629db5bfa70ddf5b3f313cdf34ab7 bf932a19c4b37726a98d2584a0822bcd b832faf11e4dd0c8babeddfb8110e5e6 907d79dacbb45581407e23fb18b30bf1
edfpa15b  6972b79c4139f4acc284eef4c850e502 f4f837636281a1dbf1640fc68d534523 c4d9807903e694a02ad3a9cb9e2da33f 104c2ab52c5e2794eb446f00c8118eb6 ef3838e695d849163f9ef78d84feb5d9
edfpa15o  4b769335e3916d62facf78cd3c2f0097 52f6add2bf5569b2264d3c7919ec900a fd72ee1a4b93b18a7b8df43ec4366a44 c3bd805a8d2dd0ff4ad35f4fa239731e dda03188686df73512e9cbdb4bb17247
edfpa15p  7bee093e90e0c01f3baa8e7e5afde644 2bc0a9e5a2bfdc675e6c5b09fb32fe49 d70f269c50c8a6fbe4be47217470c6fd 41d7b394c86a68dba1fd846d8228252a 69879480aec062bc472766d2039a0971
edfpa15q  c290c1fe8a9c82783432ab51c34093b4 c7059b9de422542120cf1465a0ded6a9 f84935229c37ad48df9ada4551fe82a3 ae072fc3e1b3c2f62fd48d0af10a3c4b 4b993c57e65c51d430104d14f743b2ef
edfpa15r  2dabc22f2aa26d765eda47f10b0fb7ba 4611329d18721281d9b5d53a2a6502c9 e2a99ab49241961a168ebf8052b31431 1ecbe0a2853a8d4c78fe11746a34ca48 f9021c8e6aa8cdd65d7de68d9da99d3c
elf9601   378953752421f3e03478cb6053d1a8ac 5d799a4409bd0dcff88bb8d2b450ff37 724605f42305cf1e730f3e04f11aab6b c83642047ee3c9471eabc5b6417145ec bc261993d2ce0a802d84f36262635722
ftr10     c176d178e69789b81570b5d7d6582524 908a1f7351b87bdb1b5140b23ebed5c5 ec393191d4a82e3192d012c5f5d8febd 572634c25905ef4b2ff6a346fae71efb 711f05474b636daaf3ff17ca1fc9c07c
isp9601   746e300eda8ae63e5481436a1e6a2a4f 51f0ca5eb179f68934f4b22e20a07808 8e5c73ab448a169536b09d0f46291902 068b840685bdd525e6069018d1c1e9ff 52f9c93a374e1af72301e6c55f2be0f2
isp9602   21894f5043c56aea0ff34521a411dbba 88272324b9a8a51e8c7d36c0a1a6a0c2 8370d83274d7247aa7a0ec0554ad3a14 10409113e7e205dfc4351da9f6f7da99 b65d3cdbf637eed9e75e353b5f79c05f
isp9603   e270dc940281d5592486dc5ddc88bd1b 0ad2a184a9ab441aeffdda22dc58805f f797e175a0e979ba2b36244b3ed98220 875f099cba6c28a86c61f6a6e7743e49 94dfe4691549d2c22a3c98ff0373a0c9
isp9604   2c99c894407f5a3655d40b1709654ca1 399868bd4ebda5e912d813059699d806 6d53cb15cb2a6d752393994e0c6c9415 416703f9b5cf0569e604d46fbebc9872 aff7e400b91b4b0494492e789cedeb92
isp9605   fb1dbe4388a64267b3e1b9970070a15e 598348b303a515144d65ce5c17e76c92 859d390364c590f3bd58cf65f7fcd073 c50f4afc7bd00d2c1500914797599c45 5bc2514162cb258277bd3a7a06e04202
isp9606   c68a68351f7f17d5aa6c70a401582538 876bd3b53d4edcbabc1e0edbaf7536eb 109fad447ed18ee6fafcdf959d46feaf 2da89ea66f7c76d741ff835774064587 dae0a4cac096f781d1cefffb7e34f360
isp9607   917dfe3530362596be741e1862fb4b73 6de3359c52ceb6912e1f3f5df1b0937e 0fd9cf416d58fa2e14253e06a5b883b5 33a53fc3355311eb23e135f4f90e7880 73bfa934b4b2e0e29b7bf35135eb9032
jbd9601   2fe87a8feaf2e0ee5ee54236f87f0967 218c30119d539cd2506f97940c99285a f48cb0fc5f2b07599a80293002cdc2c2 cb1179c3b6b78b99db755e485170883c d03c1e39bfc92236e9c147631e037a35
nus9601   83beba15a38aa025551afb6ffbe11c3e c07b1af5e90faf34428adb3a3b42f958 8770bf0de9d9c175a70993cf3ac93e05 5e1ae8002437ab18f1adc83ba6f23272 d70b0fed90e315bdccaa09b794213ab7
EOF
)
aralia_manifest() {
  local tree xmd5 dblif dprobs tblif tprobs
  while read -r tree xmd5 dblif dprobs tblif tprobs; do
    [ -n "$tree" ] || continue
    echo "aralia aralia/$tree.xml aralia data/openpsa/$tree.xml $xmd5 - $xmd5"
    echo "aralia aralia/$tree.dfs.blif aralia data/openpsa/$tree.xml $xmd5 mef2blif-dfs $dblif"
    echo "aralia aralia/$tree.dfs.probs aralia data/openpsa/$tree.xml $xmd5 mef2blif-dfs $dprobs"
    echo "aralia aralia/$tree.tdlr.blif aralia data/openpsa/$tree.xml $xmd5 mef2blif-tdlr $tblif"
    echo "aralia aralia/$tree.tdlr.probs aralia data/openpsa/$tree.xml $xmd5 mef2blif-tdlr $tprobs"
  done <<< "$ARALIA"
}
MANIFEST+=$'\n'$(aralia_manifest)

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

Fault trees of BDDs Strike Back: DOCUMENTED, NOT FETCHED (GPL-3.0).
  The artifact of Basgoze, Volk, Katoen, Khan, Stoelinga, "BDDs Strike Back:
  Efficient Analysis of Static and Dynamic Fault Trees" (NFM 2022),
  doi:10.5281/zenodo.6390998 (version 1.0.1), is GPL-3.0, its fault trees
  included, so they are not inputs this harness redistributes.

    curl -fL -o nfm22-artifact.zip \
      "https://zenodo.org/records/6390998/files/artifact.zip?download=1"
    echo "2b8779cf28db573967fc633dce02b2e1  nfm22-artifact.zip" | md5sum -c   # 1557554 bytes
    python3 -m zipfile -e nfm22-artifact.zip nfm22/
    # -> nfm22/artifact/sft-benchmarks/{aralia,random_scram,random_scram_prob,
    #    industry,railway,sprinkler}/*.dft, nfm22/artifact/tools/translate.py,
    #    nfm22/artifact/results/sft-benchmarks/sft_sizes.csv (Storm-dft's sizes)
    for f in nfm22/artifact/sft-benchmarks/random_scram/*.dft; do
      t=$(basename "$f" .dft)
      for o in dfs tdlr; do
        benchmark/sift/tools/mef2blif.py "$f" "random/$t.$o.blif" --order "$o"
      done
    done

  sft-benchmarks/aralia/*.dft are 40 of the trees of the aralia set
  (fetch-inputs.sh --sets aralia), as Galileo files: converted the same way,
  they give the same BLIF inputs in the same orders (a basic event's
  probability is then 1 - exp(-lambda), equal to the MEF value to about
  1e-16). random_scram (128 trees of 150 events) and random_scram_prob (33 of
  500) are the random trees of the paper; mef2blif.py reads them, and
  sft_sizes.csv has Storm-dft's DFS and TDLR sizes for them. The sizes of the
  Aralia trees from it are in ../fta-reference.tsv.
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

src_cudd=""; src_ispras=""; src_lsils=""; src_fiser=""; src_aralia=""
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
                  aralia=?*) src_aralia=${2#*=} ;;
                  fiser=?*)  src_fiser=${2#*=} ;;
                  *) die "--src wants cudd=DIR, ispras=DIR, lsils=DIR, aralia=DIR or fiser=DIR" ;;
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
  printf '%-10s %-26s %-32s %-13s %-32s %s\n' set file "md5 (installed)" conv "md5 (download, CR stripped)" url
  while read -r set path src spath umd5 conv imd5; do
    [ -n "$set" ] || continue
    printf '%-10s %-26s %-32s %-13s %-32s %s\n' "$set" "$path" "$imd5" "$conv" "$umd5" "$(url_of "$src" "$spath")"
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
    aralia) dir=$src_aralia ;;
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
work=$(mktemp -d "${TMPDIR:-/tmp}/fetch-inputs.XXXXXX") || die "mktemp failed"
trap 'rm -f ${tmpfiles[@]+"${tmpfiles[@]}"}; rm -rf "$work"' EXIT
case " $sets " in
  *" aralia "*) command -v python3 > /dev/null 2>&1 || die "the aralia set needs python3 (tools/mef2blif.py)" ;;
esac

# fetch_once SOURCE PATH OUT -- fetch_raw, but each file only once per run:
# the five files of a fault tree come from one download.
fetch_once() {
  local cached="$work/raw/$1/$2"
  if [ ! -f "$cached" ]; then
    mkdir -p "$(dirname "$cached")"
    fetch_raw "$1" "$2" "$cached.part" && mv "$cached.part" "$cached" || return 1
  fi
  cp "$cached" "$3"
}

# convert_tree CLEAN TREE ORDER -- the directory holding mef2blif.py's
# TREE.ORDER.blif and TREE.ORDER.probs of the checked download CLEAN, made
# once per run.
convert_tree() {
  local dir="$work/conv/$2.$3"
  if [ ! -f "$dir/$2.$3.probs" ]; then
    mkdir -p "$dir"
    cp "$1" "$dir/$2.xml"
    python3 -I "$TOOLS/mef2blif.py" "$dir/$2.xml" "$dir/$2.$3.blif" --order "$3" > /dev/null || return 1
  fi
  echo "$dir"
}

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
  if ! fetch_once "$src" "$spath" "$raw"; then
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
  elif [[ "$conv" == mef2blif-* ]]; then
    local tree=${spath##*/} cdir
    tree=${tree%.xml}
    if ! cdir=$(convert_tree "$clean" "$tree" "${conv#mef2blif-}"); then
      echo "FAILED    $target: tools/mef2blif.py could not convert $spath" >&2
      return 1
    fi
    cp "$cdir/${path##*/}" "$clean"
    got=$(md5_of "$clean")
    if [ "$got" != "$imd5" ]; then
      echo "FAILED    $target: tools/mef2blif.py made md5 $got, expected $imd5" >&2
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
  *" iscas85 "*|*" lgsynth91 "*|*" iscas89 "*|*" epfl "*|*" aralia "*) ;;
  *) echo "for the cases of cases.tsv: $0 --all $dest" ;;
esac
echo "for the other input families, see: $0 --families"
