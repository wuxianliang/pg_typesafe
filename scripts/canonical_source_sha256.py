#!/usr/bin/env python3
"""Print lowercase hex SHA-256 of the pg_typesafe install-source manifest."""
import hashlib, pathlib, sys
root = pathlib.Path(sys.argv[1]).resolve()
patterns = ("typesafe*.c", "typesafe*.h", "Makefile", "typesafe.control", "typesafe--*.sql")
files = []
for pat in patterns:
    files.extend(p for p in root.glob(pat) if p.is_file() and p.parent == root)
relpaths = sorted({p.relative_to(root).as_posix() for p in files})
h = hashlib.sha256()
for rel in relpaths:
    data = (root / rel).read_bytes()
    h.update(rel.encode("utf-8"))
    h.update(b"\0")
    h.update(str(len(data)).encode("ascii"))
    h.update(b"\0")
    h.update(data)
sys.stdout.write(h.hexdigest() + "\n")
