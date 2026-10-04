#!/usr/bin/env python3
"""SHA-256 WIP files. argv: pgembed_root out.jsonl"""
import hashlib, json, os, subprocess, sys
from pathlib import Path
root = Path(sys.argv[1]).resolve()
out = open(sys.argv[2], "w", encoding="utf-8")
porc = subprocess.check_output(["git", "-C", str(root), "status", "--porcelain", "-uall"], text=True)
paths = []
for line in porc.splitlines():
    # "XY path" or "XY orig -> path"
    rel = line[3:]
    if " -> " in rel:
        rel = rel.split(" -> ", 1)[1]
    paths.append(rel)
# always pin these even if clean
for extra in ("pgbuild/Makefile", "src/pgembed/__init__.py",
              "tools/generate_bundle_metadata.py",
              "pgbuild/.postgres-bundle-config.stamp"):
    if extra not in paths:
        paths.append(extra)
def emit(rel, kind, sha, target=None, mode=None):
    rec = {"relpath": rel, "type": kind, "sha256": sha, "target": target,
           "mode": mode}
    out.write(json.dumps(rec, sort_keys=True) + "\n")
seen = set()
for rel in paths:
    p = root / rel
    if rel in seen:
        continue
    seen.add(rel)
    mode = oct(p.lstat().st_mode & 0o777) if p.exists() or p.is_symlink() else None
    if p.is_symlink():
        emit(rel, "symlink", None, os.readlink(p), mode)
    elif p.is_file():
        emit(rel, "file", hashlib.sha256(p.read_bytes()).hexdigest(), None, mode)
    elif p.is_dir():
        emit(rel, "dir", None, None, mode)
        for q in sorted(p.rglob("*")):
            r2 = q.relative_to(root).as_posix()
            if r2 in seen:
                continue
            seen.add(r2)
            m2 = oct(q.lstat().st_mode & 0o777)
            if q.is_symlink():
                emit(r2, "symlink", None, os.readlink(q), m2)
            elif q.is_file():
                emit(r2, "file", hashlib.sha256(q.read_bytes()).hexdigest(), None, m2)
            elif q.is_dir():
                emit(r2, "dir", None, None, m2)
