CREATE EXTENSION IF NOT EXISTS typesafe;
SELECT current_setting('server_version') AS server_version;
\set ECHO none
SET jev.api_url = :'jev_api_url';
SET jev.api_key = 'test-key';
SET jev.notices = off;
\set ECHO all
SELECT jev_version();
\set VERBOSITY terse

CREATE TABLE things (id int, v text);
INSERT INTO things VALUES (1, 'a');

-- No key configured anywhere
SET jev.api_key = '';
SELECT jev(things, 'anything') FROM things;

-- Non-retryable HTTP errors surface as SQL errors
SET jev.api_key = 'wrong-key';
SELECT jev(things, 'anything') FROM things;
SET jev.api_key = 'test-key';
SELECT jev(things, 'please trigger422') FROM things;

-- Unknown question kind
SELECT jev_eval(things, 'anything', 'bogus', NULL) FROM things;

-- Errors are counted, and the session keeps working afterwards
SELECT (jev_stats()->>'errors')::int AS errors;
SELECT jev(things, 'the value is a') FROM things;
