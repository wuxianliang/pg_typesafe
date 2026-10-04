#!/usr/bin/env python3
"""Compare WIP hunk identity multisets. argv: pre.jsonl post.jsonl"""
import json, re, sys
from collections import Counter

ALLOWED_DEFS = {
    "freeze_overlay_mode",
    "read_frozen_mode",
    "ensure_share_pgembed",
    "reconstruct_pg_typesafe",
    "atomic_write",
    "build_pre",
    "run_overlay",
    "local_overlay_cli",
}
DEF_RE = re.compile(r"^def ([A-Za-z_][A-Za-z0-9_]*)\(", re.M)
PROTECTED_EQUAL = {"pgbuild/Makefile", "src/pgembed/__init__.py"}
GENERATOR = "tools/generate_bundle_metadata.py"


def load(path):
    recs = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                recs.append(json.loads(line))
    return recs


def identities(recs, relpath=None):
    items = []
    for rec in recs:
        if relpath is not None and rec["relpath"] != relpath:
            continue
        items.append((rec["relpath"], rec["body_sha256"]))
    return Counter(items)


def main() -> None:
    pre = load(sys.argv[1])
    post = load(sys.argv[2])
    for rel in PROTECTED_EQUAL:
        a = identities(pre, rel)
        b = identities(post, rel)
        if a != b:
            missing = a - b
            extra = b - a
            sys.stderr.write(
                "protected hunk identity mismatch %s missing=%s extra=%s\n"
                % (rel, missing, extra)
            )
            raise SystemExit(1)
    pre_g = identities(pre, GENERATOR)
    post_g = identities(post, GENERATOR)
    missing_g = pre_g - post_g
    if missing_g:
        sys.stderr.write("generator lost protected identities: %s\n" % missing_g)
        raise SystemExit(1)
    added = post_g - pre_g
    post_by_id = {(rec["relpath"], rec["body_sha256"]): rec for rec in post}
    for ident, count in added.items():
        rec = post_by_id[ident]
        body = rec.get("body") or ""
        header = rec.get("header") or ""
        names = set(DEF_RE.findall(body))
        names.update(DEF_RE.findall(header))
        if not (names & ALLOWED_DEFS):
            sys.stderr.write(
                "new generator hunk %s missing overlay def; names=%s header=%s\n"
                % (ident, sorted(names), rec.get("header"))
            )
            raise SystemExit(1)
        if count != 1:
            sys.stderr.write("duplicate new generator identity %s count=%s\n" % (ident, count))
            raise SystemExit(1)
    print("wip_hunk_identities_ok added=%d" % sum(added.values()))


if __name__ == "__main__":
    main()
