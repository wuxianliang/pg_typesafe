/* pg_typesafe — typesafe--0.1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION typesafe" to load this file. \quit

CREATE TYPE typesafe_choice AS (
	choice text,
	confidence double precision,
	probabilities jsonb,
	model text,
	input_tokens integer,
	output_tokens integer
);

CREATE TYPE typesafe_noul AS (
	noul double precision,
	model text,
	input_tokens integer,
	output_tokens integer
);

CREATE TYPE typesafe_score AS (
	score double precision,
	confidence double precision,
	legend jsonb,
	probabilities jsonb,
	model text,
	input_tokens integer,
	output_tokens integer
);

CREATE FUNCTION typesafe_classify(
	state text,
	instructions text,
	options jsonb,
	model text DEFAULT NULL
)
RETURNS typesafe_choice
AS 'MODULE_PATHNAME', 'typesafe_classify'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_detect(
	state text,
	instructions text,
	true_meaning text DEFAULT NULL,
	false_meaning text DEFAULT NULL,
	model text DEFAULT NULL
)
RETURNS typesafe_noul
AS 'MODULE_PATHNAME', 'typesafe_detect'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_score(
	state text,
	instructions text,
	levels text[],
	model text DEFAULT NULL
)
RETURNS typesafe_score
AS 'MODULE_PATHNAME', 'typesafe_score'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_ask(
	state jsonb,
	questions jsonb,
	model text DEFAULT NULL
)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'typesafe_ask'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_ask(
	state text,
	questions jsonb,
	model text DEFAULT NULL
)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'typesafe_ask_text'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_label(
	state text,
	instructions text,
	options jsonb,
	model text DEFAULT NULL
)
RETURNS text
AS 'MODULE_PATHNAME', 'typesafe_label'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_last_request()
RETURNS text
AS 'MODULE_PATHNAME', 'typesafe_last_request'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_detect_many(
	states text[],
	instructions text,
	true_meaning text DEFAULT NULL,
	false_meaning text DEFAULT NULL,
	model text DEFAULT NULL
)
RETURNS TABLE(
	ordinality integer,
	state text,
	noul double precision,
	model text,
	input_tokens integer,
	output_tokens integer
)
AS 'MODULE_PATHNAME', 'typesafe_detect_many'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_classify_many(
	states text[],
	instructions text,
	options jsonb,
	model text DEFAULT NULL
)
RETURNS TABLE(
	ordinality integer,
	state text,
	choice text,
	confidence double precision,
	probabilities jsonb,
	model text,
	input_tokens integer,
	output_tokens integer
)
AS 'MODULE_PATHNAME', 'typesafe_classify_many'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_label_many(
	states text[],
	instructions text,
	options jsonb,
	model text DEFAULT NULL
)
RETURNS TABLE(
	ordinality integer,
	state text,
	choice text
)
AS $$
	SELECT ordinality, state, choice
	FROM typesafe_classify_many(states, instructions, options, model);
$$ LANGUAGE SQL VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_noul(
	state text,
	instructions text,
	true_meaning text DEFAULT NULL,
	false_meaning text DEFAULT NULL,
	model text DEFAULT NULL
)
RETURNS double precision
AS $$
	SELECT CASE
		WHEN state IS NULL OR instructions IS NULL THEN NULL
		ELSE (SELECT noul
			  FROM typesafe_detect(state, instructions, true_meaning,
								   false_meaning, model))
	END;
$$ LANGUAGE SQL VOLATILE PARALLEL UNSAFE;

-- Native Jev API (compatibility version 0.2.0). Judgment helpers are STABLE
-- PARALLEL UNSAFE: they may touch backend-local cache/HTTP but not DB state.
-- Optional arguments may be NULL, so these are not STRICT.
CREATE FUNCTION jev(rec anyelement, query text, threshold double precision DEFAULT NULL)
RETURNS boolean
AS 'MODULE_PATHNAME', 'jev'
LANGUAGE C STABLE PARALLEL UNSAFE COST 100;

CREATE FUNCTION jev_prob(rec anyelement, query text)
RETURNS double precision
AS 'MODULE_PATHNAME', 'jev_prob'
LANGUAGE C STABLE PARALLEL UNSAFE COST 100;

CREATE FUNCTION jev_choice(rec anyelement, query text, options text[])
RETURNS text
AS 'MODULE_PATHNAME', 'jev_choice'
LANGUAGE C STABLE PARALLEL UNSAFE COST 100;

CREATE FUNCTION jev_score(rec anyelement, query text, levels text[])
RETURNS double precision
AS 'MODULE_PATHNAME', 'jev_score'
LANGUAGE C STABLE PARALLEL UNSAFE COST 100;

CREATE FUNCTION jev_score_norm(rec anyelement, query text, levels text[])
RETURNS double precision
AS 'MODULE_PATHNAME', 'jev_score_norm'
LANGUAGE C STABLE PARALLEL UNSAFE COST 100;

CREATE FUNCTION jev_confidence(rec anyelement, query text, kind text, options text[])
RETURNS double precision
AS 'MODULE_PATHNAME', 'jev_confidence'
LANGUAGE C STABLE PARALLEL UNSAFE COST 100;

CREATE FUNCTION jev_eval(rec anyelement, query text, kind text DEFAULT 'noul', options text[] DEFAULT NULL)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'jev_eval'
LANGUAGE C STABLE PARALLEL UNSAFE COST 100;

CREATE FUNCTION jev_stats()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'jev_stats'
LANGUAGE C STABLE PARALLEL UNSAFE COST 100;

CREATE FUNCTION jev_cache_clear()
RETURNS void
AS 'MODULE_PATHNAME', 'jev_cache_clear'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION jev_version()
RETURNS text
AS 'MODULE_PATHNAME', 'jev_version'
LANGUAGE C IMMUTABLE PARALLEL UNSAFE;

COMMENT ON FUNCTION jev(anyelement, text, double precision) IS
  'True when the row satisfies the natural-language condition (TypeSafe Jev). Usage: WHERE jev(tbl, ''condition'')';
COMMENT ON FUNCTION jev_prob(anyelement, text) IS
  'Probability that the row satisfies the natural-language condition.';
COMMENT ON FUNCTION jev_score(anyelement, text, text[]) IS
  'Probability-weighted rating of the row along ordered levels.';
COMMENT ON FUNCTION jev_choice(anyelement, text, text[]) IS
  'Classifies the row into one of the given options.';
COMMENT ON FUNCTION jev_score_norm(anyelement, text, text[]) IS
  'jev_score normalised to 0..1.';
COMMENT ON FUNCTION jev_confidence(anyelement, text, text, text[]) IS
  'Confidence (0..1) of a choice or score answer.';
COMMENT ON FUNCTION jev_eval(anyelement, text, text, text[]) IS
  'Raw TypeSafe Jev answer JSON for one row.';
COMMENT ON FUNCTION jev_stats() IS
  'Session statistics for native Jev.';
COMMENT ON FUNCTION jev_cache_clear() IS
  'Forget cached Jev judgments for this session.';
COMMENT ON FUNCTION jev_version() IS
  'Jev API compatibility version (0.2.0). Extension catalog version is 0.1.0.';

-- Outbound HTTP spends the server's TypeSafe quota. Owner only by default.
-- GRANT EXECUTE ON FUNCTION typesafe_noul(text, text, text, text, text) TO app;
-- GRANT EXECUTE ON FUNCTION jev_prob(anyelement, text) TO app;
DO $$
DECLARE
	r record;
BEGIN
	FOR r IN
		SELECT p.oid::regprocedure AS sig
		FROM pg_proc p
		JOIN pg_extension e ON e.extname = 'typesafe'
		WHERE p.pronamespace = e.extnamespace
		  AND (p.proname LIKE 'typesafe\_%' ESCAPE '\'
		       OR p.proname = 'jev'
		       OR p.proname LIKE 'jev\_%' ESCAPE '\')
	LOOP
		EXECUTE format('REVOKE ALL ON FUNCTION %s FROM PUBLIC', r.sig);
	END LOOP;
END
$$;
