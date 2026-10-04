/* pg_typesafe — upgrade 0.0.1 -> 0.1.0: add native Jev API */

\echo Use "ALTER EXTENSION typesafe UPDATE" to load this file. \quit

-- Native Jev API (compatibility version 0.2.0). Legacy typesafe_* signatures
-- are unchanged. Judgment helpers are STABLE PARALLEL UNSAFE and not STRICT.
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
DO $$
DECLARE
	r record;
BEGIN
	FOR r IN
		SELECT p.oid::regprocedure AS sig
		FROM pg_proc p
		JOIN pg_extension e ON e.extname = 'typesafe'
		WHERE p.pronamespace = e.extnamespace
		  AND (p.proname = 'jev' OR p.proname LIKE 'jev\_%' ESCAPE '\')
	LOOP
		EXECUTE format('REVOKE ALL ON FUNCTION %s FROM PUBLIC', r.sig);
	END LOOP;
END
$$;
