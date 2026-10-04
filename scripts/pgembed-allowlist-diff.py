#!/usr/bin/env python3
"""Allowlist diff for pginstall pre/post JSONL manifests."""
import json, sys
from pathlib import Path

FILE_ALLOW = {
    "lib/postgresql/typesafe.dylib",
    "lib/postgresql/typesafe.so",
    "share/postgresql/extension/typesafe.control",
    "share/postgresql/extension/typesafe--0.0.1.sql",
    "share/postgresql/extension/typesafe--0.1.0.sql",
    "share/postgresql/extension/typesafe--0.0.1--0.1.0.sql",
    "share/pgembed/build-metadata.json",
}
DIR_ALLOW = {
    "share/pgembed",
}


def load(path):
    recs = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            recs[rec["relpath"]] = rec
    return recs


def parents(relpath: str):
    parts = Path(relpath).as_posix().split("/")
    out = []
    for i in range(1, len(parts)):
        out.append("/" .join(parts[:i]) if False else "/".join(parts[:i]))
    return out


def fields(rec):
    return (rec["relpath"], rec["type"], rec.get("sha256"), rec.get("target"), rec.get("mode"))


def dir_ok(rec):
    return rec["type"] == "dir" and rec.get("mode") == "0o755" and rec.get("target") is None


def parent_ok(rel, post, pre):
    for parent in parents(rel):
        if parent in pre:
            if parent not in post:
                raise SystemExit("parent missing in post: %s" % parent)
            if fields(pre[parent]) != fields(post[parent]):
                if parent in DIR_ALLOW and dir_ok(post[parent]) and dir_ok(pre[parent]):
                    continue
                raise SystemExit("parent type/mode/target changed: %s" % parent)
        else:
            if parent not in DIR_ALLOW:
                raise SystemExit("new intermediate directory not allowlisted: %s" % parent)
            if parent not in post or not dir_ok(post[parent]):
                raise SystemExit("allowlisted parent missing or not dir/0755: %s" % parent)


def main() -> None:
    pre = load(sys.argv[1])
    post = load(sys.argv[2])
    for rel in pre:
        if rel not in post:
            raise SystemExit("post deleted pre relpath: %s" % rel)
    for rel, rec in post.items():
        if rel not in pre:
            if rec["type"] == "dir":
                if rel not in DIR_ALLOW:
                    raise SystemExit("new directory not allowlisted: %s" % rel)
                if not dir_ok(rec):
                    raise SystemExit("new directory must be dir/0755: %s" % rel)
            elif rec["type"] == "file":
                if rel not in FILE_ALLOW:
                    raise SystemExit("new file not allowlisted: %s" % rel)
            else:
                raise SystemExit("new symlink not allowed: %s" % rel)
            parent_ok(rel, post, pre)
            continue
        old = pre[rel]
        if fields(old) == fields(rec):
            continue
        if rec["type"] == "file" and rel in FILE_ALLOW and old["type"] == "file":
            if rec.get("target") != old.get("target") or rec.get("mode") != old.get("mode"):
                # codesign may keep mode; still require same type/file allowlist
                if rec.get("target") != old.get("target"):
                    raise SystemExit("allowlisted file target changed: %s" % rel)
            continue
        if rec["type"] == "dir" and rel in DIR_ALLOW:
            if not dir_ok(rec):
                raise SystemExit("share/pgembed must remain dir/0755: %s" % rel)
            continue
        raise SystemExit("disallowed field change: %s" % rel)
    print("pginstall_allowlist_ok")


if __name__ == "__main__":
    main()
