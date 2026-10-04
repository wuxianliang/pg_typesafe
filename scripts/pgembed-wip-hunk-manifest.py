#!/usr/bin/env python3
"""Emit git diff -U0 hunk identity JSONL for tracked dirty files. argv: pgembed_root"""
import hashlib, json, subprocess, sys
from pathlib import Path


def iter_hunks(diff: str):
    """Split `git diff -U0 -- <file>` stdout. File headers ignored."""
    lines = diff.replace("\r\n", "\n").replace("\r", "\n").splitlines(keepends=True)
    i = 0
    while i < len(lines) and not lines[i].startswith("@@"):
        i += 1
    while i < len(lines):
        header = lines[i]
        i += 1
        body_parts = []
        while i < len(lines) and not lines[i].startswith("@@"):
            if lines[i].startswith("diff --git"):
                break
            body_parts.append(lines[i])
            i += 1
        yield header, "".join(body_parts)


def normalize_body(body: str) -> bytes:
    return body.replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")


def body_sha256(body: str) -> str:
    return hashlib.sha256(normalize_body(body)).hexdigest()


def full_hunk_sha256(header: str, body: str) -> str:
    text = (header + body).replace("\r\n", "\n").replace("\r", "\n")
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def tracked_dirty(root: Path) -> list[str]:
    porc = subprocess.check_output(
        ["git", "-C", str(root), "status", "--porcelain", "-uall"], text=True
    )
    pinned = [
        "pgbuild/Makefile",
        "src/pgembed/__init__.py",
        "tools/generate_bundle_metadata.py",
    ]
    paths = []
    seen = set()
    for line in porc.splitlines():
        xy, rest = line[:2], line[3:]
        if xy == "??":
            continue
        rel = rest.split(" -> ", 1)[1] if " -> " in rest else rest
        if rel not in seen:
            seen.add(rel)
            paths.append(rel)
    for extra in pinned:
        if extra not in seen:
            seen.add(extra)
            paths.append(extra)
    return paths


def main() -> None:
    root = Path(sys.argv[1]).resolve()
    for rel in tracked_dirty(root):
        diff = subprocess.check_output(
            ["git", "-C", str(root), "diff", "-U0", "--", rel],
            text=True,
            errors="replace",
        )
        for header, body in iter_hunks(diff):
            rec = {
                "relpath": rel,
                "header": header.rstrip("\n"),
                "sha256": full_hunk_sha256(header, body),
                "body_sha256": body_sha256(body),
                "body": body,
            }
            sys.stdout.write(json.dumps(rec, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
