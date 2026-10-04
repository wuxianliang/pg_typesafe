#!/usr/bin/env python3
"""Assert stannum/pg_textsearch dict blocks are unchanged. argv: generate.pre.py generate.post.py"""
import re, sys
from pathlib import Path

def block(src: str, name: str) -> str:
    m = re.search(r'"%s": dict\((.*?)\n    \),' % name, src, re.S)
    if not m:
        raise SystemExit("missing EXTENSIONS %s block" % name)
    return m.group(0)

pre, post = Path(sys.argv[1]).read_text(), Path(sys.argv[2]).read_text()
assert block(pre, "stannum") == block(post, "stannum")
assert block(pre, "pg_textsearch") == block(post, "pg_textsearch")
