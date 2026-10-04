#!/usr/bin/env bash
# WI-3: freeze overlay mode, backup pginstall allowlisted files, D2 install,
# isolated O2 crash, one LIVE overlay, R1/R2/R3, pgembed API, allowlist/WIP proofs.
# Never runs pgembed make/all/bundle-metadata. Never touches user PGDATA.
set -euo pipefail

SRC=/Users/wxl/Projects/pg_typesafe
PGEMBED=/Users/wxl/Projects/pgembed
PGROOT=/Users/wxl/Projects/pgembed/src/pgembed/pginstall
STAMP=$PGEMBED/pgbuild/.postgres-bundle-config.stamp
LIVE=$PGROOT/share/pgembed/build-metadata.json
DURABLE=$SRC/docs/evidence/wi3
VENV_PY=/Users/wxl/Projects/pgembed/.venv/bin/python

if [[ -z "${JEV_EVIDENCE:-}" ]]; then
  JEV_EVIDENCE=/tmp/jev-overlay-evidence-$$
fi
export JEV_EVIDENCE
case "$JEV_EVIDENCE" in
  "$PGROOT"/*) echo "evidence inside PGROOT" >&2; exit 1 ;;
esac
mkdir "$JEV_EVIDENCE"

WI3_OK=0
BACKUP=$JEV_EVIDENCE/pginstall.backup

backup_pginstall() {
  mkdir "$BACKUP"
  python3 - "$PGROOT" "$BACKUP" <<'PY'
import os, shutil, sys
from pathlib import Path
root, dest = Path(sys.argv[1]), Path(sys.argv[2])
relpaths = [
    "lib/postgresql/typesafe.dylib",
    "lib/postgresql/typesafe.so",
    "share/postgresql/extension/typesafe.control",
    "share/postgresql/extension/typesafe--0.0.1.sql",
    "share/postgresql/extension/typesafe--0.1.0.sql",
    "share/postgresql/extension/typesafe--0.0.1--0.1.0.sql",
    "share/pgembed/build-metadata.json",
]
for rel in relpaths:
    src = root / rel
    if src.is_file():
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)
        print("backed_up", rel)
PY
}

restore_pginstall() {
  python3 - "$PGROOT" "$BACKUP" <<'PY'
import shutil, sys
from pathlib import Path
root, backup = Path(sys.argv[1]), Path(sys.argv[2])
if not backup.is_dir():
    raise SystemExit("backup missing")
for src in backup.rglob("*"):
    if src.is_file():
        rel = src.relative_to(backup)
        target = root / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)
        print("restored", rel.as_posix())
PY
}

cleanup() {
  if [[ "$WI3_OK" -ne 1 ]]; then
    echo "WI-3 failed; restoring pginstall allowlisted files from $BACKUP" >&2
    restore_pginstall || true
  fi
}
trap cleanup EXIT

echo "== 0. precheck import =="
"$VENV_PY" - <<'PY'
import pgembed
assert pgembed.has_extension("pg_textsearch") is False
print("import_ok", pgembed.has_extension("pg_typesafe"))
PY
"$VENV_PY" -c 'import pgembed; print(pgembed.has_extension("pg_typesafe"))' \
  | tee "$JEV_EVIDENCE/has_extension.pre.txt"

echo "== 0b. freeze WIP bytes and overlay.mode =="
cp "$PGEMBED/src/pgembed/__init__.py" "$JEV_EVIDENCE/init.pre.py"
cp "$PGEMBED/pgbuild/Makefile" "$JEV_EVIDENCE/pgbuild.Makefile.pre"
cp "$PGEMBED/tools/generate_bundle_metadata.py" "$JEV_EVIDENCE/generate.pre.py"
shasum -a 256 "$JEV_EVIDENCE/init.pre.py" "$JEV_EVIDENCE/pgbuild.Makefile.pre" "$JEV_EVIDENCE/generate.pre.py" \
  | tee "$JEV_EVIDENCE/wip.sha256.pre"
git -C "$PGEMBED" status --porcelain -uall | tee "$JEV_EVIDENCE/pgembed.porcelain.pre"
python3 "$PGEMBED/tools/generate_bundle_metadata.py" \
  --local-overlay --freeze-mode \
  --install-prefix "$PGROOT" | tee "$JEV_EVIDENCE/overlay.mode.printed"

echo "== 1. source / WIP / install manifests (pre) =="
python3 "$SRC/scripts/canonical_source_sha256.py" "$SRC" | tee "$JEV_EVIDENCE/source.sha256"
python3 "$SRC/scripts/pgembed-wip-manifest.py" "$PGEMBED" "$JEV_EVIDENCE/wip.pre.jsonl"
python3 "$SRC/scripts/pgembed-wip-hunk-manifest.py" "$PGEMBED" > "$JEV_EVIDENCE/wip.hunks.pre.jsonl"
python3 "$SRC/scripts/pgembed-pginstall-manifest.py" "$PGROOT" > "$JEV_EVIDENCE/pginstall.pre.jsonl"
cp "$STAMP" "$JEV_EVIDENCE/pgembed.stamp.pre"

echo "== backup allowlisted pginstall files =="
backup_pginstall

echo "== 2. overlay install (D2) =="
"$SRC/scripts/install-into-pgembed.sh" --stamp "$STAMP" --source "$SRC"

echo "== 3. O2 isolated crash then one live overlay =="
o2_rc=0
JEV_OVERLAY_CRASH_BEFORE_RENAME=1 python3 "$PGEMBED/tools/generate_bundle_metadata.py" \
  --local-overlay \
  --install-prefix "$PGROOT" \
  --pg-typesafe-source-dir "$SRC" \
  --stamp "$STAMP" \
  --output "$JEV_EVIDENCE/o2-work/share/pgembed/build-metadata.json" || o2_rc=$?
test "$o2_rc" -eq 91
unset JEV_OVERLAY_CRASH_BEFORE_RENAME
python3 - "$LIVE" "$JEV_EVIDENCE" <<'PY'
from pathlib import Path
import sys
live, evidence = Path(sys.argv[1]), Path(sys.argv[2])
mode = (evidence / "overlay.mode").read_text(encoding="utf-8").strip()
initial = evidence / "build-metadata.live-initial.json"
if mode == "live-existing":
    assert live.read_bytes() == initial.read_bytes()
else:
    assert not live.exists()
print("o2_did_not_mutate_real_live")
PY
python3 "$PGEMBED/tools/generate_bundle_metadata.py" \
  --local-overlay \
  --install-prefix "$PGROOT" \
  --pg-typesafe-source-dir "$SRC" \
  --stamp "$STAMP"
python3 "$SRC/scripts/pgembed-metadata-overlay-proof.py" \
  --live "$LIVE" --canonical "$(cat "$JEV_EVIDENCE/source.sha256")" \
  --pre "$JEV_EVIDENCE/build-metadata.pre.json" \
  --mode-file "$JEV_EVIDENCE/overlay.mode"
"$VENV_PY" -c \
  'import pgembed; assert pgembed.has_extension("pg_typesafe") is True; assert pgembed.has_extension("pg_textsearch") is False'

echo "== 4. bundled standalone/regression/Python suites =="
export KEEP_JEV_LOGS=1
JEV_CHECK_WORKDIR=$JEV_EVIDENCE/jev-check
mkdir "$JEV_CHECK_WORKDIR"
export JEV_CHECK_WORKDIR
"$SRC/scripts/pgembed-jev-check.sh"

echo "== 4b. pgembed API fresh/upgrade (isolated PGDATA, mock only) =="
"$VENV_PY" "$SRC/scripts/pgembed-api-fresh-upgrade.py" "$JEV_EVIDENCE/pgembed-api.json"

echo "== 5. allowlist + WIP protection + stamp =="
python3 "$SRC/scripts/pgembed-pginstall-manifest.py" "$PGROOT" > "$JEV_EVIDENCE/pginstall.post.jsonl"
python3 "$SRC/scripts/pgembed-allowlist-diff.py" \
  "$JEV_EVIDENCE/pginstall.pre.jsonl" "$JEV_EVIDENCE/pginstall.post.jsonl"
python3 "$SRC/scripts/pgembed-wip-manifest.py" "$PGEMBED" "$JEV_EVIDENCE/wip.post.jsonl"
python3 - "$JEV_EVIDENCE/wip.pre.jsonl" "$JEV_EVIDENCE/wip.post.jsonl" <<'PY'
import json, sys
pre = {json.loads(l)["relpath"]: json.loads(l) for l in open(sys.argv[1])}
post = {json.loads(l)["relpath"]: json.loads(l) for l in open(sys.argv[2])}
allow_src = {"tools/generate_bundle_metadata.py"}
assert set(pre) == set(post)
for k, rec in pre.items():
    if k in allow_src:
        continue
    assert rec == post[k], k
print("wip_file_manifest_ok")
PY
cmp "$JEV_EVIDENCE/init.pre.py" "$PGEMBED/src/pgembed/__init__.py"
cmp "$JEV_EVIDENCE/pgbuild.Makefile.pre" "$PGEMBED/pgbuild/Makefile"
python3 "$SRC/scripts/pgembed-protect-generator-hunks.py" \
  "$JEV_EVIDENCE/generate.pre.py" "$PGEMBED/tools/generate_bundle_metadata.py"
python3 "$SRC/scripts/pgembed-wip-hunk-manifest.py" "$PGEMBED" > "$JEV_EVIDENCE/wip.hunks.post.jsonl"
python3 "$SRC/scripts/pgembed-wip-hunk-diff.py" \
  "$JEV_EVIDENCE/wip.hunks.pre.jsonl" "$JEV_EVIDENCE/wip.hunks.post.jsonl"
cmp "$JEV_EVIDENCE/pgembed.stamp.pre" "$STAMP"

mkdir -p "$DURABLE"
python3 - "$JEV_EVIDENCE" "$DURABLE" <<'PY'
import shutil, sys
from pathlib import Path
src, dest = Path(sys.argv[1]), Path(sys.argv[2])
keep = [
    "overlay.mode", "source.sha256", "has_extension.pre.txt",
    "wip.sha256.pre", "pgembed.porcelain.pre", "wip.pre.jsonl", "wip.post.jsonl",
    "wip.hunks.pre.jsonl", "wip.hunks.post.jsonl",
    "pginstall.pre.jsonl", "pginstall.post.jsonl",
    "build-metadata.live-initial.json", "build-metadata.pre.json",
    "real-overlay.done", "jev-curl-provider.txt", "jev-curl-make-n.txt",
    "jev-typesafe-otool.txt", "jev-load.sql.out", "jev-load-vmmap.txt",
    "jev-load-lsof.txt", "pgembed-api.json", "stamp.json",
]
for name in keep:
    p = src / name
    if p.is_file():
        shutil.copy2(p, dest / name)
py = src / "jev-check" / "log" / "python.json"
if py.is_file():
    shutil.copy2(py, dest / "python.json")
print("durable_copy_ok", dest)
PY
cp "$LIVE" "$DURABLE/build-metadata.live-post.json"

WI3_OK=1
echo "WI-3 ok evidence=$JEV_EVIDENCE durable=$DURABLE"
