#!/usr/bin/env bash
# WI-2 standalone PG18 install + legacy regress + Jev pg_regress/Python harness.
# Not the WI-3 pgembed overlay entry. Bundled TAP is not enabled and is not claimed.
set -euo pipefail

SRC=$(cd "$(dirname "$0")/../.." && pwd)
PGROOT=${PGROOT:-/Users/wxl/Projects/pgembed/src/pgembed/pginstall}
PG_CONFIG=$PGROOT/bin/pg_config
PG_REGRESS=$PGROOT/lib/postgresql/pgxs/src/test/regress/pg_regress
export PGROOT SRC PG_CONFIG PG_REGRESS
export PATH=$PGROOT/bin:$PATH
FDW_CONTROL=$("$PG_CONFIG" --sharedir)/extension/postgres_fdw.control
export FDW_CONTROL
unset TYPESAFE_API_KEY
export TYPESAFE_API_KEY

test "$(command -v pg_config)" = "$PG_CONFIG"
"$PG_CONFIG" --version | grep -q 'PostgreSQL 18.4'
"$PGROOT/bin/postgres" --version | grep -q 'PostgreSQL) 18.4'
"$PGROOT/bin/psql" --version | grep -q '18.4'
command -v initdb | grep -q "$PGROOT/bin"
command -v pg_ctl  | grep -q "$PGROOT/bin"
test -x "$PG_REGRESS"
test -f "$FDW_CONTROL" || { echo "missing postgres_fdw.control at $FDW_CONTROL" >&2; exit 1; }

echo "== install typesafe into $PGROOT =="
make -C "$SRC" USE_PGXS=1 PG_CONFIG="$PG_CONFIG" -j
make -C "$SRC" USE_PGXS=1 PG_CONFIG="$PG_CONFIG" install

WORKDIR=${JEV_CHECK_WORKDIR:-$(mktemp -d /tmp/jev-check-XXXX)}
PGDATA=$WORKDIR/pgdata
SOCKDIR=$WORKDIR/sock
LOGDIR=$WORKDIR/log
MOCK=
export WORKDIR PGDATA SOCKDIR LOGDIR
mkdir -p "$SOCKDIR" "$LOGDIR"

cleanup() {
  if [[ -d ${PGDATA:-/nonexistent} ]]; then
    pg_ctl -D "$PGDATA" -m fast stop >/dev/null 2>&1 || true
  fi
  if [[ -n ${MOCK:-} ]]; then kill "$MOCK" 2>/dev/null || true; fi
  if [[ ${KEEP_JEV_LOGS:-0} != 1 ]]; then rm -rf "$WORKDIR"; fi
}
trap cleanup EXIT

initdb -D "$PGDATA" --auth=trust --no-sync --username=jev
PGPORT=
for p in $(seq 25432 25532); do
  if python3 -c 'import socket,sys; s=socket.socket(); s.bind(("127.0.0.1", int(sys.argv[1]))); s.close()' "$p"; then
    PGPORT=$p; break
  fi
done
test -n "$PGPORT" && test "$PGPORT" -gt 0
export PGPORT
pg_ctl -D "$PGDATA" -l "$LOGDIR/postgres.log" start \
  -o "-c listen_addresses='127.0.0.1' -c unix_socket_directories='$SOCKDIR' -c port=$PGPORT"
ready=0
for _ in $(seq 1 200); do
  if pg_isready -h "$SOCKDIR" -p "$PGPORT" -U jev; then ready=1; break; fi
  sleep 0.05
done
test "$ready" -eq 1
printf '%s\n' "$PGPORT" > "$WORKDIR/pg.port"
export PGHOST=$SOCKDIR PGUSER=jev PGDATABASE=postgres
export PGHOST PGPORT PGUSER PGDATABASE PGDATA

python3 "$SRC/test/jev/mock_api.py" \
  --bind 127.0.0.1 --port 0 \
  --ready-file "$WORKDIR/mock.json" \
  --log-file "$LOGDIR/mock.log" &
MOCK=$!
export MOCK
python3 - "$WORKDIR/mock.json" <<'PY'
import json, sys, time
path = sys.argv[1]
for _ in range(200):
    try:
        d = json.load(open(path, encoding="utf-8"))
        if int(d["port"]) > 0 and d["url"].endswith("/v1/systemone"):
            sys.exit(0)
    except (OSError, ValueError, KeyError, json.JSONDecodeError):
        time.sleep(0.05)
sys.exit("mock ready-file timeout")
PY
export JEV_API_URL=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["url"])' "$WORKDIR/mock.json")
export MOCK_STATS_URL=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print("http://%s:%s/_mock/stats"%(d["host"], d["port"]))' "$WORKDIR/mock.json")
export MOCK_HOST=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["host"])' "$WORKDIR/mock.json")
export MOCK_PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$WORKDIR/mock.json")
export FDW_PORT=$PGPORT
export JEV_HANDSHAKE_DIR=$WORKDIR

createdb contrib_regression_typesafe
createdb contrib_regression_jev
createdb jev_fdw_remote
psql -d jev_fdw_remote -v ON_ERROR_STOP=1 -c \
  "CREATE TABLE jev_remote(id int PRIMARY KEY, name text); INSERT INTO jev_remote VALUES (1,'Berlin'),(2,'Tokyo');"
bash -c 'test -n "$PGPORT" && test -n "$JEV_API_URL" && test -n "$FDW_PORT" && test -n "$PGHOST" && test -n "$JEV_HANDSHAKE_DIR"'

chmod +x "$SRC/scripts/psql-jev-launcher.sh"

echo "== R1 legacy typesafe regress =="
"$PG_REGRESS" --use-existing --host="$PGHOST" --port="$PGPORT" --user="$PGUSER" \
  --bindir="$PGROOT/bin" --dlpath="$PGROOT/lib/postgresql" \
  --inputdir="$SRC" --expecteddir="$SRC" --outputdir="$LOGDIR/reg_typesafe" \
  --dbname=contrib_regression_typesafe typesafe

echo "== R2 jev sql regress =="
set +e
"$PG_REGRESS" --use-existing --host="$PGHOST" --port="$PGPORT" --user="$PGUSER" \
  --bindir="$PGROOT/bin" --dlpath="$PGROOT/lib/postgresql" \
  --inputdir="$SRC/test/jev" --expecteddir="$SRC/test/jev" --outputdir="$LOGDIR/reg_jev" \
  --launcher="$SRC/scripts/psql-jev-launcher.sh" \
  --dbname=contrib_regression_jev \
  00_server_version 01_basic 02_errors 03_streaming 04_relations
R2=$?
set -e
if [[ $R2 -ne 0 ]]; then
  echo "R2 failed" >&2
  if [[ -f $LOGDIR/reg_jev/regression.diffs ]]; then
    echo "---- regression.diffs (head) ----"
    head -n 200 "$LOGDIR/reg_jev/regression.diffs" || true
  fi
fi

echo "== R3 python suite =="
set +e
python3 "$SRC/test/jev/run_python_suite.py"
R3=$?
set -e

psql -d postgres -c 'DROP DATABASE IF EXISTS jev_fdw_remote' >/dev/null 2>&1 || true

echo "WORKDIR=$WORKDIR"
echo "LOGDIR=$LOGDIR"
echo "JEV_API_URL=$JEV_API_URL"
echo "R2=$R2 R3=$R3"
if [[ -f $LOGDIR/python.json ]]; then
  python3 -c 'import json,sys; print(json.load(open(sys.argv[1])))' "$LOGDIR/python.json"
fi
test "$R2" -eq 0 && test "$R3" -eq 0
