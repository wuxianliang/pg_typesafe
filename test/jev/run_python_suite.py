#!/usr/bin/env python3
"""WI-2 Jev parity / security suite. stdlib + $PGROOT/bin/psql only."""

from __future__ import annotations

import errno
import fcntl
import json
import os
import re
import select
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

psql = os.path.join(os.environ["PGROOT"], "bin", "psql")
createdb = os.path.join(os.environ["PGROOT"], "bin", "createdb")
dropdb = os.path.join(os.environ["PGROOT"], "bin", "dropdb")
SQLSTATE_TOKEN = re.compile(rb"^JEV_SQLSTATE ([0-9A-Z]{5})$")
MAX_STDOUT, MAX_STDERR, STDERR_IDLE_S = 1 << 20, 64 << 10, 0.2
RESULTS = []
_NO_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class FrameError(RuntimeError):
    pass


class PersistentPsql:
    """One backend. SQLSTATE from :SQLSTATE only. Binary multiplex both fds."""

    def __init__(self, db="contrib_regression_jev"):
        env = os.environ.copy()
        env["LC_ALL"] = env["LC_MESSAGES"] = "C"
        self.proc = subprocess.Popen(
            [psql, "-d", db, "-v", "ON_ERROR_STOP=0", "-A", "-t", "-q",
             "-v", "jev_api_url=" + os.environ["JEV_API_URL"]],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            bufsize=0, env=env)
        for fd in (self.proc.stdout.fileno(), self.proc.stderr.fileno()):
            fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)
        self._out = b""
        self._err = b""
        self._meta(r"\set VERBOSITY verbose")
        self._meta(r"\set SHOW_CONTEXT never")
        self.backend_pid = self.scalar("SELECT pg_backend_pid();")
        assert int(self.backend_pid) > 1

    def close(self):
        if self.proc.poll() is None:
            try:
                self.proc.stdin.close()
            except Exception:
                pass
            try:
                self.proc.terminate()
            except Exception:
                pass
            try:
                self.proc.wait(timeout=5)
            except Exception:
                pass

    def _write(self, s):
        self.proc.stdin.write(s.encode("utf-8"))
        self.proc.stdin.flush()

    def _pump(self, timeout):
        fds = [self.proc.stdout.fileno(), self.proc.stderr.fileno()]
        r, _, _ = select.select(fds, [], [], timeout)
        for fd in r:
            try:
                chunk = os.read(fd, 4096)
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    continue
                raise
            if not chunk:
                continue
            if fd == self.proc.stdout.fileno():
                self._out += chunk
                if len(self._out) > MAX_STDOUT:
                    raise FrameError("stdout diagnostics exceeded %d" % MAX_STDOUT)
            else:
                self._err += chunk
                if len(self._err) > MAX_STDERR:
                    raise FrameError("stderr diagnostics exceeded %d" % MAX_STDERR)

    def _take_lines(self):
        lines, rest = [], self._out
        while True:
            i = rest.find(b"\n")
            if i < 0:
                break
            lines.append(rest[:i].rstrip(b"\r"))
            rest = rest[i + 1:]
        self._out = rest
        return lines

    def _wait_end(self, token, deadline_s):
        end_at = time.monotonic() + deadline_s
        marker = ("JEV_END " + token).encode()
        while time.monotonic() < end_at:
            self._pump(max(0.01, end_at - time.monotonic()))
            for line in self._take_lines():
                if line == marker:
                    return
        raise FrameError("missing meta END %s" % token)

    def _meta(self, cmd):
        token = uuid.uuid4().hex
        self._write("%s\n\\echo JEV_END %s\n" % (cmd, token))
        self._wait_end(token, 10.0)
        idle = time.monotonic() + STDERR_IDLE_S
        while time.monotonic() < idle:
            self._pump(max(0.0, idle - time.monotonic()))
        self._err = b""

    def send(self, sql):
        """Write framed SQL without waiting (X2/D7 handshake)."""
        self._token = uuid.uuid4().hex
        body = sql.rstrip().rstrip(";")
        self._write(
            "\\echo JEV_START %s\n%s;\n\\echo JEV_SQLSTATE :SQLSTATE\n\\echo JEV_END %s\n"
            % (self._token, body, self._token))

    def wait_frame(self, deadline_s=60.0):
        end_at = time.monotonic() + deadline_s
        saw_start = saw_state = saw_end = False
        sqlstate = None
        rows = []
        start = ("JEV_START " + self._token).encode()
        end = ("JEV_END " + self._token).encode()
        while time.monotonic() < end_at and not saw_end:
            self._pump(max(0.01, end_at - time.monotonic()))
            for line in self._take_lines():
                if line == start:
                    saw_start = True
                    continue
                m = SQLSTATE_TOKEN.match(line)
                if m:
                    sqlstate = m.group(1).decode("ascii")
                    saw_state = True
                    continue
                if line == end:
                    saw_end = True
                    continue
                if saw_start and not saw_state:
                    rows.append(line.decode("utf-8", "replace"))
            if self.proc.poll() is not None and not saw_end:
                raise FrameError("backend died (ProcDie/FATAL stays fatal): %s"
                                 % self._err.decode("utf-8", "replace")[-512:])
        idle_end = time.monotonic() + STDERR_IDLE_S
        while time.monotonic() < idle_end:
            self._pump(max(0.0, idle_end - time.monotonic()))
        err = self._err.decode("utf-8", "replace")
        self._err = b""
        if not (saw_start and saw_state and saw_end and sqlstate):
            raise FrameError("missing :SQLSTATE frame start=%s state=%s end=%s err=%s"
                             % (saw_start, sqlstate, saw_end, err[:512]))
        if "FATAL:" in err and sqlstate == "57014":
            raise FrameError("FATAL diagnostics must not be mapped to 57014: %s" % err[:512])
        return "\n".join(rows), sqlstate, err[:MAX_STDERR]

    def _run(self, sql):
        self.send(sql)
        return self.wait_frame()

    def scalar(self, sql):
        out, st, err = self._run(sql)
        if st != "00000":
            raise FrameError("unexpected error %s: %s" % (st, err))
        return out.strip()

    def expect_sqlstate(self, sql, state):
        """Capture :SQLSTATE token before the next command. No PL/pgSQL EXCEPTION."""
        if not re.fullmatch(r"[0-9A-Z]{5}", state):
            raise FrameError("wanted SQLSTATE is not [0-9A-Z]{5}: %r" % state)
        if state == "57014" and ("WHEN OTHERS" in sql or re.search(r"\bEXCEPTION\b", sql, re.I)):
            raise FrameError("57014 must not be faked through PL/pgSQL WHEN OTHERS")
        out, st, err = self._run(sql)
        if st == "00000":
            raise FrameError("unexpected success, wanted %s" % state)
        if st != state:
            raise FrameError("expected SQLSTATE %s, got %s: %s" % (state, st, err))
        return out, err

    def assert_same_backend(self):
        assert self.scalar("SELECT pg_backend_pid();") == self.backend_pid


