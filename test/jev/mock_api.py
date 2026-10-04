#!/usr/bin/env python3
"""Deterministic loopback stand-in for TypeSafe /v1/systemone (WI-2 mock protocol)."""

from __future__ import annotations

import argparse
import json
import os
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LABEL_PROBS = {"billing": 0.1, "technical": 0.8, "sales": 0.1}
RATING_LEGEND = {"0": "Calm", "1": "Frustrated", "2": "Very angry"}
RATING_PROBS = {"0": 0.05, "1": 0.3, "2": 0.65}


class MockState:
    def __init__(self):
        self.lock = threading.Lock()
        self.requests = 0
        self.accepted_connections = 0
        self.bodies = []
        self.flaky = 0
        self.barrier_after = None
        self.barrier_event = threading.Event()
        self.barrier_event.set()

    def reset(self):
        with self.lock:
            self.requests = 0
            self.bodies = []
            self.flaky = 0
            self.barrier_after = None
            self.barrier_event.set()


STATE = MockState()


def atomic_write(path, text):
    directory = os.path.dirname(os.path.abspath(path)) or "."
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".mock-ready.")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(text)
        os.replace(tmp, path)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def jev_answers(req):
    state = req.get("state") if isinstance(req.get("state"), dict) else {}
    questions = req.get("questions") if isinstance(req.get("questions"), dict) else {}
    rows = state.get("rows") or []
    cond = state.get("condition") or ""
    needle = cond.split()[-1].lower() if cond.split() else ""
    answers = {}
    for qid, q in questions.items():
        i = int(qid[1:])
        row = rows[i] if i < len(rows) else {}
        row_json = json.dumps(row, sort_keys=True)
        qtype = q.get("type")
        if qtype == "noul":
            answers[qid] = {
                "type": "noul",
                "noul": 0.9 if needle and needle in row_json.lower() else 0.1,
            }
        elif qtype == "score":
            levels = q.get("criteria") or []
            k = len(row_json) % len(levels)
            answers[qid] = {
                "type": "score",
                "score": float(k),
                "legend": {str(j): l for j, l in enumerate(levels)},
                "probabilities": {str(j): (1.0 if j == k else 0.0) for j in range(len(levels))},
                "confidence": 1.0,
            }
        elif qtype == "choice":
            opts = list((q.get("criteria") or {}).keys())
            k = len(row_json) % len(opts)
            answers[qid] = {
                "type": "choice",
                "choice": opts[k],
                "probabilities": {o: (1.0 if j == k else 0.0) for j, o in enumerate(opts)},
                "confidence": 1.0,
            }
    return answers


def legacy_answers(req):
    questions = req.get("questions") or {}
    answers = {}
    if "label" in questions:
        answers["label"] = {
            "type": "choice",
            "choice": "technical",
            "probabilities": dict(LABEL_PROBS),
            "confidence": 0.8,
        }
    if "flag" in questions:
        answers["flag"] = {"type": "noul", "noul": 0.42}
    if "rating" in questions:
        answers["rating"] = {
            "type": "score",
            "score": 1.6,
            "confidence": 0.78,
            "legend": dict(RATING_LEGEND),
            "probabilities": dict(RATING_PROBS),
        }
    for qid, q in questions.items():
        if qid in answers:
            continue
        if isinstance(qid, str) and len(qid) > 1 and qid[0] == "s" and qid[1:].isdigit():
            answers[qid] = {"type": "noul", "noul": 0.42}
    return answers, 7


