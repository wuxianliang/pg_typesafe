#!/usr/bin/env python3
"""Walk PGROOT; JSONL to stdout. Does not follow symlinks. Records dirs."""
import hashlib, json, os, sys
from pathlib import Path
root = Path(sys.argv[1]).resolve()
recs = []
for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
    # 目录 symlink：记录 type=symlink，不下降
    keep = []
    for d in dirnames:
        p = Path(dirpath) / d
        rel = p.relative_to(root).as_posix()
        st = p.lstat()
        if p.is_symlink():
            recs.append({"relpath": rel, "type": "symlink",
                         "sha256": None, "target": os.readlink(p),
                         "mode": oct(st.st_mode & 0o777)})
        else:
            recs.append({"relpath": rel, "type": "dir",
                         "sha256": None, "target": None,
                         "mode": oct(st.st_mode & 0o777)})
            keep.append(d)
    dirnames[:] = keep
    for name in filenames:
        p = Path(dirpath) / name
        rel = p.relative_to(root).as_posix()
        st = p.lstat()
        if p.is_symlink():
            recs.append({"relpath": rel, "type": "symlink",
                         "sha256": None, "target": os.readlink(p),
                         "mode": oct(st.st_mode & 0o777)})
        elif p.is_file():
            recs.append({"relpath": rel, "type": "file",
                         "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                         "target": None,
                         "mode": oct(st.st_mode & 0o777)})
for rec in sorted(recs, key=lambda r: r["relpath"]):
    sys.stdout.write(json.dumps(rec, sort_keys=True) + "\n")