def _sql_payload(text):
    lines = [ln for ln in (text or "").splitlines() if ln not in ("SET", "RESET")]
    return "\n".join(lines).strip()


def q(sql, db="contrib_regression_jev"):
    return _sql_payload(subprocess.check_output(
        [psql, "-d", db, "-v", "ON_ERROR_STOP=1", "-tAc", sql], text=True))


def q_err(sql, db="contrib_regression_jev"):
    p = subprocess.run(
        [psql, "-d", db, "-v", "ON_ERROR_STOP=1", "-tAc", sql],
        text=True, capture_output=True)
    return p.returncode, p.stdout, p.stderr


def mock_url(path):
    return os.environ["MOCK_STATS_URL"].replace("/_mock/stats", path)


def mock(path, data=None):
    url = mock_url(path)
    body = None if data is None else json.dumps(data).encode()
    headers = {"Content-Type": "application/json"} if body else {}
    req = urllib.request.Request(url, data=body, method="POST", headers=headers)
    return _NO_PROXY.open(req, timeout=30).read()


def mock_stats():
    return json.loads(_NO_PROXY.open(os.environ["MOCK_STATS_URL"], timeout=30).read().decode())


def record(name, ok, detail=""):
    RESULTS.append({"name": name, "ok": bool(ok), "detail": detail})
    status = "PASS" if ok else "FAIL"
    print("%s %s%s" % (status, name, (" " + detail) if detail else ""), flush=True)


def run_case(name, fn):
    try:
        fn()
        record(name, True)
    except Exception as exc:
        record(name, False, "%s: %s" % (type(exc).__name__, exc))
    except SystemExit as exc:
        record(name, False, "SystemExit: %s" % exc)


def sql_escape(s):
    return s.replace("'", "''")


def ensure_extension(sess):
    sess.scalar("CREATE EXTENSION IF NOT EXISTS typesafe")
    sess.scalar("SELECT jev_version()")
    sess.scalar("SET jev.api_url = '%s'" % sql_escape(os.environ["JEV_API_URL"]))
    sess.scalar("SET jev.api_key = 'test-key'")
    sess.scalar("SET jev.notices = off")


def ensure_heap(sess):
    sess.scalar("CREATE TABLE IF NOT EXISTS jev_heap (id int PRIMARY KEY, name text)")
    n = int(sess.scalar("SELECT count(*) FROM jev_heap"))
    if n == 0:
        sess.scalar("INSERT INTO jev_heap VALUES (1,'Berlin'),(2,'Tokyo')")


def jev_debug_stats(sess):
    keys = ("session_bytes", "_b_cache", "_b_job", "_b_fetched", "_b_http",
            "_b_skipped", "_b_conn", "_reserved", "_http_easies")
    out = {k: int(sess.scalar("SELECT (jev_stats()->>'%s')::bigint" % k)) for k in keys}
    out["cached"] = int(sess.scalar("SELECT (jev_stats()->>'cached_answers')::int"))
    accounted = sum(out[k] for k in ("_b_cache", "_b_job", "_b_fetched",
                                     "_b_http", "_b_skipped", "_b_conn"))
    assert out["_reserved"] == 0, out
    assert out["session_bytes"] == accounted, out
    return out


def test_env_and_token(sess):
    assert q("SELECT current_setting('server_version')").startswith("18.4")
    assert q("SHOW data_directory").startswith(os.environ["PGDATA"])
    assert q("SELECT 1") == "1"
    sess.expect_sqlstate("SELECT 1 FROM nonexistent_rel_jev", "42P01")
    sess.assert_same_backend()


def test_legacy_http():
    mock("/_mock/reset", {})
    url = os.environ["JEV_API_URL"]
    base = url.rsplit("/", 2)[0]
    setup = (
        "SET typesafe.api_key = 'test-key'; "
        "SET typesafe.mock_response = ''; "
        "SET typesafe.timeout_ms = 5000; "
    )
    opts = "'{\"billing\":\"Payments\",\"technical\":\"Bugs\",\"sales\":\"Pricing\"}'::jsonb"
    choice = q(
        setup + "SET typesafe.endpoint = '%s'; SELECT choice FROM typesafe_classify("
        "'Help! My payouts have been failing for 3 days.',"
        "'Which team should handle this?', %s);" % (sql_escape(url), opts))
    assert choice == "technical", choice
    noul = q(
        setup + "SET typesafe.endpoint = '%s'; SELECT noul FROM typesafe_detect("
        "'x', 'urgent?');" % sql_escape(url))
    assert noul.startswith("0.42"), noul
    score = q(
        setup + "SET typesafe.endpoint = '%s'; SELECT score FROM typesafe_score("
        "'x', 'how frustrated?', ARRAY['Calm','Frustrated','Very angry']);" % sql_escape(url))
    assert score.startswith("1.6"), score
    rows = q(
        setup + "SET typesafe.endpoint = '%s'; SELECT count(*), sum(noul), sum(input_tokens) "
        "FROM typesafe_detect_many(ARRAY['a','b','c','d','e'], 'q?');" % sql_escape(url))
    assert rows.split("|")[0] == "5", rows
    rc, _out, err = q_err(
        setup + "SET typesafe.api_key = 'bad-key'; SET typesafe.endpoint = '%s'; "
        "SELECT typesafe_noul('x', 'y');" % sql_escape(url))
    assert rc != 0 and "401" in err, err


