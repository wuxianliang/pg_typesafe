#!/usr/bin/env python3
"""Primary loaded-image proof for one live backend PID."""
import argparse, os, subprocess, sys
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--pid", type=int, required=True)
p.add_argument("--dlsuffix", required=True)
p.add_argument("--runtime-curl", required=True)
p.add_argument("--evidence", required=True)
args = p.parse_args()
evidence = Path(args.evidence)
needle_ext = "typesafe." + args.dlsuffix
if sys.platform == "darwin":
    vmmap = subprocess.check_output(["/usr/bin/vmmap", str(args.pid)], text=True, errors="replace")
    (evidence / "jev-load-vmmap.txt").write_text(vmmap, encoding="utf-8")
    primary = vmmap
    try:
        lsof = subprocess.check_output(["/usr/sbin/lsof", "-p", str(args.pid)], text=True, errors="replace")
    except subprocess.CalledProcessError as exc:
        lsof = exc.output or ""
    (evidence / "jev-load-lsof.txt").write_text(lsof, encoding="utf-8")
else:
    primary = Path("/proc/%d/maps" % args.pid).read_text(encoding="utf-8", errors="replace")
    (evidence / "jev-load-maps.txt").write_text(primary, encoding="utf-8")
if needle_ext not in primary or args.runtime_curl not in primary:
    sys.exit("primary loaded-image listing missing %s or %s" % (needle_ext, args.runtime_curl))
