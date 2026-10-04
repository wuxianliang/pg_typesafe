#!/usr/bin/env bash
set -eu
: "${JEV_API_URL:?}" "${FDW_PORT:?}" "${PGUSER:?}"
psql_bin=$1
shift
# 禁止 -v ON_ERROR_STOP=1：pg_regress expected 含 ERROR 行，停掉会杀会话、截断后续语句。
exec "$psql_bin" \
  -v jev_api_url="$JEV_API_URL" \
  -v FDW_PORT="$FDW_PORT" \
  -v USER="$PGUSER" \
  "$@"