def test_http_paths():
    mock("/_mock/reset", {})
    base = "http://%s:%s" % (os.environ["MOCK_HOST"], os.environ["MOCK_PORT"])
    setup = (
        "SET typesafe.api_key = 'test-key'; "
        "SET typesafe.mock_response = ''; "
    )
    flaky = q(
        setup + "SET typesafe.timeout_ms = 8000; SET typesafe.endpoint = '%s/flaky'; "
        "SELECT typesafe_noul('x', 'urgent?');" % sql_escape(base))
    assert flaky.startswith("0.42"), flaky
    rc, _o, err = q_err(
        setup + "SET typesafe.timeout_ms = 8000; SET typesafe.endpoint = '%s/always429'; "
        "SELECT typesafe_noul('x', 'urgent?');" % sql_escape(base))
    assert rc != 0 and "429" in err, err
    rc, _o, err = q_err(
        setup + "SET typesafe.timeout_ms = 5000; SET typesafe.endpoint = '%s/huge'; "
        "SELECT typesafe_noul('x', 'urgent?');" % sql_escape(base))
    assert rc != 0 and "exceeds" in err, err
    rc, _o, err = q_err(
        setup + "SET typesafe.timeout_ms = 5000; SET typesafe.endpoint = '%s/garbage'; "
        "SELECT typesafe_noul('x', 'urgent?');" % sql_escape(base))
    assert rc != 0, err
    rc, _o, err = q_err(
        setup + "SET typesafe.timeout_ms = 5000; SET typesafe.endpoint = '%s/wrongshape'; "
        "SELECT typesafe_noul('x', 'urgent?');" % sql_escape(base))
    assert rc != 0, err
    rc, _o, err = q_err(
        setup + "SET typesafe.timeout_ms = 1000; SET typesafe.endpoint = '%s/stall'; "
        "SELECT typesafe_noul('x', 'urgent?');" % sql_escape(base))
    assert rc != 0 and re.search(r"Timeout|timed out", err, re.I), err
    url = os.environ["JEV_API_URL"]
    rc, _o, err = q_err(
        "SET jev.api_url = '%s'; SET jev.api_key = 'wrong-key'; SET jev.notices = off; "
        "SELECT jev_prob(t, 'x') FROM (SELECT 1 AS id) t;" % sql_escape(url))
    assert rc != 0 and "401" in err and "invalid api key" not in err.lower(), err
    rc, _o, err = q_err(
        "SET jev.api_url = '%s'; SET jev.api_key = 'test-key'; SET jev.notices = off; "
        "SELECT jev_prob(t, 'please trigger422') FROM (SELECT 1 AS id) t;" % sql_escape(url))
    assert rc != 0 and "422" in err, err


def test_004_guc_placeholder():
    names = [
        "jev.api_key", "jev.api_url", "jev.batch_size", "jev.concurrency",
        "jev.max_prefetch_rows", "jev.max_rows_per_statement",
        "jev.max_chars_per_statement", "jev.max_session_memory_kb",
    ]
    values = {
        "jev.api_key": "attacker-key",
        "jev.api_url": "http://127.0.0.1:9/v1/systemone",
        "jev.batch_size": "3",
        "jev.concurrency": "2",
        "jev.max_prefetch_rows": "12",
        "jev.max_rows_per_statement": "4",
        "jev.max_chars_per_statement": "9",
        "jev.max_session_memory_kb": "2048",
    }
    q("DROP ROLE IF EXISTS jev_attacker")
    q("CREATE ROLE jev_attacker LOGIN")
    q("GRANT CONNECT ON DATABASE contrib_regression_jev TO jev_attacker")
    q("GRANT USAGE ON SCHEMA public TO jev_attacker")
    q("GRANT EXECUTE ON FUNCTION jev_version() TO jev_attacker")
    env = os.environ.copy()
    env["PGUSER"] = "jev_attacker"
    env["LC_ALL"] = env["LC_MESSAGES"] = "C"
    for name in names:
        val = values[name]
        script = (
            "SET %s = '%s';\n"
            "SELECT jev_version();\n"
            "SET %s = '%s';\n" % (name, val, name, val)
        )
        p = subprocess.run(
            [psql, "-d", "contrib_regression_jev", "-v", "ON_ERROR_STOP=0", "-A", "-t"],
            input=script, text=True, capture_output=True, env=env)
        err = p.stderr + p.stdout
        assert "permission denied" in err.lower() or "42501" in err, (name, err)
        q("ALTER ROLE jev_attacker SET %s = '%s'" % (name, val))
        if name == "jev.api_key":
            pshow = subprocess.run(
                [psql, "-d", "contrib_regression_jev", "-v", "ON_ERROR_STOP=0",
                 "-c", "SELECT jev_version();", "-c", "SHOW jev.api_key"],
                text=True, capture_output=True, env=env)
            shown = (pshow.stderr + pshow.stdout).lower()
            assert pshow.returncode != 0 or "permission denied" in shown, (
                pshow.stdout, pshow.stderr)
        else:
            shown = subprocess.check_output(
                [psql, "-d", "contrib_regression_jev", "-tAc", "SHOW %s" % name],
                text=True, env=env).strip()
            assert shown == val, (name, shown, val)
        q("ALTER ROLE jev_attacker RESET %s" % name)


