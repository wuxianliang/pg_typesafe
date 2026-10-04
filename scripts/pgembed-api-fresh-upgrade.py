#!/usr/bin/env python3
"""Isolated pgembed API fresh CREATE and 0.0.1->0.1.0 upgrade. Mock only. No user PGDATA."""
import json
import os
import sys
import tempfile
from pathlib import Path

os.environ.pop("TYPESAFE_API_KEY", None)
os.environ["TYPESAFE_API_KEY"] = ""

import pgembed  # noqa: E402


MOCK_CLASSIFY = """
SET typesafe.mock_response = $${
  "model": "jev-latest",
  "answers": {
    "label": {"type": "choice", "choice": "technical", "confidence": 0.82,
              "probabilities": {"billing": 0.08, "technical": 0.85, "sales": 0.07}}
  },
  "usage": {"input_tokens": 312, "output_tokens": 48}
}$$;
SELECT choice FROM typesafe_classify(
    'Help! My payouts have been failing for 3 days.',
    'Which team should handle this?',
    '{"billing": "Payments, invoicing, refunds",
      "technical": "Bugs, outages, integrations",
      "sales": "Pricing, upgrades, new accounts"}'::jsonb);
"""


def _scalar(pg, sql: str) -> str:
    raw = pg.psql(sql)
    lines = [ln.rstrip() for ln in raw.splitlines()]
    for i, ln in enumerate(lines):
        stripped = ln.strip()
        if stripped and set(stripped.replace("+", "")) <= {"-"} and "-" in stripped:
            if i + 1 < len(lines):
                return lines[i + 1].strip()
    text = raw.strip()
    if text:
        return text.splitlines()[0]
    raise AssertionError("empty psql output for %r" % sql)


def fresh(pg) -> dict:
    out = pg.create_extension("pg_typesafe").strip()
    assert out == "CREATE EXTENSION", out
    ext = _scalar(pg, "SELECT extversion FROM pg_extension WHERE extname = 'typesafe';")
    ver = _scalar(pg, "SELECT jev_version();")
    choice = pg.psql(MOCK_CLASSIFY)
    assert ext == "0.1.0", ext
    assert ver == "0.2.0", ver
    assert "technical" in choice, choice
    return {"create": out, "extversion": ext, "jev_version": ver, "classify": "technical"}


def upgrade(pg) -> dict:
    created = _scalar(pg, "CREATE EXTENSION typesafe VERSION '0.0.1';")
    assert created == "CREATE EXTENSION", created
    before = _scalar(pg, "SELECT extversion FROM pg_extension WHERE extname = 'typesafe';")
    assert before == "0.0.1", before
    updated = _scalar(pg, "ALTER EXTENSION typesafe UPDATE;")
    assert updated == "ALTER EXTENSION", updated
    after = _scalar(pg, "SELECT extversion FROM pg_extension WHERE extname = 'typesafe';")
    ver = _scalar(pg, "SELECT jev_version();")
    assert after == "0.1.0", after
    assert ver == "0.2.0", ver
    choice = pg.psql(MOCK_CLASSIFY)
    assert "technical" in choice, choice
    return {
        "before": before,
        "after": after,
        "jev_version": ver,
        "classify": "technical",
    }


def main() -> None:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("pgembed-api.json")
    assert pgembed.has_extension("pg_typesafe") is True
    assert pgembed.has_extension("pg_textsearch") is False
    payload = {
        "has_extension_pg_typesafe": True,
        "has_extension_pg_textsearch": False,
        "fresh": None,
        "upgrade": None,
    }
    fresh_dir = tempfile.mkdtemp(prefix="jev-pgembed-api-fresh-")
    upgrade_dir = tempfile.mkdtemp(prefix="jev-pgembed-api-upgrade-")
    with pgembed.get_server(fresh_dir, cleanup_mode="delete") as pg:
        payload["fresh"] = fresh(pg)
    with pgembed.get_server(upgrade_dir, cleanup_mode="delete") as pg:
        payload["upgrade"] = upgrade(pg)
    out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(payload, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
