#!/usr/bin/env python3
"""Read-only O1 semantic comparison for the local overlay. Does not write."""
import argparse
import copy
import json
import os
import sys
from pathlib import Path

FORBIDDEN = {
    "--crash-before-rename",
    "--o2-work",
    "--output",
    "--install-prefix",
    "--local-overlay-from",
    "--live-from",
}


def strip_overlay_delta(doc):
    d = copy.deepcopy(doc)
    d.pop("generated_at", None)
    ex = d.setdefault("extensions", {})
    ex.pop("pg_typesafe", None)
    return d


def main() -> None:
    for flag in FORBIDDEN:
        if flag in sys.argv:
            raise SystemExit("forbidden writer flag on read-only proof: %s" % flag)
    p = argparse.ArgumentParser()
    p.add_argument("--pre", required=True)
    p.add_argument("--live", required=True)
    p.add_argument("--canonical", required=True)
    p.add_argument("--mode-file", required=True)
    args = p.parse_args()
    if "JEV_EVIDENCE" not in os.environ:
        raise SystemExit("JEV_EVIDENCE is required")
    evidence = Path(os.environ["JEV_EVIDENCE"])
    pre_path = Path(args.pre)
    live = Path(args.live)
    prefix = live.resolve().parent.parent.parent
    old = json.loads(pre_path.read_bytes())
    new = json.loads(live.read_text(encoding="utf-8"))
    mode = Path(args.mode_file).read_text(encoding="utf-8").strip()
    assert mode in ("live-existing", "bootstrap-generate")
    if mode == "live-existing":
        baseline = json.loads((evidence / "build-metadata.live-initial.json").read_bytes())
        assert old == baseline, "pre must be semantically identical to live-initial"
    else:
        cand_path = evidence / "build-metadata.generate-candidate.json"
        assert cand_path.is_file()
        assert pre_path.resolve() != cand_path.resolve()
        baseline = old
    assert "pg_textsearch" not in old.get("extensions", {})
    assert "pg_textsearch" not in new.get("extensions", {})
    assert strip_overlay_delta(new) == strip_overlay_delta(baseline)
    assert set(new["extensions"]) == set(old["extensions"]) | {"pg_typesafe"}
    rec = new["extensions"]["pg_typesafe"]
    assert rec["source_sha256"] == args.canonical
    assert rec["version"] == "0.1.0"
    assert rec["source_commit"] is None
    assert rec["source_ref"] == "local-overlay:typesafe"
    assert rec["built"] is True and rec["library"]
    assert not rec["library"].startswith("/")
    assert rec["library"].startswith("lib/postgresql/typesafe.")
    assert (prefix / rec["library"]).is_file()
    for name, rec_old in old["extensions"].items():
        if name != "pg_typesafe":
            assert new["extensions"][name] == rec_old, name
    print("o1_semantic_baseline_ok")


if __name__ == "__main__":
    main()