def test_005_upgrade():
    db = "jev_upgrade"
    subprocess.call([dropdb, "--if-exists", db])
    subprocess.check_call([createdb, db])
    try:
        q("CREATE EXTENSION typesafe VERSION '0.0.1'", db=db)
        ver = q("SELECT extversion FROM pg_extension WHERE extname='typesafe'", db=db)
        assert ver == "0.0.1", ver
        rc, _o, err = q_err("SELECT jev_version()", db=db)
        assert rc != 0, err
        q("SELECT typesafe_last_request() IS NULL OR true", db=db)
        q("ALTER EXTENSION typesafe UPDATE TO '0.1.0'", db=db)
        assert q("SELECT jev_version()", db=db) == "0.2.0"
        assert q("SELECT extversion FROM pg_extension WHERE extname='typesafe'", db=db) == "0.1.0"
        attrs = q(
            "SELECT string_agg(proname || ':' || provolatile::text || proparallel::text || "
            "CASE WHEN proisstrict THEN 's' ELSE 'n' END, ',' ORDER BY proname) "
            "FROM pg_proc p JOIN pg_language l ON l.oid=p.prolang "
            "WHERE l.lanname = 'c' AND proname IN ('jev','jev_prob','jev_choice','jev_score','jev_score_norm',"
            "'jev_confidence','jev_eval','jev_stats','jev_cache_clear','jev_version')",
            db=db)
        assert "jev:sun" in attrs and "jev_prob:sun" in attrs, attrs
        assert "jev_cache_clear:vun" in attrs and "jev_version:iun" in attrs, attrs
        assert q("SELECT COUNT(*) FROM pg_proc WHERE proname IN ("
                 "'jev','jev_prob','jev_choice','jev_score','jev_score_norm',"
                 "'jev_confidence','jev_eval','jev_stats','jev_cache_clear',"
                 "'jev_version','typesafe_noul','typesafe_classify')", db=db) == "12"
        execs = q(
            "SELECT COUNT(*) FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace "
            "JOIN pg_extension e ON e.extnamespace=n.oid AND e.extname='typesafe' "
            "JOIN LATERAL aclexplode(COALESCE(p.proacl, acldefault('f', p.proowner))) a ON true "
            "WHERE (p.proname = 'jev' OR p.proname LIKE 'jev\\_%') AND a.grantee=0",
            db=db)
        assert execs == "0", execs
    finally:
        subprocess.call([dropdb, "--if-exists", db])


def test_d8b_mock_bodies():
    stats = mock_stats()
    bodies = stats.get("bodies") or []
    texts = [json.dumps(b.get("json")).lower() for b in bodies if b.get("json")]
    joined = " ".join(texts)
    assert "berlin" in joined and "tokyo" in joined, "R2 did not send heap/mv/view/part/ft rows"
    assert stats.get("requests", 0) >= 5
    rc, _o, err = q_err("SELECT jev_prob(1, 'x')")
    assert rc != 0
    assert "0A000" not in err, err
    assert ("42883" in err or "22703" in err or "42804" in err
            or "composite" in err.lower() or "function jev_prob" in err.lower()), err


def test_g1_g2(sess):
    ensure_extension(sess)
    sess.scalar("CREATE TABLE IF NOT EXISTS jev_g (id int PRIMARY KEY, name text)")
    sess.scalar("DELETE FROM jev_g")
    sess.scalar("INSERT INTO jev_g SELECT g, 'row' || g FROM generate_series(1, 6) g")
    sess.scalar("SELECT jev_cache_clear()")
    sess.scalar("RESET jev.batch_size")
    mock("/_mock/reset", {})
    errors0 = int(sess.scalar("SELECT (jev_stats()->>'errors')::int"))
    sess.scalar("SET jev.max_rows_per_statement = 3")
    _out, err = sess.expect_sqlstate(
        "SELECT jev_prob(jev_g, 'the name is Tokyo') FROM jev_g", "54000")
    assert "would send" in err, err
    assert mock_stats()["requests"] == 0
    assert int(sess.scalar("SELECT (jev_stats()->>'errors')::int")) == errors0
    sess.scalar("RESET jev.max_rows_per_statement")
    sess.scalar("SELECT jev_cache_clear()")
    mock("/_mock/reset", {})
    errors0 = int(sess.scalar("SELECT (jev_stats()->>'errors')::int"))
    sess.scalar("SET jev.batch_size = 2")
    sess.scalar("SET jev.max_rows_per_statement = 3")
    _out, err = sess.expect_sqlstate(
        "SELECT jev_prob(jev_g, 'the name is Tokyo g2') FROM jev_g", "54000")
    assert "would send" in err, err
    stats = mock_stats()
    assert stats["requests"] in (0, 1), stats["requests"]
    if stats["requests"] == 1:
        rows = ((stats["bodies"][0].get("json") or {}).get("state") or {}).get("rows") or []
        assert len(rows) == 2, rows
    for b in stats["bodies"]:
        rows = ((b.get("json") or {}).get("state") or {}).get("rows") or []
        assert len(rows) != 3
    assert int(sess.scalar("SELECT (jev_stats()->>'errors')::int")) == errors0
    sess.scalar("RESET jev.batch_size")
    sess.scalar("RESET jev.max_rows_per_statement")
    sess.assert_same_backend()


