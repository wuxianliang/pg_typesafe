CREATE EXTENSION IF NOT EXISTS typesafe;
SELECT current_setting('server_version') AS server_version;
\set ECHO none
SET jev.api_url = :'jev_api_url';
SET jev.api_key = 'test-key';
SET jev.notices = off;
\set ECHO all
SELECT jev_version();
SELECT extversion FROM pg_extension WHERE extname = 'typesafe';
SELECT p.proname,
       pg_get_function_result(p.oid) AS result,
       l.lanname,
       p.provolatile,
       p.proparallel,
       p.proisstrict,
       p.prosecdef
FROM pg_proc p
JOIN pg_language l ON l.oid = p.prolang
JOIN pg_extension e ON p.pronamespace = e.extnamespace AND e.extname = 'typesafe'
WHERE p.proname = 'jev' OR p.proname LIKE 'jev\_%'
ORDER BY 1, pg_get_function_identity_arguments(p.oid);