def dispatch_systemone(req):
    questions = req.get("questions")
    if not isinstance(questions, dict):
        return 422, {"error": "invalid request"}
    state = req.get("state")
    if not isinstance(state, dict):
        state = {}
    if "trigger422" in (state.get("condition") or ""):
        return 422, {"error": "mock validation failure"}
    if isinstance(state.get("rows"), list):
        answers = jev_answers(req)
        body = json.dumps(req).encode()
        return 200, {
            "model": "jev-mock",
            "answers": answers,
            "usage": {"input_tokens": len(body) // 4, "output_tokens": len(answers)},
        }
    if any(k in questions for k in ("label", "flag", "rating")):
        answers, tokens = legacy_answers(req)
        return 200, {
            "model": "jev-latest",
            "answers": answers,
            "usage": {"input_tokens": tokens, "output_tokens": 2},
        }
    if any(isinstance(k, str) and k.startswith("s") for k in questions):
        answers, tokens = legacy_answers(req)
        return 200, {
            "model": "jev-mock",
            "answers": answers,
            "usage": {"input_tokens": tokens, "output_tokens": 3},
        }
    return 422, {"error": "invalid request"}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    ready = False
    log_fh = None

    def handle(self):
        STATE.accepted_connections += 1
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def log_message(self, fmt, *args):
        if self.log_fh is not None:
            self.log_fh.write("%s - %s\n" % (self.address_string(), fmt % args))
            self.log_fh.flush()

    def _read_body(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        return self.rfile.read(n) if n else b""

    def _send(self, code, obj=None, extra=None, raw=None):
        if raw is None:
            data = json.dumps(obj).encode() if obj is not None else b""
        else:
            data = raw if isinstance(raw, bytes) else raw.encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        if extra:
            for k, v in extra:
                self.send_header(k, v)
        self.end_headers()
        if data:
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

    def _record(self, path, body):
        auth = self.headers.get("Authorization", "")
        parsed = None
        if body:
            try:
                parsed = json.loads(body.decode("utf-8"))
            except ValueError:
                parsed = None
        with STATE.lock:
            STATE.requests += 1
            STATE.bodies.append({"path": path, "authorization": auth, "json": parsed})
            after = STATE.barrier_after
            n = STATE.requests
        # after N completed business POSTs, block subsequent ones
        if after is not None and n > after:
            STATE.barrier_event.wait()

    def do_GET(self):
        if self.path.split("?", 1)[0] == "/_mock/stats":
            with STATE.lock:
                payload = {
                    "requests": STATE.requests,
                    "accepted_connections": STATE.accepted_connections,
                    "bodies": list(STATE.bodies),
                }
            return self._send(200, payload)
        return self._send(404, {"error": "not found"})

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        body = self._read_body()
        if path == "/_mock/reset":
            STATE.reset()
            return self._send(200, {"ok": True})
        if path == "/_mock/barrier/arm":
            spec = {}
            if body:
                try:
                    spec = json.loads(body.decode("utf-8"))
                except ValueError:
                    spec = {}
            with STATE.lock:
                STATE.barrier_after = int(spec.get("after_requests", 1))
                STATE.barrier_event.clear()
            return self._send(200, {"ok": True})
        if path == "/_mock/barrier/release":
            STATE.barrier_event.set()
            return self._send(200, {"ok": True})

        if not self.ready:
            return self._send(503, {"error": "not ready"})

        if self.headers.get("Authorization", "") != "Bearer test-key":
            self._record(path, body)
            return self._send(401, {"error": "invalid api key"})

        if path == "/stall":
            self._record(path, body)
            time.sleep(30)
            return
        if path == "/429":
            self._record(path, body)
            return self._send(429, {"error": "rate limited"}, extra=[("Retry-After", "30")])
        if path == "/flaky":
            self._record(path, body)
            with STATE.lock:
                STATE.flaky += 1
                n = STATE.flaky
            if n <= 2:
                return self._send(429, {"error": "rate limited"}, extra=[("Retry-After", "1")])
            return self._ok_legacy(body)
        if path == "/always429":
            self._record(path, body)
            return self._send(429, {"error": "rate limited"})
        if path == "/huge":
            self._record(path, body)
            raw = b'{"model":"m","pad":"' + (b"x" * (9 * 1024 * 1024)) + b'"}'
            return self._send(200, raw=raw)
        if path == "/garbage":
            self._record(path, body)
            return self._send(200, raw=b"this is not json {{{")
        if path == "/wrongshape":
            self._record(path, body)
            return self._send(200, {"model": "jev-latest"})
        if path in ("/v1/systemone", "/ok"):
            self._record(path, body)
            try:
                req = json.loads(body.decode("utf-8")) if body else {}
            except ValueError:
                return self._send(422, {"error": "invalid request"})
            try:
                code, obj = dispatch_systemone(req)
            except Exception:
                return self._send(422, {"error": "invalid request"})
            return self._send(code, obj)
        self._record(path, body)
        return self._send(404, {"error": "not found"})

    def _ok_legacy(self, body):
        try:
            req = json.loads(body.decode("utf-8")) if body else {}
        except ValueError:
            req = {}
        code, obj = dispatch_systemone(req)
        return self._send(code, obj)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bind", default="127.0.0.1")
    p.add_argument("--port", type=int, default=0)
    p.add_argument("--ready-file", required=True)
    p.add_argument("--log-file", required=True)
    args = p.parse_args()

    log_fh = open(args.log_file, "w", encoding="utf-8")
    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    host, port = server.server_address
    Handler.log_fh = log_fh
    url = "http://%s:%s/v1/systemone" % (host, port)
    ready = {"host": host, "port": int(port), "url": url}
    atomic_write(args.ready_file, json.dumps(ready))
    Handler.ready = True
    try:
        server.serve_forever()
    finally:
        log_fh.close()


if __name__ == "__main__":
    main()