def test_m1(sess):
    ensure_extension(sess)
    sess.scalar("DROP TABLE IF EXISTS jev_mem")
    sess.scalar("CREATE TABLE jev_mem (id int PRIMARY KEY, payload text)")
    sess.scalar("INSERT INTO jev_mem SELECT g, repeat('x', 4000) FROM generate_series(1, 400) g")
    sess.scalar("SELECT jev_cache_clear()")
    try:
        sess.scalar("SET jev.batch_size = 1")
        sess.scalar("SET jev.max_session_memory_kb = 65536")
        sess.scalar("SELECT jev_prob(t, 'the name is Tokyo') FROM jev_mem t WHERE id = 1")
        s1 = int(sess.scalar("SELECT (jev_stats()->>'session_bytes')::bigint"))
        c1 = int(sess.scalar("SELECT (jev_stats()->>'cached_answers')::int"))
        assert s1 > 0 and c1 >= 1, (s1, c1)
        sess.scalar("SELECT jev_cache_clear()")
        cap_kb = max(1024, int((s1 * 3 + 1023) / 1024))
        sess.scalar("SET jev.max_session_memory_kb = %d" % cap_kb)
        sess.expect_sqlstate("SELECT jev_prob(jev_mem, 'the name is Tokyo') FROM jev_mem", "54000")
        sess.assert_same_backend()
        session_bytes = int(sess.scalar("SELECT (jev_stats()->>'session_bytes')::bigint"))
        in_flight = int(sess.scalar("SELECT (jev_stats()->>'in_flight')::int"))
        cached = int(sess.scalar("SELECT (jev_stats()->>'cached_answers')::int"))
        reserved = int(sess.scalar("SELECT (jev_stats()->>'_reserved')::bigint"))
        buckets = [int(sess.scalar("SELECT (jev_stats()->>'%s')::bigint" % k))
                   for k in ("_b_cache", "_b_job", "_b_fetched", "_b_http", "_b_skipped", "_b_conn")]
        assert in_flight == 0, in_flight
        assert reserved == 0, reserved
        assert session_bytes == sum(buckets), (session_bytes, buckets)
        assert session_bytes <= cap_kb * 1024, (session_bytes, cap_kb)
        assert cached >= 1, cached
        assert session_bytes >= cached * (s1 // max(c1, 1) // 4), (session_bytes, cached, s1, c1)
        sess.scalar("SELECT jev_cache_clear()")
        sess.scalar("SELECT jev_prob(t, 'the name is Tokyo') FROM jev_mem t WHERE id = 1")
        sess.assert_same_backend()
    finally:
        sess.scalar("RESET jev.batch_size")
        sess.scalar("SET jev.max_session_memory_kb = 65536")
        sess.scalar("SELECT jev_cache_clear()")


def test_x1(sess):
    ensure_extension(sess)
    ensure_heap(sess)
    sess.scalar("BEGIN")
    sess.scalar("CREATE TABLE jev_x1 (id int PRIMARY KEY, name text)")
    sess.scalar("INSERT INTO jev_x1 VALUES (1,'Berlin'),(2,'Tokyo')")
    sess.scalar("SAVEPOINT s")
    out = sess.scalar(
        "SELECT string_agg(name || '=' || jev_prob(jev_x1, 'the name is Tokyo')::text, ',' ORDER BY id) FROM jev_x1")
    assert "Berlin=0.1" in out and "Tokyo=0.9" in out, out
    sess.scalar("ROLLBACK TO SAVEPOINT s")
    assert sess.scalar("SELECT (jev_stats()->>'in_flight')::int") == "0"
    out = sess.scalar(
        "SELECT string_agg(name || '=' || jev_prob(jev_x1, 'the name is Tokyo')::text, ',' ORDER BY id) FROM jev_x1")
    assert "Berlin=0.1" in out and "Tokyo=0.9" in out, out
    sess.scalar("COMMIT")
    sess.assert_same_backend()


def test_x_nested(sess):
    ensure_extension(sess)
    ensure_heap(sess)
    sess.scalar("CREATE OR REPLACE FUNCTION jev_nest() RETURNS float8 LANGUAGE sql AS $$ "
                "SELECT jev_prob(t, 'the name is Tokyo') FROM jev_heap t WHERE id = 1 $$")
    sess.scalar("SELECT jev_prob(t, 'the name is Tokyo') FROM jev_heap t WHERE id = 1")
    sess.scalar("SELECT jev_nest()")
    sess.scalar("BEGIN")
    sess.scalar("DECLARE c CURSOR FOR SELECT jev_prob(t, 'the name is Tokyo') FROM jev_heap t")
    sess.scalar("FETCH ALL FROM c")
    sess.scalar("CLOSE c")
    sess.scalar("ROLLBACK")
    assert sess.scalar("SELECT (jev_stats()->>'in_flight')::int") == "0"
    sess.assert_same_backend()


def test_x2(sess):
    ensure_extension(sess)
    hdir = os.environ["JEV_HANDSHAKE_DIR"]
    ready = os.path.join(hdir, "x2-job-ready.json")
    ack = os.path.join(hdir, "x2-handshake.ack")
    http_done = os.path.join(hdir, "x2-http-done.json")
    admit = os.path.join(hdir, "x2-admit-next.ack")
    for p in (ready, ack, http_done, admit):
        if os.path.exists(p):
            os.unlink(p)
    sess.scalar("CREATE TABLE IF NOT EXISTS jev_x2_heap (id int PRIMARY KEY, name text)")
    sess.scalar("DELETE FROM jev_x2_heap")
    sess.scalar("INSERT INTO jev_x2_heap VALUES (1,'Berlin'),(2,'Tokyo')")
    sess.scalar("SELECT jev_cache_clear()")
    sess.scalar("SET jev.batch_size = 1")
    try:
        mock("/_mock/reset", {})
        mock("/_mock/barrier/arm", {"after_requests": 1})
        sess.scalar("SET jev._test_handshake_dir = '%s'" % sql_escape(hdir))
        sess.scalar("SET jev._test_internal_spi_fault = on")
        sess.send("SELECT jev_prob(h, 'the name is Tokyo') FROM jev_x2_heap h")
        for _ in range(200):
            if mock_stats()["requests"] >= 1:
                break
            time.sleep(0.05)
        else:
            raise SystemExit("X2 mock first HTTP timeout")
        for _ in range(200):
            if os.path.isfile(http_done):
                break
            time.sleep(0.05)
        else:
            raise SystemExit("X2 http-done timeout")
        open(admit, "w", encoding="utf-8").write("1\n")
        for _ in range(200):
            if os.path.isfile(ready):
                break
            time.sleep(0.05)
        else:
            raise SystemExit("X2 job-ready timeout")
        info = json.loads(open(ready, encoding="utf-8").read())
        assert str(info["backend_pid"]) == sess.backend_pid, info
        assert int(info["jobs"]) >= 1, info
        open(ack, "w", encoding="utf-8").write("1\n")
        _out, st, err = sess.wait_frame()
        if st == "00000":
            raise SystemExit("X2 unexpected success")
        if st != "XX000" or "jev_test: internal SPI fault" not in err:
            raise SystemExit("X2 expected XX000 jev_test, got %s %s" % (st, err))
        sess.assert_same_backend()
        assert sess.scalar("SELECT (jev_stats()->>'in_flight')::int") == "0"
        mock("/_mock/barrier/release", {})
        if os.path.exists(admit):
            os.unlink(admit)
        sess.scalar("RESET jev._test_internal_spi_fault")
        sess.scalar("RESET jev._test_handshake_dir")
        sess.scalar("SELECT jev_cache_clear()")
        hits = int(sess.scalar("SELECT (jev_stats()->>'cache_hits')::int"))
        qtext = "the name is Tokyo x2-recover %s" % time.time_ns()
        sess.scalar("SELECT jev_prob(h, '%s') FROM jev_x2_heap h WHERE id = 2" % sql_escape(qtext))
        assert int(sess.scalar("SELECT (jev_stats()->>'cache_hits')::int")) == hits
        sess.assert_same_backend()
    finally:
        try:
            mock("/_mock/barrier/release", {})
        except Exception:
            pass
        try:
            sess.scalar("RESET jev._test_internal_spi_fault")
            sess.scalar("RESET jev._test_handshake_dir")
        except Exception:
            pass
        for p in (ready, ack, http_done, admit):
            if os.path.exists(p):
                os.unlink(p)


def test_d7_cancel(sess):
    ensure_extension(sess)
    ensure_heap(sess)
    hdir = os.environ["JEV_HANDSHAKE_DIR"]
    for stage in ("dns", "connect", "response", "retry_sleep"):
        ready, ack = os.path.join(hdir, "cancel-stage-ready.json"), os.path.join(hdir, "cancel-stage.ack")
        for p in (ready, ack):
            if os.path.exists(p):
                os.unlink(p)
        sess.scalar("SELECT jev_cache_clear()")
        sess.scalar("SET jev.keepalive = 0")
        sess.scalar("SET jev._test_handshake_dir = '%s'" % sql_escape(hdir))
        sess.scalar("SET jev._test_cancel_stage = '%s'" % stage)
        qtext = "the name is Tokyo d7 %s %s" % (stage, time.time_ns())
        hits = int(sess.scalar("SELECT (jev_stats()->>'cache_hits')::int"))
        sess.send("SELECT jev_prob(t, '%s') FROM jev_heap t WHERE id=1" % sql_escape(qtext))
        for _ in range(200):
            if os.path.isfile(ready):
                break
            time.sleep(0.05)
        else:
            raise SystemExit("D7 stage-ready timeout: %s" % stage)
        info = json.loads(open(ready, encoding="utf-8").read())
        assert str(info["backend_pid"]) == sess.backend_pid, info
        assert info["stage"] == stage, info
        t0 = time.monotonic()
        q("SELECT pg_cancel_backend(%s)" % sess.backend_pid)
        open(ack, "w", encoding="utf-8").write("1\n")
        _out, st, err = sess.wait_frame(deadline_s=5.0)
        if st != "57014":
            raise SystemExit("D7 expected 57014 got %s %s" % (st, err))
        elapsed = time.monotonic() - t0
        assert elapsed <= 0.25, "D7 cancel too slow: %.3fs stage=%s" % (elapsed, stage)
        last = sess.scalar("SELECT jev_stats()->>'last_cancel_stage'")
        assert last == stage, "last_cancel_stage=%r stage=%s" % (last, stage)
        sess.assert_same_backend()
        sess.scalar("RESET jev._test_cancel_stage")
        sess.scalar("SELECT jev_cache_clear()")
        rec = "the name is Tokyo d7-recover %s %s" % (stage, time.time_ns())
        sess.scalar("SELECT jev_prob(t, '%s') FROM jev_heap t WHERE id=1" % sql_escape(rec))
        hits2 = int(sess.scalar("SELECT (jev_stats()->>'cache_hits')::int"))
        assert hits2 == hits, "D7 recover must cold-miss hits %s -> %s" % (hits, hits2)
        sess.assert_same_backend()
    sess.scalar("RESET jev._test_handshake_dir")
    rc, _o, err = q_err(
        "SET jev.api_url = 'http://jev-cancel.invalid:9/v1/systemone'; "
        "SET jev.api_key = 'test-key'; SET jev.notices = off; "
        "SELECT jev_prob(t, 'x') FROM (SELECT 1 AS id) t;")
    assert rc != 0 and "57014" not in err, err


def test_d7_procdie():
    sess = PersistentPsql()
    try:
        ensure_extension(sess)
        ensure_heap(sess)
        hdir = os.environ["JEV_HANDSHAKE_DIR"]
        ready, ack = os.path.join(hdir, "cancel-stage-ready.json"), os.path.join(hdir, "cancel-stage.ack")
        for p in (ready, ack):
            if os.path.exists(p):
                os.unlink(p)
        sess.scalar("SELECT jev_cache_clear()")
        sess.scalar("SET jev.keepalive = 0")
        sess.scalar("SET jev._test_handshake_dir = '%s'" % sql_escape(hdir))
        sess.scalar("SET jev._test_cancel_stage = 'dns'")
        sess.send("SELECT jev_prob(t, 'the name is Tokyo procdie') FROM jev_heap t WHERE id=1")
        for _ in range(200):
            if os.path.isfile(ready):
                break
            time.sleep(0.05)
        else:
            raise SystemExit("ProcDie stage-ready timeout")
        info = json.loads(open(ready, encoding="utf-8").read())
        assert str(info["backend_pid"]) == sess.backend_pid
        os.kill(int(sess.backend_pid), signal.SIGTERM)
        open(ack, "w", encoding="utf-8").write("1\n")
        try:
            sess.wait_frame(deadline_s=5.0)
            raise SystemExit("ProcDie must not return a normal frame")
        except FrameError as exc:
            assert "57014" not in str(exc), exc
    finally:
        sess.close()
    other = PersistentPsql()
    try:
        assert other.scalar("SELECT 1") == "1"
    finally:
        other.close()


def test_http_reap_and_prefetch(sess):
    ensure_extension(sess)
    ensure_heap(sess)
    sess.scalar("DROP TABLE IF EXISTS jev_cur")
    sess.scalar("CREATE TABLE jev_cur (id int PRIMARY KEY, name text)")
    sess.scalar("INSERT INTO jev_cur SELECT g, 'row' || g FROM generate_series(1, 40) g")
    sess.scalar("SELECT jev_cache_clear()")
    sess.scalar("SET jev.batch_size = 2")
    sess.scalar("SET jev.concurrency = 2")
    try:
        easies = []
        for i in range(12):
            qtext = "reap terminal %s %s" % (i, time.time_ns())
            sess.scalar("SELECT jev_prob(t, '%s') FROM jev_heap t WHERE id = 1" % sql_escape(qtext))
            http = int(sess.scalar("SELECT (jev_stats()->>'_b_http')::bigint"))
            inflight = int(sess.scalar("SELECT (jev_stats()->>'in_flight')::int"))
            easy = int(sess.scalar("SELECT (jev_stats()->>'_http_easies')::int"))
            assert inflight == 0, inflight
            assert http == 0, http
            assert easy <= 64, easy
            easies.append(easy)
        assert easies[-1] <= easies[0] + 2, easies
        sess.scalar("SELECT jev_cache_clear()")
        assert int(sess.scalar("SELECT (jev_stats()->>'_b_http')::bigint")) == 0
        assert int(sess.scalar("SELECT (jev_stats()->>'_http_easies')::int")) <= max(easies[0], 4) + 2

        qtext = "prefetch eog %s" % time.time_ns()
        sess.scalar("SELECT jev_prob(t, '%s') FROM jev_cur t LIMIT 1" % sql_escape(qtext))
        recovered = sess.scalar(
            "SELECT count(*) FROM (SELECT jev_prob(t, '%s') FROM jev_cur t LIMIT 10) s" % sql_escape(qtext))
        assert recovered == "10", recovered
        assert sess.scalar("SELECT (jev_stats()->>'in_flight')::int") == "0"
        assert sess.scalar("SELECT (jev_stats()->>'_b_http')::int") == "0"

        qtext = "cursor keepalive %s" % time.time_ns()
        sess.scalar("SET jev.keepalive = 600")
        sess.scalar("BEGIN")
        sess.scalar(
            "DECLARE c NO SCROLL CURSOR FOR SELECT jev_prob(t, '%s') FROM jev_cur t" % sql_escape(qtext))
        sess.scalar("FETCH 1 FROM c")
        sess.scalar("SET jev.keepalive = 0")
        sess.send("FETCH 5 FROM c")
        _out, st, err = sess.wait_frame(deadline_s=15.0)
        if st != "00000":
            raise FrameError("cursor keepalive FETCH hung or failed %s %s" % (st, err))
        sess.scalar("CLOSE c")
        sess.scalar("COMMIT")
        assert sess.scalar("SELECT (jev_stats()->>'in_flight')::int") == "0"
        assert sess.scalar("SELECT (jev_stats()->>'_b_http')::int") == "0"
        sess.assert_same_backend()
    finally:
        sess.scalar("RESET jev.batch_size")
        sess.scalar("RESET jev.concurrency")
        sess.scalar("RESET jev.keepalive")
        sess.scalar("SELECT jev_cache_clear()")


def test_memory_accounting_stable(sess):
    ensure_extension(sess)
    ensure_heap(sess)
    sess.scalar("SELECT jev_cache_clear()")
    try:
        s0 = jev_debug_stats(sess)
        assert s0["_b_cache"] == 0 and s0["_b_job"] == 0, s0
        assert s0["_b_fetched"] == 0 and s0["_b_http"] == 0 and s0["_b_skipped"] == 0, s0
        assert s0["cached"] == 0, s0
        assert s0["session_bytes"] == s0["_b_conn"], s0
        assert s0["_http_easies"] <= 64, s0

        easies = []
        caches = []
        for i in range(8):
            qtext = "acct unique %s %s" % (i, time.time_ns())
            sess.scalar("SELECT jev_prob(s, '%s') FROM (SELECT 1 AS id, 'Berlin' AS name) s" % sql_escape(qtext))
            s = jev_debug_stats(sess)
            assert s["_b_fetched"] == 0 and s["_b_http"] == 0, s
            assert s["_http_easies"] <= 64, s
            assert s["cached"] == i + 1, s
            easies.append(s["_http_easies"])
            caches.append(s["_b_cache"])
        assert easies[-1] <= easies[0] + 2, easies
        if len(caches) >= 3:
            step0 = caches[1] - caches[0]
            steplast = caches[-1] - caches[-2]
            assert steplast <= step0 * 3 + 64, (caches, step0, steplast)

        qhit = "acct hit %s" % time.time_ns()
        sess.scalar("SELECT jev_prob(s, '%s') FROM (SELECT 1 AS id, 'Berlin' AS name) s" % sql_escape(qhit))
        s_hit1 = jev_debug_stats(sess)
        sess.scalar("SELECT jev_prob(s, '%s') FROM (SELECT 1 AS id, 'Berlin' AS name) s" % sql_escape(qhit))
        s_hit2 = jev_debug_stats(sess)
        assert s_hit2["_b_cache"] == s_hit1["_b_cache"], (s_hit1, s_hit2)
        assert s_hit2["session_bytes"] == s_hit1["session_bytes"], (s_hit1, s_hit2)
        assert s_hit2["_b_fetched"] == 0 and s_hit2["_b_http"] == 0, s_hit2

        sess.scalar("SELECT jev_cache_clear()")
        s1 = jev_debug_stats(sess)
        assert s1["_b_cache"] == 0 and s1["_b_job"] == 0, s1
        assert s1["_b_fetched"] == 0 and s1["_b_http"] == 0 and s1["_b_skipped"] == 0, s1
        assert s1["session_bytes"] == s1["_b_conn"], s1

        for i in range(8):
            qtext = "acct unique2 %s %s" % (i, time.time_ns())
            sess.scalar("SELECT jev_prob(s, '%s') FROM (SELECT 1 AS id, 'Berlin' AS name) s" % sql_escape(qtext))
        sess.scalar("SELECT jev_cache_clear()")
        s2 = jev_debug_stats(sess)
        assert s2["_b_cache"] == 0 and s2["_b_job"] == 0, s2
        assert s2["_b_fetched"] == 0 and s2["_b_http"] == 0, s2
        assert s2["session_bytes"] == s1["session_bytes"], (s1, s2)
        assert s2["_http_easies"] <= max(s1["_http_easies"], 4) + 2, (s1, s2)
    finally:
        sess.scalar("SELECT jev_cache_clear()")


def test_l1(sess):
    ensure_extension(sess)
    ensure_heap(sess)
    url = os.environ["JEV_API_URL"]
    sess.scalar("SET typesafe.api_key = 'test-key'")
    sess.scalar("SET typesafe.mock_response = ''")
    sess.scalar("SET typesafe.endpoint = '%s'" % sql_escape(url))
    sess.scalar("SELECT typesafe_noul('x', 'urgent?')")
    before = sess.scalar("SELECT typesafe_last_request()")
    assert before
    sess.scalar("SELECT jev_prob(t, 'the name is Tokyo l1') FROM jev_heap t WHERE id=1")
    after = sess.scalar("SELECT typesafe_last_request()")
    assert after == before
    sess.assert_same_backend()


def write_report():
    logdir = os.environ.get("LOGDIR") or os.environ.get("JEV_HANDSHAKE_DIR") or "."
    path = os.path.join(logdir, "python.json")
    payload = {
        "results": RESULTS,
        "passed": sum(1 for r in RESULTS if r["ok"]),
        "failed": sum(1 for r in RESULTS if not r["ok"]),
        "tap_tests": "not run: bundled PG18 configure has no --enable-tap-tests",
    }
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2)
        fh.write("\n")
    return path, payload


