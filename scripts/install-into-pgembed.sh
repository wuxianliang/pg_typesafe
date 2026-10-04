#!/usr/bin/env bash
set -euo pipefail
SOURCE=/Users/wxl/Projects/pg_typesafe
STAMP=/Users/wxl/Projects/pgembed/pgbuild/.postgres-bundle-config.stamp
while [[ $# -gt 0 ]]; do
  case "$1" in
    --source) SOURCE=$2; shift 2 ;;
    --stamp) STAMP=$2; shift 2 ;;
    *) echo "usage: install-into-pgembed.sh --stamp PATH --source PATH" >&2; exit 1 ;;
  esac
done
EVIDENCE=${JEV_EVIDENCE:-/tmp/jev-overlay-evidence}
mkdir -p "$EVIDENCE"
LOAD=
LOAD_PY=
cleanup() {
  if [[ -n "${LOAD_PY:-}" ]]; then kill "$LOAD_PY" 2>/dev/null || true; fi
  if [[ -n "${LOAD:-}" && -d "$LOAD/data" ]]; then
    "$PGROOT/bin/pg_ctl" -D "$LOAD/data" -m fast stop >/dev/null 2>&1 || true
  fi
  [[ -n "${LOAD:-}" ]] && rm -rf "$LOAD"
}
trap cleanup EXIT
cp "$STAMP" "$EVIDENCE/pgembed.stamp.pre"

python3 - "$STAMP" "$EVIDENCE/stamp.json" <<'PY'
import json, sys
d = {}
for line in open(sys.argv[1], encoding="utf-8"):
    line = line.strip()
    if not line or line.startswith("#") or "=" not in line:
        continue
    k, v = line.split("=", 1)
    d[k] = v
json.dump(d, open(sys.argv[2], "w"), indent=2)
PY
eval "$(python3 - "$EVIDENCE/stamp.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
def sh(k, envk=None):
    print("%s=%s" % (envk or k.upper(), json.dumps(d[k])))
sh("install_prefix", "PGROOT")
sh("pg_net_curl_prefix", "PG_NET_CURL_PREFIX")
sh("host_os", "HOST_OS")
sh("dlsuffix", "DLSUFFIX")
PY
)"
export PGROOT PG_NET_CURL_PREFIX HOST_OS DLSUFFIX
export PG_CONFIG="$PGROOT/bin/pg_config"
export PATH="$PGROOT/bin:$PATH"
unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR

if [[ -n "$PG_NET_CURL_PREFIX" ]]; then
  export TYPESAFE_CURL_CFLAGS="-I${PG_NET_CURL_PREFIX}/include"
  export TYPESAFE_CURL_LIBS="-L${PG_NET_CURL_PREFIX}/lib -lcurl"
  BUILD_CURL_SUBSTR=$PG_NET_CURL_PREFIX
  RUNTIME_CURL=$PGROOT/lib/libcurl.so.4
  test -f "$RUNTIME_CURL" || { echo "bundled libcurl missing: $RUNTIME_CURL" >&2; exit 1; }
else
  case "$HOST_OS" in
    Darwin)
      export TYPESAFE_CURL_CFLAGS=""
      export TYPESAFE_CURL_LIBS="-lcurl"
      BUILD_CURL_SUBSTR=system-lcurl
      RUNTIME_CURL=/usr/lib/libcurl.4.dylib
      ;;
    *)
      echo "stamp has empty pg_net_curl_prefix on $HOST_OS; refuse" >&2
      exit 1
      ;;
  esac
fi
export BUILD_CURL_SUBSTR RUNTIME_CURL
{
  printf 'build host_os=%s prefix=%s cflags=%s libs=%s\n' \
    "$HOST_OS" "$PG_NET_CURL_PREFIX" "$TYPESAFE_CURL_CFLAGS" "$TYPESAFE_CURL_LIBS"
  printf 'runtime expect=%s\n' "$RUNTIME_CURL"
} | tee "$EVIDENCE/jev-curl-provider.txt"

export PKG_CONFIG=/usr/bin/false
make -C "$SOURCE" clean
make -C "$SOURCE" -n "typesafe.$DLSUFFIX" > "$EVIDENCE/jev-curl-make-n.txt"
if grep -E 'pkg-config|Cellar/curl|/opt/homebrew/.*/curl|/opt/curl' "$EVIDENCE/jev-curl-make-n.txt"; then
  echo "dry-run used ambient curl provider" >&2; exit 1
fi
grep -F -- "$TYPESAFE_CURL_LIBS" "$EVIDENCE/jev-curl-make-n.txt" || {
  echo "dry-run missing TYPESAFE_CURL_LIBS" >&2; exit 1
}

make -C "$SOURCE" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" 2>&1 | tee "$EVIDENCE/jev-curl-make.txt"
if grep -E 'pkg-config|Cellar/curl|/opt/homebrew/.*/curl|/opt/curl' "$EVIDENCE/jev-curl-make.txt"; then
  echo "make used ambient curl provider" >&2; exit 1
fi
make -C "$SOURCE" install
LIB=$("$PG_CONFIG" --pkglibdir)/typesafe.$DLSUFFIX
test -f "$LIB"

if [[ "$(uname -s)" == Darwin ]]; then
  otool -L "$LIB" | awk 'NR>1 {print $1}' | grep -E '^/' | while IFS= read -r ref; do
    case "$ref" in
      "$PGROOT"/*)
        install_name_tool -change "$ref" "@loader_path/$(basename "$ref")" "$LIB"
        ;;
    esac
  done
  codesign --force --sign - --timestamp=none "$LIB"
  otool -L "$LIB" | tee "$EVIDENCE/jev-typesafe-otool.txt"
  grep -F /usr/lib/libcurl "$EVIDENCE/jev-typesafe-otool.txt" || exit 1
  if grep -E 'Cellar/curl|/opt/homebrew/.*/libcurl' "$EVIDENCE/jev-typesafe-otool.txt"; then exit 1; fi
