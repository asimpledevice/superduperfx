#!/usr/bin/env python3
"""Retrain icache_order.ld: several restarts of icache_trg.py, each scored
offline in icsim, the best one written out.

The placer is greedy and noisy between restarts (one seed's layout is not a
reliable basis for anything), so the rule is: seeds 0..N-1, each scored on
the training builds only, best by geometric mean of misses relative to the
best seen for each build. Held-out builds are replayed and reported, never
used to choose.

  icache_sweep.py -o icache_order.ld
      --train ELF MAP TRACE [--train ...]     (the first is the build laid out)
      [--held ELF MAP TRACE ...] [--seeds 8] [--gap-cost 0.5] [--slack 12]

ELF/MAP: a build and its linker map (build/snesrecomp_interp.{elf,map}).
TRACE: its code-line trace from ares ($ARES_ICTRACE, the local icprof patch).
Needs icsim/icsim and icsim/trg (cc -O2 -o icsim/X icsim/X.c).
"""
import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))


def misses(trace, remap=None):
    out = subprocess.run([os.path.join(HERE, "icsim", "icsim"), trace] + ([remap] if remap else []),
                         capture_output=True, text=True, check=True).stdout
    return sum(int(l.split()[2]) for l in out.splitlines() if l.startswith("STAGE"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--train", nargs=3, action="append", required=True, metavar=("ELF", "MAP", "TRACE"))
    ap.add_argument("--held", nargs=3, action="append", default=[], metavar=("ELF", "MAP", "TRACE"))
    ap.add_argument("--seeds", type=int, default=8)
    ap.add_argument("--gap-cost", default="0.5")
    ap.add_argument("--slack", default="12")
    ap.add_argument("--nm", default="mips64-elf-nm")
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    builds = a.train + a.held
    td = tempfile.mkdtemp()

    def one(seed):
        ld = os.path.join(td, "s%d.ld" % seed)
        cmd = [sys.executable, os.path.join(HERE, "icache_trg.py")] + a.train[0] + ["-o", ld, "--nm", a.nm,
               "--gap-cost", a.gap_cost, "--slack", a.slack, "--seed", str(seed)]
        for b in a.train[1:]:
            cmd += ["--also"] + b
        for i, b in enumerate(builds):
            cmd += ["--remap", b[0], os.path.join(td, "s%d.%d" % (seed, i))]
        note = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip().splitlines()[-1]
        return ld, [misses(b[2], os.path.join(td, "s%d.%d" % (seed, i))) for i, b in enumerate(builds)], note

    now = [misses(b[2]) for b in builds]
    with ThreadPoolExecutor(a.jobs) as ex:
        res = list(ex.map(one, range(a.seeds)))
    nt = len(a.train)
    best = [min(r[1][i] for r in res) for i in range(nt)]
    score = [math.exp(sum(math.log(r[1][i] / best[i]) for i in range(nt)) / nt) for r in res]
    pick = min(range(a.seeds), key=lambda s: score[s])
    names = [os.path.basename(b[0]) for b in builds]
    print("simulated misses in each trace (train: %s; held out: %s)" % (", ".join(names[:nt]), ", ".join(names[nt:]) or "none"))
    print("  as built: %s" % " ".join("%d" % m for m in now))
    for s, (ld, m, note) in enumerate(res):
        print("  seed %d: %s  score %.3f%s  (%s)" % (s, " ".join("%d" % x for x in m), score[s],
                                                    " <- chosen" if s == pick else "", note))
    shutil.copy(res[pick][0], a.out)
    shutil.rmtree(td)


if __name__ == "__main__":
    main()