def main():
    required = ["PGHOST", "PGPORT", "PGUSER", "PGROOT", "PGDATA", "JEV_API_URL",
                "MOCK_STATS_URL", "MOCK_HOST", "MOCK_PORT", "FDW_PORT",
                "JEV_HANDSHAKE_DIR"]
    missing = [k for k in required if not os.environ.get(k)]
    if missing:
        raise SystemExit("missing env: %s" % ", ".join(missing))
    sess = PersistentPsql()
    try:
        ensure_extension(sess)
        run_case("test_persistent_psql_sqlstate_token", lambda: test_env_and_token(sess))
        run_case("test_d8b_mock_bodies", test_d8b_mock_bodies)
        run_case("test_legacy_http_001_002", test_legacy_http)
        run_case("test_http_paths_001_002", test_http_paths)
        run_case("test_004_guc_placeholder", test_004_guc_placeholder)
        run_case("test_g1_g2", lambda: test_g1_g2(sess))
        run_case("test_m1_disjoint_buckets", lambda: test_m1(sess))
        run_case("test_x1_savepoint", lambda: test_x1(sess))
        run_case("test_x_nested_portal_abort", lambda: test_x_nested(sess))
        run_case("test_http_reap_prefetch_keepalive", lambda: test_http_reap_and_prefetch(sess))
        run_case("test_memory_accounting_stable", lambda: test_memory_accounting_stable(sess))
        run_case("test_x2_release_before_cold_miss", lambda: test_x2(sess))
        run_case("test_d7_cancel_stage_handshake", lambda: test_d7_cancel(sess))
        run_case("test_l1_last_request_isolated", lambda: test_l1(sess))
    finally:
        sess.close()
    run_case("test_d7_procdie_not_57014", test_d7_procdie)
    run_case("test_005_upgrade", test_005_upgrade)
    path, payload = write_report()
    print("wrote %s (%d passed, %d failed)" % (path, payload["passed"], payload["failed"]))
    if payload["failed"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