elif [[ "$(uname -s)" == Linux ]]; then
  command -v patchelf >/dev/null
  # 单引号：RPATH 必须是字面量 $ORIGIN:$ORIGIN/..（与 pgembed relativize 对 postgresql/*.so 相同）
  # 禁止 '\$ORIGIN'（会把反斜杠写进 DT_RUNPATH）
  patchelf --set-rpath '$ORIGIN:$ORIGIN/..' "$LIB"
  readelf -d "$LIB" | tee "$EVIDENCE/jev-typesafe-readelf.txt"
  grep -F '$ORIGIN:$ORIGIN/..' "$EVIDENCE/jev-typesafe-readelf.txt" || exit 1
  ldd "$LIB" | tee "$EVIDENCE/jev-typesafe-ldd.txt"
  grep -F "$RUNTIME_CURL" "$EVIDENCE/jev-typesafe-ldd.txt" || exit 1
  if grep -F "$PG_NET_CURL_PREFIX/lib" "$EVIDENCE/jev-typesafe-ldd.txt"; then
    echo "ldd still maps libcurl to build prefix" >&2; exit 1
  fi
fi

# ---- 单一持久 client：CREATE EXTENSION/LOAD、打印 PID、保持阻塞；禁止第二段短命 psql ----
LOAD=$EVIDENCE/load-cluster
rm -rf "$LOAD"
mkdir -p "$LOAD/sock"
initdb -D "$LOAD/data" --auth=trust --no-sync --username=jev
LOAD_PORT=
for p in $(seq 25432 25532); do
  if python3 -c 'import socket,sys; s=socket.socket(); s.bind(("127.0.0.1", int(sys.argv[1]))); s.close()' "$p"; then
    LOAD_PORT=$p; break
  fi
done
test -n "$LOAD_PORT"
export LOAD_PORT
"$PGROOT/bin/pg_ctl" -D "$LOAD/data" -l "$LOAD/postgres.log" -w start \
  -o "-c listen_addresses='' -c unix_socket_directories='$LOAD/sock' -c port=$LOAD_PORT"
for _ in $(seq 1 100); do
  "$PGROOT/bin/pg_isready" -h "$LOAD/sock" -p "$LOAD_PORT" -U jev && break
  sleep 0.05
done
export PGHOST=$LOAD/sock PGPORT=$LOAD_PORT PGUSER=jev LC_ALL=C LC_MESSAGES=C
LOAD_PID_FILE=$LOAD/backend.pid
LOAD_READY=$LOAD/maps.ready
LOAD_DONE=$LOAD/maps.done
rm -f "$LOAD_PID_FILE" "$LOAD_READY" "$LOAD_DONE"
python3 - "$LOAD_PID_FILE" "$LOAD_READY" "$LOAD_DONE" "$EVIDENCE/jev-load.sql.out" <<'PY' &
import os, sys, time, subprocess
pid_file, ready_file, done_file, out_file = sys.argv[1:]
psql = os.path.join(os.environ["PGROOT"], "bin", "psql")
proc = subprocess.Popen(
    [psql, "-d", "postgres", "-v", "ON_ERROR_STOP=1", "-A", "-t", "-q"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
SENT = "__JEV_LOAD_END__"
def run(sql):
    proc.stdin.write(sql.rstrip(";") + ";\nSELECT '%s';\n" % SENT)
    proc.stdin.flush()
    lines = []
    while True:
        line = proc.stdout.readline()
        if line == "":
            raise SystemExit("persistent psql died: %s" % proc.stderr.read())
        if line.strip() == SENT:
            break
        lines.append(line)
    return "".join(lines)
chunks = []
chunks.append(run("CREATE EXTENSION typesafe"))
chunks.append(run("SELECT jev_version()"))
chunks.append(run("SHOW server_version"))
pid = run("SELECT pg_backend_pid()").strip()
assert pid.isdigit() and int(pid) > 1, pid
chunks.append(pid + "\n")
open(out_file, "w", encoding="utf-8").write("".join(chunks))
open(pid_file, "w", encoding="utf-8").write(pid + "\n")
proc.stdin.write("SELECT pg_sleep(3600);\n")
proc.stdin.flush()
time.sleep(0.2)
open(ready_file, "w", encoding="utf-8").write("1\n")
for _ in range(2000):
    if os.path.exists(done_file):
        break
    time.sleep(0.05)
else:
    raise SystemExit("maps inspection timeout")
proc.stdin.close()
proc.terminate()
proc.wait(timeout=10)
PY
LOAD_PY=$!
export LOAD_PY
for _ in $(seq 1 200); do
  [[ -f $LOAD_READY ]] && break
  sleep 0.05
done
test -f "$LOAD_READY"
grep -q '18.4' "$EVIDENCE/jev-load.sql.out"
LOAD_PID=$(cat "$LOAD_PID_FILE")
export LOAD_PID
test "$LOAD_PID" -gt 1
# 必须在这个仍阻塞的同一 backend 上取加载镜像。禁止再开短命 psql。
python3 "$SOURCE/scripts/d2_loaded_images.py" \
  --pid "$LOAD_PID" \
  --dlsuffix "$DLSUFFIX" \
  --runtime-curl "$RUNTIME_CURL" \
  --evidence "$EVIDENCE"
: > "$LOAD_DONE"
wait "$LOAD_PY"
LOAD_PY=
# 此处不手动 stop/rm：EXIT trap 清理。
cmp "$EVIDENCE/pgembed.stamp.pre" "$STAMP"
