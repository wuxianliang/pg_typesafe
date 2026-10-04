# Native Jev tests

Loopback-only. Do not call the live TypeSafe API.

```bash
bash test/jev/run_standalone.sh
```

This installs into the bundled PG 18.4 prefix, runs legacy `typesafe`
`pg_regress`, then `test/jev/sql` against `test/jev/mock_api.py`, then
`run_python_suite.py`. `REGRESS` in the Makefile remains `typesafe` only.
Bundled PG18 TAP is not enabled; `t/*.pl` is not a green signal there.

Allowed golden-output differences versus pg-jev:

1. `CREATE EXTENSION typesafe` replaces `CREATE EXTENSION jev`.
2. Endpoint configuration uses the psql variable `:jev_api_url`.
3. Native errors omit `plpy.Error:`.
4. HTTP diagnostics omit provider response bodies.
5. Comments may mention bounded resources, cancellation, and `jev()` returning boolean.
