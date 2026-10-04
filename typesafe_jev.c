/*-------------------------------------------------------------------------
 *
 * typesafe_jev.c
 *		Native Jev engine: GUCs, cache, lifecycle, scheduler, SQL entry.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "common/cryptohash.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/parse_func.h"
#include "port.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "tcop/dest.h"
#include "tcop/pquery.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/wait_event.h"

#include "typesafe_internal.h"
#include "typesafe_http.h"
#include "typesafe_jev.h"

PG_FUNCTION_INFO_V1(jev);
PG_FUNCTION_INFO_V1(jev_prob);
PG_FUNCTION_INFO_V1(jev_choice);
PG_FUNCTION_INFO_V1(jev_score);
PG_FUNCTION_INFO_V1(jev_score_norm);
PG_FUNCTION_INFO_V1(jev_confidence);
PG_FUNCTION_INFO_V1(jev_eval);
PG_FUNCTION_INFO_V1(jev_stats);
PG_FUNCTION_INFO_V1(jev_cache_clear);
PG_FUNCTION_INFO_V1(jev_version);

int			jev_spi_depth = 0;
uint64		jev_call_generation = 0;
uint64		jev_curl_generation = 1;
JevCallContext *jev_call_context = NULL;

Size			jev_bucket[JEV_NBUCKETS];
Size			jev_reserved_bytes = 0;

MemoryContext JevSessionContext = NULL;
MemoryContext JevCacheContext = NULL;
MemoryContext JevCurlContext = NULL;

static char *jev_guc_api_key = NULL;
static char *jev_guc_api_url = NULL;
static char *jev_guc_model = NULL;
static int	jev_guc_batch_size = 20;
static int	jev_guc_concurrency = 16;
static int	jev_guc_max_prefetch = 5000;
static bool jev_guc_notices = true;
static double jev_guc_timeout = 30.0;
static int	jev_guc_keepalive = 600;
static int	jev_guc_max_rows = 0;
static int	jev_guc_max_chars = 0;
static int	jev_guc_max_session_memory_kb = 65536;
static double jev_guc_threshold = 0.5;
static bool jev_guc_test_spi_fault = false;
static char *jev_guc_handshake_dir = NULL;
static char *jev_guc_cancel_stage = NULL;

static uint8 jev_backend_nonce[16];
static bool jev_nonce_ready = false;
static Oid	jev_userid = InvalidOid;
static Oid	jev_sessid = InvalidOid;
static HTAB    *jev_cache_hash = NULL;
static HTAB    *jev_job_hash = NULL;
TsHttpSession *jev_http = NULL;
static List    *jev_scope_stack = NIL;

static int64 jev_stat_requests = 0;
static int64 jev_stat_input_tokens = 0;
static int64 jev_stat_output_tokens = 0;
static int64 jev_stat_rows_evaluated = 0;
static int64 jev_stat_cache_hits = 0;
static double jev_stat_api_ms = 0;
static int64 jev_stat_errors = 0;
static int64 jev_stat_retries = 0;
static int64 jev_stat_cancelled = 0;
static char jev_last_cancel_stage[32] = "";

static TimestampTz jev_spend_ts = 0;
static int64 jev_spend_rows = 0;
static int64 jev_spend_chars = 0;

static char *jev_last_url = NULL;
static uint8 jev_last_cred[JEV_SHA256_LEN];
static bool jev_have_last_cred = false;
static int	jev_last_keepalive = -1;

static int	jev_x2_admitted = 0;
static bool jev_x2_http_done = false;
static bool jev_x2_handshake_done = false;
static int	jev_stmt_http_completed = 0;
static TimestampTz jev_x2_fault_ts = 0;

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

static void jev_register_gucs(void);
static void jev_ensure_session(void);
static void jev_x2_reset_counters(void);
static void jev_on_proc_exit(int code, Datum arg);
static void jev_xact_cb(XactEvent event, void *arg);
static void jev_subxact_cb(SubXactEvent event, SubTransactionId mySubid,
						   SubTransactionId parentSubid, void *arg);
static void jev_portal_cleanup(Portal portal);
static void jev_scope_push(QueryDesc *qd);
static void jev_scope_pop(JevExecScope *scope);
static void jev_abort_from_nest(int nest);
static void jev_harvest_generation(uint64 generation, bool cancel_rest);
static void jev_destroy_job(JevJob *job, bool finalize_http);
static void jev_cancel_jobs_subid(SubTransactionId mySubid);
static void jev_reparent_jobs(SubTransactionId mySubid, SubTransactionId parent);
static JevCfg jev_load_cfg(void);
static void jev_cred_fp(const char *key, uint8 *out);
static void jev_ident_digest(Oid userid, Oid typid, int32 typmod, Oid relid,
							 const char *query, JevKind kind, char **opts, int nopts,
							 const JevCfg *cfg, uint8 *out, char **canon_out, Size *canon_len);
static char *jev_datum_to_json(Datum rec, Oid typid, int32 typmod);
static Jsonb *jev_eval_core(FunctionCallInfo fcinfo, const char *query,
							JevKind kind, char **opts, int nopts);
static void jev_parse_text_array(ArrayType *arr, JevKind kind, char ***out, int *n);
static int32 jev_arg_typmod(FunctionCallInfo fcinfo, int argno);
static void jev_guard(const JevCfg *cfg, JevRowNode **rows, int n);
static TsHttpRequest *jev_submit_batch(JevJob *job, JevRowNode **rows, int n, bool demand);
static void jev_harvest_request(JevJob *job, TsHttpRequest *req);
static void jev_pending_drop_req(JevJob *job, TsHttpRequest *req);
static void jev_release_http_request(JevJob *job, TsHttpRequest *req);
static void jev_detach_cancelled_pending(JevJob *job);
static void jev_drop_cancelled_pending(JevJob *job);
static void jev_drop_cancelled_all(void);
static void jev_detach_cancelled_all(void);
static void jev_wait_row(JevJob *job, JevRowNode *want);
static bool jev_cache_lookup(const uint8 *ident, const uint8 *row, const char *row_json,
							 JevCacheEnt **ent);
static void jev_cache_store(const uint8 *ident, JevRowNode *row, const char *answer);
static char *jev_build_body(JevJob *job, JevRowNode **rows, int n);
static bool jev_validate_and_publish(JevJob *job, TsHttpRequest *req, Jsonb *resp);
static void jev_error_cleanup(void);
static void jev_identity_check(void);
static void jev_maybe_rebuild_http(const JevCfg *cfg);
static void jev_cancel_stage_handshake(const char *stage);
static void jev_atomic_write(const char *dir, const char *name, const char *json);
static bool jev_wait_file(const char *path, long timeout_ms);
static int64 jev_utf8_codepoints(const char *s);
static JevJob *jev_job_get(const uint8 *ident, Size ident_bytes, const char *canon,
						   JevKind kind, const char *query, char **opts, int nopts,
						   Oid typid, int32 typmod, const JevCfg *cfg);
static void jev_top_up(JevJob *job);
static bool jev_locate(JevJob *job, const uint8 *digest);
static JevRowNode *jev_row_from_json(JevJob *job, const char *json, JevBucket bucket);
static void jev_skip(JevJob *job, JevRowNode *row);
static void jev_move_bucket(JevRowNode *row, JevBucket to);
static int jev_window(void);
static void jev_notice_sweep(JevJob *job);
static Jsonb *jev_answer_jsonb(const char *answer_json);
static bool jev_finite01(float8 v);

void
jev_sha256(const void *data, Size len, uint8 *out)
{
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);

	if (ctx == NULL ||
		pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, (const uint8 *) data, len) < 0 ||
		pg_cryptohash_final(ctx, out, JEV_SHA256_LEN) < 0)
	{
		if (ctx)
			pg_cryptohash_free(ctx);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("jev: SHA-256 failed")));
	}
	pg_cryptohash_free(ctx);
}

void
jev_sha256_row(const char *json, uint8 *out)
{
	jev_sha256(json, json ? strlen(json) : 0, out);
}

Size
jev_accounted(void)
{
	int			i;
	Size		s = 0;

	for (i = 0; i < JEV_NBUCKETS; i++)
		s += jev_bucket[i];
	return s;
}

Size
jev_cap_bytes(void)
{
	return (Size) jev_guc_max_session_memory_kb * 1024UL;
}

void
jev_reserve(Size n)
{
	if (jev_accounted() + jev_reserved_bytes + n > jev_cap_bytes())
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("jev session memory limit exceeded"),
				 errhint("Call jev_cache_clear() or raise jev.max_session_memory_kb.")));
	jev_reserved_bytes += n;
}

void
jev_commit(Size n, JevBucket bucket)
{
	if (jev_reserved_bytes < n)
		jev_reserved_bytes = 0;
	else
		jev_reserved_bytes -= n;
	jev_bucket[bucket] += n;
}

void
jev_release_reserve(Size n)
{
	if (jev_reserved_bytes < n)
		jev_reserved_bytes = 0;
	else
		jev_reserved_bytes -= n;
}

void
jev_free_committed(Size n, JevBucket bucket)
{
	if (jev_bucket[bucket] < n)
		jev_bucket[bucket] = 0;
	else
		jev_bucket[bucket] -= n;
}

char *
jev_ctx_strdup(MemoryContext ctx, const char *s, JevBucket bucket)
{
	Size		n;
	char	   *copy;
	MemoryContext old;

	n = strlen(s) + 1;
	jev_reserve(n);
	old = MemoryContextSwitchTo(ctx);
	copy = pstrdup(s);
	MemoryContextSwitchTo(old);
	jev_commit(n, bucket);
	return copy;
}

static void
jev_move_bucket(JevRowNode *row, JevBucket to)
{
	if (row->bucket == to)
		return;
	jev_free_committed(row->nbytes, row->bucket);
	jev_bucket[to] += row->nbytes;
	row->bucket = to;
}

static void
jev_release_row(JevRowNode *row)
{
	if (row == NULL || row->nbytes == 0)
		return;
	jev_free_committed(row->nbytes, row->bucket);
	row->nbytes = 0;
}

static int64
jev_utf8_codepoints(const char *s)
{
	const unsigned char *p;
	int64		n = 0;

	p = (const unsigned char *) pg_server_to_any(s, strlen(s), PG_UTF8);
	while (*p)
	{
		int			l = pg_utf_mblen(p);

		if (l < 1)
			l = 1;
		p += l;
		n++;
	}
	return n;
}

static void
jev_make_nonce(void)
{
	int			i;

	if (jev_nonce_ready)
		return;
	for (i = 0; i < 16; i++)
		jev_backend_nonce[i] = (uint8) (random() & 0xFF);
	jev_nonce_ready = true;
}

static void
jev_cred_fp(const char *key, uint8 *out)
{
	pg_cryptohash_ctx *ctx;

	jev_make_nonce();
	ctx = pg_cryptohash_create(PG_SHA256);
	if (ctx == NULL || pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, jev_backend_nonce, 16) < 0 ||
		pg_cryptohash_update(ctx, (const uint8 *) key, strlen(key)) < 0 ||
		pg_cryptohash_final(ctx, out, JEV_SHA256_LEN) < 0)
	{
		if (ctx)
			pg_cryptohash_free(ctx);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("jev: SHA-256 failed")));
	}
	pg_cryptohash_free(ctx);
}

typedef struct JevJobHashEnt
{
	uint8		ident[JEV_SHA256_LEN];
	JevJob	   *job;
} JevJobHashEnt;

static void
jev_ensure_hashes(void)
{
	HASHCTL		ctl;

	if (jev_cache_hash == NULL)
	{
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(JevCacheKey);
		ctl.entrysize = sizeof(JevCacheEnt);
		ctl.hcxt = JevCacheContext;
		jev_cache_hash = hash_create("JevCache", 128, &ctl,
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	if (jev_job_hash == NULL)
	{
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(((JevJobHashEnt *) NULL)->ident);
		ctl.entrysize = sizeof(JevJobHashEnt);
		ctl.hcxt = JevSessionContext;
		jev_job_hash = hash_create("JevJobs", 32, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
}

JevJob *
jev_job_lookup_ident(const uint8 *ident)
{
	JevJobHashEnt *ent;

	if (jev_job_hash == NULL)
		return NULL;
	ent = hash_search(jev_job_hash, ident, HASH_FIND, NULL);
	return ent ? ent->job : NULL;
}

void
jev_ensure_session(void)
{
	if (JevSessionContext != NULL)
	{
		jev_ensure_hashes();
		if (jev_http == NULL)
			jev_http = ts_http_session_create(JevCurlContext);
		return;
	}

	JevSessionContext = AllocSetContextCreate(TopMemoryContext,
											  "JevSession",
											  ALLOCSET_DEFAULT_SIZES);
	JevCacheContext = AllocSetContextCreate(JevSessionContext,
											"JevCache",
											ALLOCSET_DEFAULT_SIZES);
	JevCurlContext = AllocSetContextCreate(JevSessionContext,
										   "JevCurl",
										   ALLOCSET_DEFAULT_SIZES);
	jev_ensure_hashes();
	jev_http = ts_http_session_create(JevCurlContext);
	jev_userid = GetUserId();
	jev_sessid = GetSessionUserId();
	jev_bucket[B_CONN] = ts_http_conn_bytes(jev_http);
}

static void
jev_guc_conflict(const char *name)
{
	int			flags = GetConfigOptionFlags(name, true);
	const char *val = GetConfigOption(name, true, false);

	if ((val != NULL || flags != 0) && (flags & GUC_CUSTOM_PLACEHOLDER) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("%s is already defined by another module", name)));
}

static void
jev_x2_reset_counters(void)
{
	jev_x2_admitted = 0;
	jev_x2_http_done = false;
	jev_x2_handshake_done = false;
	jev_stmt_http_completed = 0;
	jev_x2_fault_ts = 0;
	if (jev_http)
		jev_http->pause_admit = false;
}

static void
jev_assign_spi_fault(bool newval, void *extra)
{
	(void) newval;
	(void) extra;
	jev_x2_reset_counters();
}

static void
jev_x2_note_statement(TimestampTz stmt_ts)
{
	if (!jev_guc_test_spi_fault)
		return;
	if (jev_x2_fault_ts != stmt_ts)
	{
		jev_x2_fault_ts = stmt_ts;
		jev_x2_admitted = 0;
		jev_x2_http_done = false;
		jev_x2_handshake_done = false;
		jev_stmt_http_completed = 0;
		if (jev_http)
			jev_http->pause_admit = false;
	}
}

static void
jev_register_gucs(void)
{
	jev_guc_conflict("jev.api_key");

	DefineCustomStringVariable("jev.api_key",
							   "API key for TypeSafe Jev.",
							   NULL,
							   &jev_guc_api_key,
							   "",
							   PGC_SUSET,
							   GUC_NO_SHOW_ALL | GUC_SUPERUSER_ONLY | GUC_NOT_IN_SAMPLE,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("jev.api_url",
							   "TypeSafe System One HTTP endpoint for Jev.",
							   NULL,
							   &jev_guc_api_url,
							   TYPESAFE_DEFAULT_ENDPOINT,
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("jev.model",
							   "Default Jev model name.",
							   NULL,
							   &jev_guc_model,
							   TYPESAFE_DEFAULT_MODEL,
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomIntVariable("jev.batch_size",
							"Rows packed into one Jev API request.",
							NULL,
							&jev_guc_batch_size,
							20, 1, 128,
							PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("jev.concurrency",
							"Max simultaneous Jev HTTP requests.",
							NULL,
							&jev_guc_concurrency,
							16, 1, 64,
							PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("jev.max_prefetch_rows",
							"How far a Jev read-ahead may scan for a row.",
							NULL,
							&jev_guc_max_prefetch,
							5000, 1, 1000000,
							PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable("jev.notices",
							 "Emit Jev progress NOTICE messages.",
							 NULL,
							 &jev_guc_notices,
							 true,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomRealVariable("jev.timeout",
							 "HTTP timeout in seconds for Jev requests.",
							 NULL,
							 &jev_guc_timeout,
							 30.0, 0.001, 600.0,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("jev.keepalive",
							"Seconds an idle Jev HTTP connection may be reused.",
							NULL,
							&jev_guc_keepalive,
							600, 0, 86400,
							PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("jev.max_rows_per_statement",
							"Spend guard: max rows sent to the API in one statement (0=off).",
							NULL,
							&jev_guc_max_rows,
							0, 0, INT_MAX,
							PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("jev.max_chars_per_statement",
							"Spend guard: max Unicode characters of row JSON per statement (0=off).",
							NULL,
							&jev_guc_max_chars,
							0, 0, INT_MAX,
							PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("jev.max_session_memory_kb",
							"Maximum Jev-owned session memory in kilobytes.",
							NULL,
							&jev_guc_max_session_memory_kb,
							65536, 1024, 1048576,
							PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomRealVariable("jev.threshold",
							 "Default probability threshold for jev().",
							 NULL,
							 &jev_guc_threshold,
							 0.5, 0.0, 1.0,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable("jev._test_internal_spi_fault",
							 "Test-only: inject an internal SPI fault after handshake.",
							 NULL,
							 &jev_guc_test_spi_fault,
							 false,
							 PGC_SUSET, GUC_NOT_IN_SAMPLE, NULL, jev_assign_spi_fault, NULL);

	DefineCustomStringVariable("jev._test_handshake_dir",
							   "Test-only: directory for handshake files.",
							   NULL,
							   &jev_guc_handshake_dir,
							   "",
							   PGC_SUSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);

	DefineCustomStringVariable("jev._test_cancel_stage",
							   "Test-only: synthetic cancel stage (dns/connect/response/retry_sleep).",
							   NULL,
							   &jev_guc_cancel_stage,
							   "",
							   PGC_SUSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);

	MarkGUCPrefixReserved("jev");
}

JevExecScope *
jev_scope_lookup_exact(QueryDesc *qd)
{
	ListCell   *lc;

	foreach(lc, jev_scope_stack)
	{
		JevExecScope *s = (JevExecScope *) lfirst(lc);

		if (s->qd == qd)
			return s;
	}
	return NULL;
}

static void
jev_scope_push(QueryDesc *qd)
{
	JevExecScope *scope;
	MemoryContext old;

	old = MemoryContextSwitchTo(JevSessionContext ? JevSessionContext : TopMemoryContext);
	scope = (JevExecScope *) palloc0(sizeof(JevExecScope));
	scope->qd = qd;
	scope->generation = 0;
	scope->portal = ActivePortal;
	scope->prev_portal_cleanup = NULL;
	scope->nest = list_length(jev_scope_stack);
	scope->active = false;
	if (ActivePortal != NULL && ActivePortal->queryDesc == qd)
	{
		scope->prev_portal_cleanup = ActivePortal->cleanup;
		ActivePortal->cleanup = jev_portal_cleanup;
	}
	jev_scope_stack = lappend(jev_scope_stack, scope);
	MemoryContextSwitchTo(old);
}

static void
jev_scope_pop(JevExecScope *scope)
{
	jev_scope_stack = list_delete_ptr(jev_scope_stack, scope);
	pfree(scope);
}

static void
jev_cancel_job_requests(JevJob *job)
{
	HASH_SEQ_STATUS status;
	JevPendingEnt *ent;

	if (job->pending == NULL)
		return;
	hash_seq_init(&status, job->pending);
	while ((ent = hash_seq_search(&status)) != NULL)
	{
		if (ent->req && !ts_http_request_terminal(ent->req))
		{
			ts_http_cancel_request(jev_http, ent->req);
			jev_stat_cancelled++;
		}
	}
}

static void
jev_destroy_job(JevJob *job, bool finalize_http)
{
	JevJobHashEnt *hent;

	if (job == NULL || job->destroyed)
		return;
	jev_cancel_job_requests(job);
	if (job->pending)
	{
		HASH_SEQ_STATUS st;
		JevPendingEnt *ent;
		List	   *reqs = NIL;
		ListCell   *lc;

		hash_seq_init(&st, job->pending);
		while ((ent = hash_seq_search(&st)) != NULL)
		{
			if (ent->row)
				jev_release_row(ent->row);
			if (ent->req)
				reqs = list_append_unique_ptr(reqs, ent->req);
		}
		foreach(lc, reqs)
		{
			if (finalize_http)
				jev_release_http_request(job, (TsHttpRequest *) lfirst(lc));
			else
			{
				TsHttpRequest *req = (TsHttpRequest *) lfirst(lc);

				jev_pending_drop_req(job, req);
				if (job->batches != NIL)
					job->batches = list_delete_ptr(job->batches, req);
				if (req->http_acct > 0)
				{
					jev_free_committed(req->http_acct, B_HTTP);
					req->http_acct = 0;
				}
			}
		}
		list_free(reqs);
	}
	job->destroyed = true;
	hent = hash_search(jev_job_hash, job->ident_digest, HASH_FIND, NULL);
	if (hent && hent->job == job)
		hash_search(jev_job_hash, job->ident_digest, HASH_REMOVE, NULL);

	jev_free_committed(job->job_acct, B_JOB);
	/* Remaining fetched/skipped/http live in job ctx and vanish with it. */
	{
		dlist_iter	it;

		dlist_foreach(it, &job->fetched)
		{
			JevRowNode *r = dlist_container(JevRowNode, node, it.cur);

			jev_release_row(r);
		}
		dlist_foreach(it, &job->skipped)
		{
			JevRowNode *r = dlist_container(JevRowNode, node, it.cur);

			jev_release_row(r);
		}
	}
	if (job->ctx)
		MemoryContextDelete(job->ctx);
}

static void
jev_harvest_generation(uint64 generation, bool cancel_rest)
{
	HASH_SEQ_STATUS status;
	JevJobHashEnt *ent;
	List	   *jobs = NIL;
	ListCell   *lc;

	if (jev_job_hash == NULL)
		return;
	hash_seq_init(&status, jev_job_hash);
	while ((ent = hash_seq_search(&status)) != NULL)
	{
		if (ent->job && ent->job->generation == generation)
			jobs = lappend(jobs, ent->job);
	}
	foreach(lc, jobs)
	{
		JevJob	   *job = (JevJob *) lfirst(lc);

		if (job->pending)
		{
			HASH_SEQ_STATUS st2;
			JevPendingEnt *pent;
			List	   *done = NIL;
			List	   *cancel = NIL;
			ListCell   *lc2;

			hash_seq_init(&st2, job->pending);
			while ((pent = hash_seq_search(&st2)) != NULL)
			{
				if (pent->req == NULL)
					continue;
				if (ts_http_request_succeeded(pent->req) && !pent->req->harvested)
					done = list_append_unique_ptr(done, pent->req);
				else if (cancel_rest && !ts_http_request_terminal(pent->req))
					cancel = list_append_unique_ptr(cancel, pent->req);
			}
			foreach(lc2, done)
				jev_harvest_request(job, (TsHttpRequest *) lfirst(lc2));
			foreach(lc2, cancel)
			{
				ts_http_cancel_request(jev_http, (TsHttpRequest *) lfirst(lc2));
				jev_stat_cancelled++;
			}
			list_free(done);
			list_free(cancel);
		}
		if (cancel_rest)
			jev_destroy_job(job, true);
	}
	list_free(jobs);
}

static void
jev_abort_from_nest(int nest)
{
	HASH_SEQ_STATUS status;
	JevJobHashEnt *ent;
	List	   *jobs = NIL;
	List	   *scopes = NIL;
	ListCell   *lc;

	if (jev_job_hash)
	{
		hash_seq_init(&status, jev_job_hash);
		while ((ent = hash_seq_search(&status)) != NULL)
		{
			if (ent->job && ent->job->nest >= nest)
				jobs = lappend(jobs, ent->job);
		}
	}
	foreach(lc, jobs)
		jev_destroy_job((JevJob *) lfirst(lc), true);
	list_free(jobs);

	foreach(lc, jev_scope_stack)
	{
		JevExecScope *s = (JevExecScope *) lfirst(lc);

		if (s->nest >= nest)
			scopes = lappend(scopes, s);
	}
	foreach(lc, scopes)
		jev_scope_pop((JevExecScope *) lfirst(lc));
	list_free(scopes);
}

static void
jev_portal_cleanup(Portal portal)
{
	JevExecScope *scope = NULL;
	ListCell   *lc;
	void		(*prev) (Portal portal) = NULL;

	foreach(lc, jev_scope_stack)
	{
		JevExecScope *s = (JevExecScope *) lfirst(lc);

		if (s->portal == portal && s->prev_portal_cleanup != NULL)
		{
			scope = s;
			break;
		}
	}
	if (scope == NULL && portal->queryDesc != NULL)
	{
		JevExecScope *alt = jev_scope_lookup_exact(portal->queryDesc);

		if (alt != NULL && alt != scope)
		{
			jev_abort_from_nest(alt->nest);
			scope = NULL;
		}
	}
	if (scope != NULL)
	{
		prev = scope->prev_portal_cleanup;
		portal->cleanup = prev;
		jev_abort_from_nest(scope->nest);
	}
	else if (portal->cleanup == jev_portal_cleanup)
		portal->cleanup = NULL;

	if (prev)
		prev(portal);
	else if (portal->cleanup && portal->cleanup != jev_portal_cleanup)
		portal->cleanup(portal);
}

static void
jev_install_call(QueryDesc *qd, void (*inner) (void *), void *arg)
{
	JevCallContext ctx;

	ctx.qd = qd;
	ctx.scope = jev_scope_lookup_exact(qd);
	ctx.prev = jev_call_context;
	jev_call_context = &ctx;
	PG_TRY();
	{
		inner(arg);
	}
	PG_FINALLY();
	{
		jev_call_context = ctx.prev;
	}
	PG_END_TRY();
}

typedef struct JevStartArgs
{
	QueryDesc  *qd;
	int			eflags;
} JevStartArgs;

static void
jev_run_prev_start(void *arg)
{
	JevStartArgs *a = (JevStartArgs *) arg;

	if (prev_ExecutorStart)
		prev_ExecutorStart(a->qd, a->eflags);
	else
		standard_ExecutorStart(a->qd, a->eflags);
}

static void
jev_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	jev_ensure_session();
	if (jev_spi_depth == 0)
	{
		JevStartArgs a;

		jev_scope_push(queryDesc);
		a.qd = queryDesc;
		a.eflags = eflags;
		jev_install_call(queryDesc, jev_run_prev_start, &a);
	}
	else if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

typedef struct JevRunArgs
{
	QueryDesc  *qd;
	ScanDirection dir;
	uint64		count;
#if PG_VERSION_NUM < 180000
	bool		execute_once;
#endif
} JevRunArgs;

static void
jev_run_prev_run(void *arg)
{
	JevRunArgs *a = (JevRunArgs *) arg;

	if (prev_ExecutorRun)
#if PG_VERSION_NUM >= 180000
		prev_ExecutorRun(a->qd, a->dir, a->count);
	else
		standard_ExecutorRun(a->qd, a->dir, a->count);
#else
		prev_ExecutorRun(a->qd, a->dir, a->count, a->execute_once);
	else
		standard_ExecutorRun(a->qd, a->dir, a->count, a->execute_once);
#endif
}

#if PG_VERSION_NUM >= 180000
static void
jev_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
#else
static void
jev_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once)
#endif
{
	if (jev_spi_depth == 0)
	{
		JevRunArgs	a;

		a.qd = queryDesc;
		a.dir = direction;
		a.count = count;
#if PG_VERSION_NUM < 180000
		a.execute_once = execute_once;
#endif
		jev_install_call(queryDesc, jev_run_prev_run, &a);
	}
	else if (prev_ExecutorRun)
#if PG_VERSION_NUM >= 180000
		prev_ExecutorRun(queryDesc, direction, count);
	else
		standard_ExecutorRun(queryDesc, direction, count);
#else
		prev_ExecutorRun(queryDesc, direction, count, execute_once);
	else
		standard_ExecutorRun(queryDesc, direction, count, execute_once);
#endif
}

static void
jev_run_prev_finish(void *arg)
{
	QueryDesc  *qd = (QueryDesc *) arg;

	if (prev_ExecutorFinish)
		prev_ExecutorFinish(qd);
	else
		standard_ExecutorFinish(qd);
}

static void
jev_ExecutorFinish(QueryDesc *queryDesc)
{
	if (jev_spi_depth == 0)
		jev_install_call(queryDesc, jev_run_prev_finish, queryDesc);
	else if (prev_ExecutorFinish)
		prev_ExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
}

static void
jev_ExecutorEnd(QueryDesc *queryDesc)
{
	if (jev_spi_depth == 0)
	{
		JevExecScope *scope = jev_scope_lookup_exact(queryDesc);
		JevExecScope *top = NULL;

		if (jev_scope_stack != NIL)
			top = (JevExecScope *) llast(jev_scope_stack);
		if (scope == NULL)
		{
			/* nothing */
		}
		else if (top != NULL && top->qd != queryDesc)
			jev_abort_from_nest(scope->nest);
		else
		{
			if (scope->active)
				jev_harvest_generation(scope->generation, true);
			if (scope->portal != NULL &&
				scope->portal->cleanup == jev_portal_cleanup)
				scope->portal->cleanup = scope->prev_portal_cleanup;
			jev_scope_pop(scope);
		}
	}
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

static void
jev_cancel_jobs_subid(SubTransactionId mySubid)
{
	HASH_SEQ_STATUS status;
	JevJobHashEnt *ent;
	List	   *jobs = NIL;
	ListCell   *lc;

	if (jev_job_hash == NULL)
		return;
	hash_seq_init(&status, jev_job_hash);
	while ((ent = hash_seq_search(&status)) != NULL)
	{
		if (ent->job && ent->job->owner_subid == mySubid)
			jobs = lappend(jobs, ent->job);
	}
	foreach(lc, jobs)
		jev_destroy_job((JevJob *) lfirst(lc), true);
	list_free(jobs);
}

static void
jev_reparent_jobs(SubTransactionId mySubid, SubTransactionId parent)
{
	HASH_SEQ_STATUS status;
	JevJobHashEnt *ent;

	if (jev_job_hash == NULL)
		return;
	hash_seq_init(&status, jev_job_hash);
	while ((ent = hash_seq_search(&status)) != NULL)
	{
		if (ent->job && ent->job->owner_subid == mySubid)
			ent->job->owner_subid = parent;
	}
}

static void
jev_xact_cb(XactEvent event, void *arg)
{
	(void) arg;
	if (event == XACT_EVENT_ABORT)
	{
		HASH_SEQ_STATUS status;
		JevJobHashEnt *ent;
		List	   *jobs = NIL;
		ListCell   *lc;

		if (jev_job_hash)
		{
			hash_seq_init(&status, jev_job_hash);
			while ((ent = hash_seq_search(&status)) != NULL)
				jobs = lappend(jobs, ent->job);
		}
		foreach(lc, jobs)
			jev_destroy_job((JevJob *) lfirst(lc), true);
		list_free(jobs);
		while (jev_scope_stack != NIL)
			jev_scope_pop((JevExecScope *) linitial(jev_scope_stack));
		jev_call_context = NULL;
		jev_reserved_bytes = 0;
		jev_x2_admitted = 0;
		jev_x2_http_done = false;
		jev_x2_handshake_done = false;
		jev_stmt_http_completed = 0;
	}
}

static void
jev_subxact_cb(SubXactEvent event, SubTransactionId mySubid,
			   SubTransactionId parentSubid, void *arg)
{
	(void) arg;
	if (jev_spi_depth > 0)
		return;
	if (event == SUBXACT_EVENT_COMMIT_SUB)
		jev_reparent_jobs(mySubid, parentSubid);
	else if (event == SUBXACT_EVENT_ABORT_SUB)
		jev_cancel_jobs_subid(mySubid);
}

static void
jev_on_proc_exit(int code, Datum arg)
{
	(void) code;
	(void) arg;
	if (jev_http)
	{
		ts_http_session_destroy(jev_http);
		jev_http = NULL;
	}
}

void
jev_startup(void)
{
	jev_register_gucs();
	JevSessionContext = AllocSetContextCreate(TopMemoryContext,
											  "JevSession",
											  ALLOCSET_DEFAULT_SIZES);
	JevCacheContext = AllocSetContextCreate(JevSessionContext,
											"JevCache",
											ALLOCSET_DEFAULT_SIZES);
	JevCurlContext = AllocSetContextCreate(JevSessionContext,
										   "JevCurl",
										   ALLOCSET_DEFAULT_SIZES);

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = jev_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = jev_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = jev_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = jev_ExecutorEnd;
	ts_http_success_hook = jev_x2_on_http_complete;

	RegisterXactCallback(jev_xact_cb, NULL);
	RegisterSubXactCallback(jev_subxact_cb, NULL);
	on_proc_exit(jev_on_proc_exit, 0);
}

static JevCfg
jev_load_cfg(void)
{
	JevCfg		cfg;
	const char *key;
	const char *env;

	memset(&cfg, 0, sizeof(cfg));
	key = (jev_guc_api_key && jev_guc_api_key[0] != '\0') ? jev_guc_api_key : NULL;
	if (key == NULL)
	{
		env = getenv("TYPESAFE_API_KEY");
		if (env != NULL && env[0] != '\0')
			key = env;
	}
	if (key == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("jev: no API key. SET jev.api_key = '...' or start the server with TYPESAFE_API_KEY set.")));

	ts_http_reject_header_injection(key);
	cfg.api_key = pstrdup(key);
	cfg.api_url = pstrdup((jev_guc_api_url && jev_guc_api_url[0]) ? jev_guc_api_url : TYPESAFE_DEFAULT_ENDPOINT);
	ts_http_check_endpoint(cfg.api_url, "jev.api_url");
	cfg.model = pstrdup((jev_guc_model && jev_guc_model[0]) ? jev_guc_model : TYPESAFE_DEFAULT_MODEL);
	cfg.batch_size = jev_guc_batch_size;
	cfg.concurrency = jev_guc_concurrency;
	cfg.max_prefetch = jev_guc_max_prefetch;
	cfg.notices = jev_guc_notices;
	cfg.timeout_s = jev_guc_timeout;
	cfg.keepalive_s = jev_guc_keepalive;
	cfg.max_rows = jev_guc_max_rows;
	cfg.max_chars = jev_guc_max_chars;
	cfg.max_session_memory_kb = jev_guc_max_session_memory_kb;
	cfg.threshold = jev_guc_threshold;
	cfg.stmt_ts = GetCurrentStatementStartTimestamp();
	jev_cred_fp(cfg.api_key, cfg.cred_fp);
	return cfg;
}

static void
jev_ident_digest(Oid userid, Oid typid, int32 typmod, Oid relid,
				const char *query, JevKind kind, char **opts, int nopts,
				const JevCfg *cfg, uint8 *out, char **canon_out, Size *canon_len)
{
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);
	appendStringInfo(&buf, "u=%u;t=%u;m=%d;r=%u;k=%d;q=",
					userid, typid, typmod, relid, (int) kind);
	appendStringInfoString(&buf, query ? query : "");
	appendStringInfoString(&buf, ";opts=");
	for (i = 0; i < nopts; i++)
	{
		if (i)
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, opts[i]);
	}
	appendStringInfo(&buf, ";model=%s;url=%s;fp=", cfg->model, cfg->api_url);
	for (i = 0; i < JEV_SHA256_LEN; i++)
		appendStringInfo(&buf, "%02x", cfg->cred_fp[i]);
	jev_sha256(buf.data, buf.len, out);
	if (canon_out)
	{
		*canon_out = buf.data;
		*canon_len = buf.len;
	}
}

static int32
jev_arg_typmod(FunctionCallInfo fcinfo, int argno)
{
	if (fcinfo->flinfo == NULL || fcinfo->flinfo->fn_expr == NULL)
		return -1;
	if (IsA(fcinfo->flinfo->fn_expr, FuncExpr))
	{
		List	   *args = ((FuncExpr *) fcinfo->flinfo->fn_expr)->args;

		if (argno >= list_length(args))
			return -1;
		return exprTypmod((Node *) list_nth(args, argno));
	}
	return -1;
}

char *
jev_canon_row_json(const char *json)
{
	/* Match pg-jev: spend-guard counts raw to_json()::text, not jsonb pretty-print. */
	if (json == NULL || json[0] == '\0')
		return pstrdup("null");
	return pstrdup(json);
}

static char *
jev_datum_to_json(Datum rec, Oid typid, int32 typmod)
{
	static Oid to_json_oid = InvalidOid;
	FmgrInfo	flinfo;
	LOCAL_FCINFO(fcinfo, 1);
	Const	   *c;
	FuncExpr   *fe;
	Datum		result;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	Oid			arg = ANYELEMENTOID;
	char	   *textjson;

	if (!OidIsValid(to_json_oid))
		to_json_oid = LookupFuncName(list_make1(makeString("to_json")),
									1, &arg, false);
	get_typlenbyvalalign(typid, &typlen, &typbyval, &typalign);
	if (!typbyval && rec != (Datum) 0)
	{
		HeapTupleHeader hdr;
		HeapTupleData tmptup;
		HeapTuple	copy;

		hdr = DatumGetHeapTupleHeader(rec);
		memset(&tmptup, 0, sizeof(tmptup));
		tmptup.t_len = HeapTupleHeaderGetDatumLength(hdr);
		ItemPointerSetInvalid(&tmptup.t_self);
		tmptup.t_tableOid = InvalidOid;
		tmptup.t_data = hdr;
		copy = heap_copytuple(&tmptup);
		rec = HeapTupleGetDatum(copy);
	}
	fmgr_info(to_json_oid, &flinfo);
	c = makeConst(typid, typmod, InvalidOid, typlen, rec, false, typbyval);
	fe = makeFuncExpr(to_json_oid, JSONOID, list_make1(c),
					InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	flinfo.fn_expr = (Node *) fe;
	InitFunctionCallInfoData(*fcinfo, &flinfo, 1, InvalidOid, NULL, NULL);
	fcinfo->args[0].value = rec;
	fcinfo->args[0].isnull = false;
	result = FunctionCallInvoke(fcinfo);
	if (fcinfo->isnull)
		return pstrdup("null");
	textjson = TextDatumGetCString(result);
	return jev_canon_row_json(textjson);
}

static void
jev_parse_text_array(ArrayType *arr, JevKind kind, char ***out, int *n)
{
	Datum	   *datums;
	bool	   *nulls;
	int			nelems;
	int			i;
	char	  **vals;
	int			nout = 0;

	*out = NULL;
	*n = 0;
	if (arr == NULL)
	{
		if (kind != JEV_KIND_NOUL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("jev: %s needs a text[] of %s",
							kind == JEV_KIND_SCORE ? "score" : "choice",
							kind == JEV_KIND_SCORE ? "levels" : "options")));
		return;
	}
	if (ARR_NDIM(arr) == 0)
	{
		if (kind != JEV_KIND_NOUL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("jev: %s needs a text[] of %s",
							kind == JEV_KIND_SCORE ? "score" : "choice",
							kind == JEV_KIND_SCORE ? "levels" : "options")));
		return;
	}
	if (ARR_NDIM(arr) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("jev: options must be a one-dimensional array")));
#if PG_VERSION_NUM >= 160000
	deconstruct_array_builtin(arr, TEXTOID, &datums, &nulls, &nelems);
#else
	deconstruct_array(arr, TEXTOID, -1, false, TYPALIGN_INT, &datums, &nulls, &nelems);
#endif
	vals = (char **) palloc(sizeof(char *) * nelems);
	for (i = 0; i < nelems; i++)
	{
		char	   *s;
		int			j;
		bool		dup = false;

		if (nulls && nulls[i])
		{
			if (kind != JEV_KIND_NOUL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("jev: %s must not contain nulls",
								kind == JEV_KIND_SCORE ? "levels" : "options")));
			continue;
		}
		s = TextDatumGetCString(datums[i]);
		if (kind == JEV_KIND_CHOICE)
		{
			for (j = 0; j < nout; j++)
			{
				if (strcmp(vals[j], s) == 0)
				{
					dup = true;
					break;
				}
			}
			if (dup)
				continue;
		}
		vals[nout++] = s;
	}
	if (kind == JEV_KIND_CHOICE && nout == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("jev: choice needs a text[] of options")));
	if (kind == JEV_KIND_SCORE && nout < 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("jev: score needs a text[] of levels")));
	*out = vals;
	*n = nout;
}

static char *
jev_build_body(JevJob *job, JevRowNode **rows, int n)
{
	StringInfoData buf;
	int			i;
	MemoryContext old;

	old = MemoryContextSwitchTo(job->ctx);
	initStringInfo(&buf);
	appendStringInfoString(&buf, "{\"model\": ");
	escape_json(&buf, job->cfg.model);
	appendStringInfoString(&buf, ", \"state\": {");
	if (job->kind == JEV_KIND_NOUL)
	{
		appendStringInfoString(&buf, "\"condition\": ");
		escape_json(&buf, job->query);
		appendStringInfoString(&buf, ", ");
	}
	appendStringInfoString(&buf, "\"rows\": [");
	for (i = 0; i < n; i++)
	{
		if (i)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, rows[i]->json);
	}
	appendStringInfoString(&buf, "]}, \"questions\": {");
	for (i = 0; i < n; i++)
	{
		char		ref[32];

		snprintf(ref, sizeof(ref), "rows[%d]", i);
		if (i)
			appendStringInfoString(&buf, ", ");
		appendStringInfo(&buf, "\"r%d\": {\"type\": ", i);
		if (job->kind == JEV_KIND_NOUL)
		{
			appendStringInfoString(&buf, "\"noul\", \"instructions\": ");
			{
				char	   *ins = psprintf("Does the record `%s` satisfy the condition stated in `condition`?", ref);

				escape_json(&buf, ins);
				pfree(ins);
			}
		}
		else if (job->kind == JEV_KIND_SCORE)
		{
			char	   *ins = psprintf("Rate the record `%s`: %s", ref, job->query);
			int			j;

			appendStringInfoString(&buf, "\"score\", \"instructions\": ");
			escape_json(&buf, ins);
			pfree(ins);
			appendStringInfoString(&buf, ", \"criteria\": [");
			for (j = 0; j < job->noptions; j++)
			{
				if (j)
					appendStringInfoString(&buf, ", ");
				escape_json(&buf, job->options[j]);
			}
			appendStringInfoChar(&buf, ']');
		}
		else
		{
			char	   *ins = psprintf("For the record `%s`: %s", ref, job->query);
			int			j;

			appendStringInfoString(&buf, "\"choice\", \"instructions\": ");
			escape_json(&buf, ins);
			pfree(ins);
			appendStringInfoString(&buf, ", \"criteria\": {");
			for (j = 0; j < job->noptions; j++)
			{
				if (j)
					appendStringInfoString(&buf, ", ");
				escape_json(&buf, job->options[j]);
				appendStringInfoString(&buf, ": null");
			}
			appendStringInfoChar(&buf, '}');
		}
		appendStringInfoChar(&buf, '}');
	}
	appendStringInfoString(&buf, "}}");
	MemoryContextSwitchTo(old);
	return buf.data;
}

static bool
jev_finite01(float8 v)
{
	return isfinite(v) && v >= 0.0 && v <= 1.0;
}

static Jsonb *
jev_answer_jsonb(const char *answer_json)
{
	return DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(answer_json)));
}

static void
jev_atomic_write(const char *dir, const char *name, const char *json)
{
	char		path[MAXPGPATH];
	char		tmp[MAXPGPATH];
	FILE	   *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (f == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				errmsg("could not write %s: %m", tmp)));
	fwrite(json, 1, strlen(json), f);
	fputc('\n', f);
	fflush(f);
#ifndef WIN32
	fsync(fileno(f));
#endif
	fclose(f);
	if (rename(tmp, path) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				errmsg("could not rename %s: %m", tmp)));
}

static bool
jev_wait_file(const char *path, long timeout_ms)
{
	instr_time	start;
	instr_time	now;

	INSTR_TIME_SET_CURRENT(start);
	for (;;)
	{
		struct stat st;

		if (stat(path, &st) == 0)
			return true;
		if (ProcDiePending)
			ProcessInterrupts();
		if (QueryCancelPending)
			return false;
		INSTR_TIME_SET_CURRENT(now);
		if ((INSTR_TIME_GET_MICROSEC(now) - INSTR_TIME_GET_MICROSEC(start)) / 1000 > timeout_ms)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					errmsg("jev_test: handshake timeout waiting for %s", path)));
		(void) WaitLatch(MyLatch,
						WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						10,
						PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

static void
jev_cancel_stage_handshake(const char *stage)
{
	char		ready[MAXPGPATH];
	char		ack[MAXPGPATH];
	char		json[256];

	if (jev_guc_handshake_dir == NULL || jev_guc_handshake_dir[0] == '\0')
		return;
	strlcpy(jev_last_cancel_stage, stage, sizeof(jev_last_cancel_stage));
	snprintf(json, sizeof(json), "{\"backend_pid\": %d, \"stage\": \"%s\"}",
			MyProcPid, stage);
	jev_atomic_write(jev_guc_handshake_dir, "cancel-stage-ready.json", json);
	snprintf(ready, sizeof(ready), "%s/cancel-stage-ready.json", jev_guc_handshake_dir);
	snprintf(ack, sizeof(ack), "%s/cancel-stage.ack", jev_guc_handshake_dir);
	if (!jev_wait_file(ack, 10000))
	{
		if (ProcDiePending)
			ProcessInterrupts();
		if (QueryCancelPending)
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					errmsg("canceling statement due to user request")));
	}
	if (ProcDiePending)
		ProcessInterrupts();
	if (QueryCancelPending)
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				errmsg("canceling statement due to user request")));
}

void
jev_x2_on_http_complete(void)
{
	jev_stmt_http_completed++;
	if (jev_guc_test_spi_fault && jev_guc_handshake_dir &&
		jev_guc_handshake_dir[0] && !jev_x2_http_done && jev_stmt_http_completed >= 1)
	{
		char		json[256];

		snprintf(json, sizeof(json),
				"{\"backend_pid\": %d, \"http_completed\": %d, \"in_flight\": %d}",
				MyProcPid, jev_stmt_http_completed, ts_http_window_count(jev_http));
		jev_atomic_write(jev_guc_handshake_dir, "x2-http-done.json", json);
		jev_x2_http_done = true;
	}
}

void
jev_x2_maybe_fault(void)
{
	char		json[256];
	char		ack[MAXPGPATH];

	if (!jev_guc_test_spi_fault || jev_x2_handshake_done)
		return;
	if (jev_guc_handshake_dir == NULL || jev_guc_handshake_dir[0] == '\0')
		return;
	if (jev_x2_admitted < 2 || jev_stmt_http_completed < 1)
		return;

	snprintf(json, sizeof(json),
			"{\"backend_pid\": %d, \"jobs\": %d, \"in_flight\": %d}",
			MyProcPid, jev_x2_admitted, ts_http_window_count(jev_http));
	jev_atomic_write(jev_guc_handshake_dir, "x2-job-ready.json", json);
	snprintf(ack, sizeof(ack), "%s/x2-handshake.ack", jev_guc_handshake_dir);
	(void) jev_wait_file(ack, 10000);
	jev_x2_handshake_done = true;
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			errmsg("jev_test: internal SPI fault")));
}

static int
jev_window(void)
{
	return ts_http_window_count(jev_http);
}

static void
jev_guard(const JevCfg *cfg, JevRowNode **rows, int n)
{
	int64		add_chars = 0;
	int			i;

	if (jev_spend_ts != cfg->stmt_ts)
	{
		jev_spend_ts = cfg->stmt_ts;
		jev_spend_rows = 0;
		jev_spend_chars = 0;
	}
	for (i = 0; i < n; i++)
		add_chars += jev_utf8_codepoints(rows[i]->json);
	if (cfg->max_rows && jev_spend_rows + n > cfg->max_rows)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("jev: this statement would send %lld rows to the API, above jev.max_rows_per_statement = %d",
						(long long) (jev_spend_rows + n), cfg->max_rows)));
	if (cfg->max_chars && jev_spend_chars + add_chars > cfg->max_chars)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("jev: this statement would send %lld characters of row data to the API, above jev.max_chars_per_statement = %d",
						(long long) (jev_spend_chars + add_chars), cfg->max_chars)));
	jev_spend_rows += n;
	jev_spend_chars += add_chars;
}

static JevRowNode *
jev_row_from_json(JevJob *job, const char *json, JevBucket bucket)
{
	JevRowNode *row;
	MemoryContext old;
	char	   *canon = jev_canon_row_json(json);
	Size		n = strlen(canon) + 1;

	jev_reserve(n + sizeof(JevRowNode));
	old = MemoryContextSwitchTo(job->ctx);
	row = (JevRowNode *) palloc0(sizeof(JevRowNode));
	row->json = pstrdup(canon);
	row->nbytes = n;
	row->bucket = bucket;
	jev_sha256_row(row->json, row->digest);
	MemoryContextSwitchTo(old);
	jev_commit(n + sizeof(JevRowNode), bucket);
	row->nbytes = n;
	/* node size charged to B_JOB */
	jev_free_committed(sizeof(JevRowNode), bucket);
	jev_bucket[B_JOB] += sizeof(JevRowNode);
	job->job_acct += sizeof(JevRowNode);
	return row;
}

static void
jev_skip(JevJob *job, JevRowNode *row)
{
	bool		found = false;
	JevPendingKey key;
	JevPendingEnt *ent;

	memcpy(key.digest, row->digest, JEV_SHA256_LEN);
	ent = hash_search(job->skipped_map, &key, HASH_ENTER, &found);
	if (found)
	{
		jev_release_row(row);
		return;
	}
	ent->row = row;
	jev_move_bucket(row, B_SKIPPED);
	dlist_push_tail(&job->skipped, &row->node);
	row->on_queue = true;
	job->n_skipped++;
	while (job->n_skipped > job->cfg.max_prefetch)
	{
		dlist_node *n = dlist_pop_head_node(&job->skipped);
		JevRowNode *old = dlist_container(JevRowNode, node, n);
		JevPendingKey k;

		memcpy(k.digest, old->digest, JEV_SHA256_LEN);
		hash_search(job->skipped_map, &k, HASH_REMOVE, NULL);
		old->on_queue = false;
		jev_release_row(old);
		job->n_skipped--;
	}
}

static bool
jev_locate(JevJob *job, const uint8 *digest)
{
	int			scanned = 0;

	for (;;)
	{
		while (!dlist_is_empty(&job->fetched))
		{
			JevRowNode *head = dlist_head_element(JevRowNode, node, &job->fetched);

			if (memcmp(head->digest, digest, JEV_SHA256_LEN) == 0)
				return true;
			dlist_pop_head_node(&job->fetched);
			job->n_fetched--;
			head->on_queue = false;
			jev_skip(job, head);
			scanned++;
		}
		if (scanned >= job->cfg.max_prefetch)
			return false;
		if (jev_scan_fetch_page(job, JEV_SPI_PAGE_ROWS) <= 0)
			return false;
	}
}

static void
jev_sync_conn_acct(void)
{
	Size		n = ts_http_conn_bytes(jev_http);

	jev_bucket[B_CONN] = n;
}

static void
jev_requeue_fetched(JevJob *job, JevRowNode **rows, int n)
{
	int			i;

	for (i = n - 1; i >= 0; i--)
	{
		if (rows[i] == NULL || rows[i]->on_queue)
			continue;
		dlist_push_head(&job->fetched, &rows[i]->node);
		rows[i]->on_queue = true;
		job->n_fetched++;
	}
}

static TsHttpRequest *
jev_submit_batch(JevJob *job, JevRowNode **rows, int n, bool demand)
{
	TsHttpPolicy pol;
	TsHttpRequest *req;
	char	   *body;
	int			i;
	Size		body_n;

	(void) demand;
	if (n <= 0)
		return NULL;

	{
		JevRowNode *kept[128];
		int			m = 0;
		JevCacheEnt *cent;
		JevPendingKey key;

		for (i = 0; i < n; i++)
		{
			memcpy(key.digest, rows[i]->digest, JEV_SHA256_LEN);
			if (hash_search(job->pending, &key, HASH_FIND, NULL) != NULL ||
				jev_cache_lookup(job->ident_digest, rows[i]->digest, rows[i]->json, &cent))
			{
				if (!rows[i]->on_queue)
					jev_release_row(rows[i]);
				continue;
			}
			kept[m++] = rows[i];
		}
		for (i = 0; i < m; i++)
			rows[i] = kept[i];
		n = m;
	}
	if (n <= 0)
		return NULL;

	jev_x2_note_statement(job->cfg.stmt_ts);
	if (jev_guc_test_spi_fault && jev_x2_admitted >= 1 && jev_http)
	{
		char		ack[MAXPGPATH];

		/*
		 * First job is already queued. Pump it to completion before pausing
		 * further admits; pause_admit would otherwise leave that QUEUED
		 * request unlaunched.
		 */
		while (!jev_x2_http_done)
		{
			ts_http_pump(jev_http, TYPESAFE_WAIT_QUANTUM_MS);
			CHECK_FOR_INTERRUPTS();
		}
		jev_http->pause_admit = true;
		snprintf(ack, sizeof(ack), "%s/x2-admit-next.ack", jev_guc_handshake_dir ? jev_guc_handshake_dir : "");
		(void) jev_wait_file(ack, 10000);
		jev_http->pause_admit = false;
	}

	jev_guard(&job->cfg, rows, n);
	body = jev_build_body(job, rows, n);
	body_n = strlen(body) + 1;
	if (jev_accounted() + jev_reserved_bytes + body_n > jev_cap_bytes())
	{
		jev_requeue_fetched(job, rows, n);
		pfree(body);
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("jev session memory limit exceeded"),
				errhint("Call jev_cache_clear() or raise jev.max_session_memory_kb.")));
	}
	jev_reserve(body_n);

	memset(&pol, 0, sizeof(pol));
	pol.endpoint = job->cfg.api_url;
	pol.credential = job->cfg.api_key;
	pol.guc_name = "jev.api_url";
	pol.user_agent = JEV_USER_AGENT;
	pol.timeout_ms = (long) ceil(job->cfg.timeout_s * 1000.0);
	pol.max_attempts = JEV_MAX_ATTEMPTS;
	pol.retry_5xx = true;
	pol.http11 = true;
	pol.keepalive_s = job->cfg.keepalive_s;
	pol.concurrency = job->cfg.concurrency;
	pol.max_response = TYPESAFE_MAX_RESPONSE_BYTES;
	pol.family = TS_FAMILY_JEV;
	pol.require_asynch_dns = true;
	pol.owner_generation = job->curl_generation;

	if (jev_guc_cancel_stage && jev_guc_cancel_stage[0])
		jev_cancel_stage_handshake(jev_guc_cancel_stage);

	req = ts_http_submit(jev_http, &pol, body, job->curl_generation);
	req->http_acct = body_n;
	req->owner = job;
	jev_commit(body_n, B_HTTP);
	pfree(body);

	for (i = 0; i < n; i++)
	{
		bool		found = false;
		JevPendingEnt *ent;
		JevPendingKey key;

		rows[i]->req = req;
		rows[i]->qid = i;
		memcpy(key.digest, rows[i]->digest, JEV_SHA256_LEN);
		ent = hash_search(job->pending, &key, HASH_ENTER, &found);
		ent->row = rows[i];
		ent->req = req;
	}
	{
		MemoryContext old = MemoryContextSwitchTo(job->ctx);

		job->batches = lappend(job->batches, req);
		MemoryContextSwitchTo(old);
	}
	jev_x2_admitted++;
	jev_x2_maybe_fault();
	return req;
}

static void
jev_top_up(JevJob *job)
{
	int			cap = 2 * job->cfg.concurrency;
	int			submitted = 0;

	/*
	 * Keep the 2x concurrency pipeline, but do not harvest-and-refill past
	 * one window in a single call. Fast loopback HTTP otherwise completes
	 * in-flight work inside this loop and judges far more than the filtered
	 * extra<=800 contract.
	 */
	while (jev_window() < cap && submitted < cap)
	{
		JevRowNode *batch[128];
		int			n = 0;

		jev_harvest_job(job);
		jev_drop_cancelled_pending(job);

		while (job->n_fetched < job->cfg.batch_size && !job->exhausted)
		{
			if (jev_scan_fetch_page(job, JEV_SPI_PAGE_ROWS) <= 0)
				break;
		}
		if (dlist_is_empty(&job->fetched))
			return;
		while (n < job->cfg.batch_size && !dlist_is_empty(&job->fetched))
		{
			JevRowNode *r = dlist_head_element(JevRowNode, node, &job->fetched);

			dlist_pop_head_node(&job->fetched);
			job->n_fetched--;
			r->on_queue = false;
			batch[n++] = r;
		}
		if (n == 0)
			return;
		jev_submit_batch(job, batch, n, false);
		submitted++;
	}
}

static bool
jev_validate_and_publish(JevJob *job, TsHttpRequest *req, Jsonb *resp)
{
	JsonbValue	abuf;
	JsonbValue *answers;
	JsonbContainer *acont;
	int			n = 0;
	int			i;
	ListCell   *lc;
	JevRowNode *rows[128];

	foreach(lc, job->batches)
	{
		if (lfirst(lc) == req)
			break;
	}
	{
		HASH_SEQ_STATUS st;
		JevPendingEnt *ent;

		hash_seq_init(&st, job->pending);
		while ((ent = hash_seq_search(&st)) != NULL)
		{
			if (ent->req == req && n < 128)
			{
				rows[n] = ent->row;
				n++;
			}
		}
	}
	/* sort by qid */
	for (i = 0; i < n; i++)
	{
		int			j;

		for (j = i + 1; j < n; j++)
		{
			if (rows[j]->qid < rows[i]->qid)
			{
				JevRowNode *tmp = rows[i];

				rows[i] = rows[j];
				rows[j] = tmp;
			}
		}
	}

	answers = getKeyJsonValueFromContainer(&resp->root, "answers", 7, &abuf);
	acont = jsonb_value_object_container(answers);
	if (acont == NULL || n <= 0)
		return false;

	for (i = 0; i < n; i++)
	{
		char		qid[16];
		JsonbValue	qbuf;
		JsonbValue *qval;
		JsonbContainer *qcont;
		JsonbValue	tbuf;
		JsonbValue *typev;
		JsonbValue	vbuf;
		JsonbValue *v;

		snprintf(qid, sizeof(qid), "r%d", rows[i]->qid);
		qval = getKeyJsonValueFromContainer(acont, qid, strlen(qid), &qbuf);
		qcont = jsonb_value_object_container(qval);
		if (qcont == NULL)
			return false;
		typev = getKeyJsonValueFromContainer(qcont, "type", 4, &tbuf);
		if (job->kind == JEV_KIND_NOUL && !jsonb_str_eq(typev, "noul"))
			return false;
		if (job->kind == JEV_KIND_SCORE && !jsonb_str_eq(typev, "score"))
			return false;
		if (job->kind == JEV_KIND_CHOICE && !jsonb_str_eq(typev, "choice"))
			return false;
		if (job->kind == JEV_KIND_NOUL)
		{
			v = getKeyJsonValueFromContainer(qcont, "noul", 4, &vbuf);
			if (v == NULL || v->type != jbvNumeric || !jev_finite01(jsonb_numeric_float8(v)))
				return false;
		}
		else if (job->kind == JEV_KIND_SCORE)
		{
			float8		score;

			v = getKeyJsonValueFromContainer(qcont, "score", 5, &vbuf);
			if (v == NULL || v->type != jbvNumeric)
				return false;
			score = jsonb_numeric_float8(v);
			if (!isfinite(score) || score < 0 || score > (float8) (job->noptions - 1))
				return false;
		}
		else
		{
			char	   *choice;
			int			j;
			bool		ok = false;

			v = getKeyJsonValueFromContainer(qcont, "choice", 6, &vbuf);
			if (v == NULL || v->type != jbvString)
				return false;
			choice = jsonb_str_cstring(v);
			for (j = 0; j < job->noptions; j++)
			{
				if (strcmp(choice, job->options[j]) == 0)
					ok = true;
			}
			if (!ok)
				return false;
		}
		v = getKeyJsonValueFromContainer(qcont, "confidence", 10, &vbuf);
		if (v != NULL)
		{
			if (v->type != jbvNumeric || !jev_finite01(jsonb_numeric_float8(v)))
				return false;
		}
	}

	for (i = 0; i < n; i++)
	{
		char		qid[16];
		JsonbValue	qbuf;
		JsonbValue *qval;
		Jsonb	   *aj;
		char	   *ajson;

		snprintf(qid, sizeof(qid), "r%d", rows[i]->qid);
		qval = getKeyJsonValueFromContainer(acont, qid, strlen(qid), &qbuf);
		aj = jsonb_value_object_to_jsonb(qval);
		ajson = JsonbToCString(NULL, &aj->root, VARSIZE(aj));
		jev_cache_store(job->ident_digest, rows[i], ajson);
	}
	return true;
}

static void
jev_cache_store(const uint8 *ident, JevRowNode *row, const char *answer)
{
	JevCacheKey key;
	JevCacheEnt *ent;
	bool		found;
	Size		ans_n = strlen(answer) + 1;
	MemoryContext old;

	memcpy(key.ident, ident, JEV_SHA256_LEN);
	memcpy(key.row, row->digest, JEV_SHA256_LEN);
	jev_reserve(row->nbytes + ans_n);
	old = MemoryContextSwitchTo(JevCacheContext);
	ent = hash_search(jev_cache_hash, &key, HASH_ENTER, &found);
	if (!found)
	{
		ent->row_json = pstrdup(row->json);
		ent->row_nbytes = row->nbytes;
		ent->answer_json = pstrdup(answer);
		ent->answer_nbytes = ans_n;
		MemoryContextSwitchTo(old);
		jev_commit(row->nbytes + ans_n, B_CACHE);
	}
	else
	{
		MemoryContextSwitchTo(old);
		jev_release_reserve(row->nbytes + ans_n);
	}
}

static bool
jev_cache_lookup(const uint8 *ident, const uint8 *row, const char *row_json,
				JevCacheEnt **ent_out)
{
	JevCacheKey key;
	JevCacheEnt *ent;

	(void) row_json;

	memcpy(key.ident, ident, JEV_SHA256_LEN);
	memcpy(key.row, row, JEV_SHA256_LEN);
	ent = hash_search(jev_cache_hash, &key, HASH_FIND, NULL);
	if (ent == NULL)
		return false;
	*ent_out = ent;
	return true;
}

static void
jev_pending_drop_req(JevJob *job, TsHttpRequest *req)
{
	HASH_SEQ_STATUS st;
	JevPendingEnt *ent;
	List	   *keys = NIL;
	ListCell   *lc;

	if (job == NULL || job->pending == NULL || req == NULL)
		return;
	hash_seq_init(&st, job->pending);
	while ((ent = hash_seq_search(&st)) != NULL)
	{
		if (ent->req == req)
		{
			JevPendingKey *k = (JevPendingKey *) palloc(sizeof(JevPendingKey));

			*k = ent->key;
			keys = lappend(keys, k);
			if (ent->row)
			{
				ent->row->req = NULL;
				if (!ent->row->on_queue)
					jev_release_row(ent->row);
			}
		}
	}
	foreach(lc, keys)
	{
		hash_search(job->pending, lfirst(lc), HASH_REMOVE, NULL);
		pfree(lfirst(lc));
	}
	list_free(keys);
}

static void
jev_release_http_request(JevJob *job, TsHttpRequest *req)
{
	Size		acct;

	if (req == NULL)
		return;
	if (job != NULL)
	{
		jev_pending_drop_req(job, req);
		if (job->batches != NIL)
			job->batches = list_delete_ptr(job->batches, req);
	}
	acct = req->http_acct;
	req->http_acct = 0;
	if (acct > 0)
		jev_free_committed(acct, B_HTTP);
	if (req->session != NULL)
		ts_http_recycle_request(req->session, req);
	else
		ts_http_recycle_request(jev_http, req);
}

static void
jev_detach_cancelled_pending(JevJob *job)
{
	HASH_SEQ_STATUS st;
	JevPendingEnt *ent;
	List	   *reqs = NIL;
	ListCell   *lc;

	if (job == NULL || job->pending == NULL)
		return;
	hash_seq_init(&st, job->pending);
	while ((ent = hash_seq_search(&st)) != NULL)
	{
		if (ent->req && ent->req->state == TS_HTTP_CANCELLED)
			reqs = list_append_unique_ptr(reqs, ent->req);
	}
	foreach(lc, reqs)
	{
		TsHttpRequest *req = (TsHttpRequest *) lfirst(lc);

		jev_pending_drop_req(job, req);
		if (job->batches != NIL)
			job->batches = list_delete_ptr(job->batches, req);
		if (req->http_acct > 0)
		{
			jev_free_committed(req->http_acct, B_HTTP);
			req->http_acct = 0;
		}
	}
	list_free(reqs);
}

static void
jev_drop_cancelled_pending(JevJob *job)
{
	HASH_SEQ_STATUS st;
	JevPendingEnt *ent;
	List	   *reqs = NIL;
	ListCell   *lc;

	if (job == NULL || job->pending == NULL)
		return;
	hash_seq_init(&st, job->pending);
	while ((ent = hash_seq_search(&st)) != NULL)
	{
		if (ent->req && ent->req->state == TS_HTTP_CANCELLED)
			reqs = list_append_unique_ptr(reqs, ent->req);
	}
	foreach(lc, reqs)
		jev_release_http_request(job, (TsHttpRequest *) lfirst(lc));
	list_free(reqs);
}

static void
jev_detach_cancelled_all(void)
{
	HASH_SEQ_STATUS st;
	JevJobHashEnt *ent;
	List	   *jobs = NIL;
	ListCell   *lc;

	if (jev_job_hash == NULL)
		return;
	hash_seq_init(&st, jev_job_hash);
	while ((ent = hash_seq_search(&st)) != NULL)
	{
		if (ent->job && !ent->job->destroyed)
			jobs = lappend(jobs, ent->job);
	}
	foreach(lc, jobs)
		jev_detach_cancelled_pending((JevJob *) lfirst(lc));
	list_free(jobs);
}

static void
jev_drop_cancelled_all(void)
{
	HASH_SEQ_STATUS st;
	JevJobHashEnt *ent;
	List	   *jobs = NIL;
	ListCell   *lc;

	if (jev_job_hash == NULL)
		return;
	hash_seq_init(&st, jev_job_hash);
	while ((ent = hash_seq_search(&st)) != NULL)
	{
		if (ent->job && !ent->job->destroyed)
			jobs = lappend(jobs, ent->job);
	}
	foreach(lc, jobs)
		jev_drop_cancelled_pending((JevJob *) lfirst(lc));
	list_free(jobs);
}

void
jev_harvest_job(JevJob *job)
{
	HASH_SEQ_STATUS st;
	JevPendingEnt *ent;
	List	   *reqs = NIL;
	ListCell   *lc;

	if (job == NULL || job->pending == NULL)
		return;
	hash_seq_init(&st, job->pending);
	while ((ent = hash_seq_search(&st)) != NULL)
	{
		if (ent->req && ts_http_request_succeeded(ent->req) &&
			ent->req->result_body != NULL && !ent->req->harvested)
			reqs = list_append_unique_ptr(reqs, ent->req);
	}
	foreach(lc, reqs)
		jev_harvest_request(job, (TsHttpRequest *) lfirst(lc));
	list_free(reqs);
}

static void
jev_harvest_request(JevJob *job, TsHttpRequest *req)
{
	Jsonb	   *resp;
	JsonbValue	ubuf;
	JsonbValue *usage;
	JsonbContainer *ucont;
	int64		in_tok = 0;
	int64		out_tok = 0;

	if (req == NULL || req->state != TS_HTTP_SUCCEEDED || req->result_body == NULL)
		return;
	if (req->harvested)
		return;

	if (req->result_body[0] == '\0')
	{
		jev_stat_errors++;
		req->state = TS_HTTP_FAILED;
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_JSON_TEXT),
				errmsg("jev: TypeSafe API response is not JSON")));
	}

	resp = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(req->result_body)));

	if (!jev_validate_and_publish(job, req, resp))
	{
		jev_stat_errors++;
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				errmsg("jev: TypeSafe API response is missing answers")));
	}

	usage = getKeyJsonValueFromContainer(&resp->root, "usage", 5, &ubuf);
	ucont = jsonb_value_object_container(usage);
	if (ucont)
	{
		JsonbValue	ibuf;
		JsonbValue *iv;

		iv = getKeyJsonValueFromContainer(ucont, "input_tokens", 12, &ibuf);
		if (iv && iv->type == jbvNumeric)
		{
			int32		v = jsonb_numeric_int4(iv);

			if (v >= 0)
				in_tok = v;
		}
		iv = getKeyJsonValueFromContainer(ucont, "output_tokens", 13, &ibuf);
		if (iv && iv->type == jbvNumeric)
		{
			int32		v = jsonb_numeric_int4(iv);

			if (v >= 0)
				out_tok = v;
		}
	}
	jev_stat_requests++;
	jev_stat_input_tokens += in_tok;
	jev_stat_output_tokens += out_tok;
	jev_stat_api_ms += (double) req->elapsed_ms;
	jev_stat_retries += req->retries_done;
	job->done_reqs++;
	job->tokens += in_tok;
	{
		HASH_SEQ_STATUS st;
		JevPendingEnt *ent;
		int			nrows = 0;

		hash_seq_init(&st, job->pending);
		while ((ent = hash_seq_search(&st)) != NULL)
		{
			if (ent->req == req)
				nrows++;
		}
		jev_stat_rows_evaluated += nrows;
		job->done_rows += nrows;
	}
	req->harvested = true;
	jev_release_http_request(job, req);
	jev_sync_conn_acct();
}

static void
jev_notice_sweep(JevJob *job)
{
	if (!job->cfg.notices || job->rel_qname == NULL)
		return;
	if (job->done_reqs > job->reported_reqs)
	{
		int64		total_reqs = job->done_reqs;

		job->reported_reqs = job->done_reqs;
		if (job->est_rows > 0)
			total_reqs = Max(job->done_reqs, (job->est_rows + job->cfg.batch_size - 1) / job->cfg.batch_size);
		ereport(NOTICE,
				(errmsg("jev: progress %lld/%lld requests, %lld/%lld rows",
						(long long) job->done_reqs, (long long) total_reqs,
						(long long) job->done_rows,
						(long long) Max(job->done_rows, job->est_rows))));
	}
}

static void
jev_wait_row(JevJob *job, JevRowNode *want)
{
	uint8		digest[JEV_SHA256_LEN];

	memcpy(digest, want->digest, JEV_SHA256_LEN);
	for (;;)
	{
		JevCacheEnt *ent;
		JevPendingEnt *pent;
		JevPendingKey key;

		CHECK_FOR_INTERRUPTS();
		jev_harvest_job(job);
		jev_drop_cancelled_pending(job);
		if (jev_cache_lookup(job->ident_digest, digest, want->json, &ent))
			return;
		memcpy(key.digest, digest, JEV_SHA256_LEN);
		pent = hash_search(job->pending, &key, HASH_FIND, NULL);
		if (pent && pent->req)
		{
			if (ts_http_request_succeeded(pent->req))
			{
				if (!pent->req->harvested)
					jev_harvest_request(job, pent->req);
				if (jev_cache_lookup(job->ident_digest, digest, want->json, &ent))
					return;
				if (want && !want->on_queue)
					jev_release_row(want);
				hash_search(job->pending, &key, HASH_REMOVE, NULL);
				return;
			}
			if (pent->req->state == TS_HTTP_CANCELLED)
			{
				jev_release_http_request(job, pent->req);
				return;
			}
			if (pent->req->state == TS_HTTP_FAILED)
			{
				if (pent->req->http_status != 200)
					jev_stat_errors++;
				ts_http_await(jev_http, pent->req);
			}
			ts_http_pump(jev_http, TYPESAFE_WAIT_QUANTUM_MS);
			continue;
		}
		return;
	}
}

static JevJob *
jev_job_get(const uint8 *ident, Size ident_bytes, const char *canon,
			JevKind kind, const char *query, char **opts, int nopts,
			Oid typid, int32 typmod, const JevCfg *cfg)
{
	JevJobHashEnt *hent;
	bool		found;
	JevJob	   *job;
	MemoryContext old;
	HASHCTL		ctl;
	int			i;
	Size		jobsz;

	hent = hash_search(jev_job_hash, ident, HASH_ENTER, &found);
	if (found && hent->job && !hent->job->destroyed)
	{
		job = hent->job;
		job->cfg = *cfg;
		job->cfg.api_key = MemoryContextStrdup(job->ctx, cfg->api_key);
		job->cfg.api_url = MemoryContextStrdup(job->ctx, cfg->api_url);
		job->cfg.model = MemoryContextStrdup(job->ctx, cfg->model);
		return job;
	}

	jobsz = sizeof(JevJob) + ident_bytes + strlen(query) + 1;
	jev_reserve(jobsz);
	old = MemoryContextSwitchTo(JevSessionContext);
	job = (JevJob *) palloc0(sizeof(JevJob));
	job->ctx = AllocSetContextCreate(JevSessionContext, "JevJob",
									ALLOCSET_SMALL_SIZES);
	MemoryContextSwitchTo(job->ctx);
	memcpy(job->ident_digest, ident, JEV_SHA256_LEN);
	job->ident_canon = pstrdup(canon);
	job->ident_bytes = ident_bytes;
	job->kind = kind;
	job->query = pstrdup(query);
	job->noptions = nopts;
	if (nopts)
	{
		job->options = (char **) palloc(sizeof(char *) * nopts);
		for (i = 0; i < nopts; i++)
			job->options[i] = pstrdup(opts[i]);
	}
	job->typid = typid;
	job->typmod = typmod;
	job->cfg = *cfg;
	job->cfg.api_key = pstrdup(cfg->api_key);
	job->cfg.api_url = pstrdup(cfg->api_url);
	job->cfg.model = pstrdup(cfg->model);
	dlist_init(&job->fetched);
	dlist_init(&job->skipped);
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(JevPendingKey);
	ctl.entrysize = sizeof(JevPendingEnt);
	ctl.hcxt = job->ctx;
	job->skipped_map = hash_create("JevSkipped", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	job->pending = hash_create("JevPending", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	job->curl_generation = jev_curl_generation;
	job->restart_ts = cfg->stmt_ts;
	INSTR_TIME_SET_CURRENT(job->t0);
	if (jev_call_context && jev_call_context->scope)
	{
		job->qd = jev_call_context->qd;
		job->generation = jev_call_context->scope->generation;
		job->nest = jev_call_context->scope->nest;
	}
	else
		job->generation = PG_UINT64_MAX;
	job->owner_subid = GetCurrentSubTransactionId();
	job->call_generation = jev_call_generation;
	MemoryContextSwitchTo(old);
	jev_commit(jobsz, B_JOB);
	job->job_acct = jobsz;
	hent->job = job;
	jev_scan_prepare(job);
	return job;
}

static void
jev_maybe_rebuild_http(const JevCfg *cfg)
{
	bool		rebuild = false;

	if (cfg->keepalive_s == 0)
		rebuild = true;
	if (jev_last_url && strcmp(jev_last_url, cfg->api_url) != 0)
		rebuild = true;
	if (jev_have_last_cred && memcmp(jev_last_cred, cfg->cred_fp, JEV_SHA256_LEN) != 0)
		rebuild = true;
	if (jev_last_keepalive == 0 && cfg->keepalive_s != 0)
		rebuild = true;
	if (rebuild && jev_http)
	{
		jev_curl_generation++;
		ts_http_session_cancel_inflight(jev_http);
		jev_detach_cancelled_all();
		ts_http_session_rebuild_multi(jev_http);
		ts_http_finalize_cancelled(jev_http);
	}
	else if (jev_last_url && (strcmp(jev_last_url, cfg->api_url) != 0 ||
							(jev_have_last_cred && memcmp(jev_last_cred, cfg->cred_fp, JEV_SHA256_LEN) != 0)))
	{
		jev_curl_generation++;
		ts_http_cancel_owner(jev_http, jev_curl_generation - 1);
		jev_drop_cancelled_all();
	}
	if (JevSessionContext)
	{
		MemoryContext old = MemoryContextSwitchTo(JevSessionContext);

		if (jev_last_url)
			pfree(jev_last_url);
		jev_last_url = pstrdup(cfg->api_url);
		MemoryContextSwitchTo(old);
	}
	memcpy(jev_last_cred, cfg->cred_fp, JEV_SHA256_LEN);
	jev_have_last_cred = true;
	jev_last_keepalive = cfg->keepalive_s;
}

static void
jev_identity_check(void)
{
	Oid			uid = GetUserId();
	Oid			sid = GetSessionUserId();

	if (jev_userid != InvalidOid && (uid != jev_userid || sid != jev_sessid))
	{
		HASH_SEQ_STATUS st;
		JevJobHashEnt *ent;
		List	   *jobs = NIL;
		ListCell   *lc;

		jev_curl_generation++;
		if (jev_job_hash)
		{
			hash_seq_init(&st, jev_job_hash);
			while ((ent = hash_seq_search(&st)) != NULL)
				jobs = lappend(jobs, ent->job);
		}
		foreach(lc, jobs)
			jev_destroy_job((JevJob *) lfirst(lc), false);
		list_free(jobs);
		if (JevCacheContext)
			MemoryContextReset(JevCacheContext);
		jev_cache_hash = NULL;
		jev_ensure_hashes();
		jev_bucket[B_CACHE] = 0;
		jev_bucket[B_JOB] = 0;
		jev_bucket[B_FETCHED] = 0;
		jev_bucket[B_SKIPPED] = 0;
		jev_bucket[B_HTTP] = 0;
		jev_stat_requests = jev_stat_input_tokens = jev_stat_output_tokens = 0;
		jev_stat_rows_evaluated = jev_stat_cache_hits = 0;
		jev_stat_api_ms = 0;
		jev_stat_errors = jev_stat_retries = jev_stat_cancelled = 0;
		if (jev_http)
		{
			ts_http_session_reset_multi(jev_http);
			ts_http_finalize_all(jev_http);
		}
	}
	jev_userid = uid;
	jev_sessid = sid;
}

static void
jev_error_cleanup(void)
{
	JevExecScope *scope = NULL;
	HASH_SEQ_STATUS st;
	JevJobHashEnt *ent;

	jev_reserved_bytes = 0;
	if (jev_call_context)
		scope = jev_call_context->scope;
	if (jev_http && jev_job_hash)
	{
		hash_seq_init(&st, jev_job_hash);
		while ((ent = hash_seq_search(&st)) != NULL)
		{
			JevJob	   *job = ent->job;
			HASH_SEQ_STATUS st2;
			JevPendingEnt *pent;

			if (scope && job->generation != scope->generation)
				continue;
			jev_harvest_job(job);
			if (job->pending == NULL)
				continue;
			hash_seq_init(&st2, job->pending);
			while ((pent = hash_seq_search(&st2)) != NULL)
			{
				if (pent->req == NULL || ts_http_request_terminal(pent->req))
					continue;
				ts_http_cancel_request(jev_http, pent->req);
				jev_stat_cancelled++;
			}
			jev_drop_cancelled_pending(job);
		}
	}
	jev_sync_conn_acct();
}

static Jsonb *
jev_eval_core(FunctionCallInfo fcinfo, const char *query, JevKind kind, char **opts, int nopts)
{
	Oid			typid;
	int32		typmod;
	Datum		rec;
	char	   *row_json;
	uint8		row_digest[JEV_SHA256_LEN];
	uint8		ident[JEV_SHA256_LEN];
	char	   *canon;
	Size		canon_len;
	JevCfg		cfg;
	JevCacheEnt *cent;
	JevJob	   *job;
	JevExecScope *scope;
	QueryDesc  *qd;
	Oid			relid;
	Jsonb	   *result;

	jev_ensure_session();
	jev_identity_check();

	if (jev_call_context == NULL || jev_call_context->qd == NULL)
	{
		/*
		* Library loaded during this query: hooks did not wrap ExecutorRun.
		* Judge this row only; do not guess a QueryDesc for read-ahead.
		*/
		qd = NULL;
		scope = NULL;
	}
	else
	{
		qd = jev_call_context->qd;
		scope = jev_scope_lookup_exact(qd);
		if (scope == NULL)
		{
			/* Library loaded after ExecutorStart: judge this row only. */
			qd = NULL;
		}
		else if (!scope->active)
		{
			scope->generation++;
			scope->active = true;
		}
		jev_call_generation++;
	}
	(void) qd;

	typid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	typmod = jev_arg_typmod(fcinfo, 0);
	rec = PG_GETARG_DATUM(0);

	if (!type_is_rowtype(typid))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				errmsg("jev requires a composite argument")));

	relid = get_typ_typrelid(typid);
	if (!OidIsValid(relid))
		relid = get_typ_typrelid(getBaseType(typid));
	if (OidIsValid(relid))
	{
		char		rk = get_rel_relkind(relid);

		if (rk != RELKIND_RELATION && rk != RELKIND_MATVIEW &&
			rk != RELKIND_VIEW && rk != RELKIND_PARTITIONED_TABLE &&
			rk != RELKIND_FOREIGN_TABLE)
			jev_relkind_error(rk);
	}

	cfg = jev_load_cfg();
	jev_maybe_rebuild_http(&cfg);
	jev_x2_note_statement(cfg.stmt_ts);
	row_json = jev_datum_to_json(rec, typid, typmod);
	if (strlen(row_json) > TYPESAFE_MAX_RESPONSE_BYTES)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("jev row exceeds %d bytes", TYPESAFE_MAX_RESPONSE_BYTES)));
	jev_sha256_row(row_json, row_digest);
	jev_ident_digest(GetUserId(), typid, typmod, relid, query, kind, opts, nopts,
					&cfg, ident, &canon, &canon_len);

	if (jev_http)
		ts_http_pump(jev_http, 0);

	if (jev_cache_lookup(ident, row_digest, row_json, &cent))
	{
		jev_stat_cache_hits++;
		job = jev_job_lookup_ident(ident);
		if (job && jev_window() < job->cfg.concurrency)
			jev_top_up(job);
		return jev_answer_jsonb(cent->answer_json);
	}

	job = jev_job_get(ident, canon_len, canon, kind, query, opts, nopts,
					typid, typmod, &cfg);
	job->call_generation = jev_call_generation;

	{
		JevPendingKey key;
		JevPendingEnt *pent;
		JevRowNode *want = NULL;

		memcpy(key.digest, row_digest, JEV_SHA256_LEN);
		pent = hash_search(job->pending, &key, HASH_FIND, NULL);
		if (pent && pent->req)
		{
			jev_wait_row(job, pent->row);
		}
		else if (scope != NULL && job->relid != InvalidOid && job->mode != JEV_SCAN_NONE)
		{
			JevPendingEnt *sk;

			sk = hash_search(job->skipped_map, &key, HASH_FIND, NULL);
			if (sk && sk->row)
			{
				JevRowNode *batch[128];
				int			n = 0;

				hash_search(job->skipped_map, &key, HASH_REMOVE, NULL);
				dlist_delete(&sk->row->node);
				job->n_skipped--;
				sk->row->on_queue = false;
				batch[n++] = sk->row;
				while (n < job->cfg.batch_size && !dlist_is_empty(&job->skipped))
				{
					JevRowNode *r = dlist_tail_element(JevRowNode, node, &job->skipped);
					JevPendingKey k2;

					dlist_delete(&r->node);
					job->n_skipped--;
					r->on_queue = false;
					memcpy(k2.digest, r->digest, JEV_SHA256_LEN);
					hash_search(job->skipped_map, &k2, HASH_REMOVE, NULL);
					batch[n++] = r;
				}
				jev_submit_batch(job, batch, n, true);
				jev_x2_maybe_fault();
				jev_wait_row(job, batch[0]);
			}
			else
			{
				bool		found = jev_locate(job, row_digest);

				if (!found && job->exhausted && job->restart_ts != cfg.stmt_ts)
				{
					job->exhausted = false;
					job->have_tid = false;
					job->offset = 0;
					job->block = 0;
					job->restart_ts = cfg.stmt_ts;
					found = jev_locate(job, row_digest);
				}
				if (found)
				{
					jev_top_up(job);
					memcpy(key.digest, row_digest, JEV_SHA256_LEN);
					pent = hash_search(job->pending, &key, HASH_FIND, NULL);
					if (pent && pent->row)
						jev_wait_row(job, pent->row);
				}
			}
		}

		if (!jev_cache_lookup(ident, row_digest, row_json, &cent))
		{
			want = jev_row_from_json(job, row_json, B_FETCHED);
			jev_submit_batch(job, &want, 1, true);
			jev_x2_maybe_fault();
			jev_wait_row(job, want);
		}
	}

	jev_notice_sweep(job);
	if (jev_http)
		ts_http_pump(jev_http, 0);

	if (!jev_cache_lookup(ident, row_digest, row_json, &cent))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("jev: the API returned no answer for a row")));
	result = jev_answer_jsonb(cent->answer_json);
	return result;
}

static Jsonb *
jev_call_eval(FunctionCallInfo fcinfo, JevKind kind, int opts_arg, bool opts_present)
{
	char	   *query;
	char	  **opts = NULL;
	int			nopts = 0;
	Jsonb	   *ans;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		return NULL;
	query = text_to_cstring(PG_GETARG_TEXT_PP(1));
	if (opts_present && opts_arg >= 0 && opts_arg < PG_NARGS() && !PG_ARGISNULL(opts_arg))
		jev_parse_text_array(PG_GETARG_ARRAYTYPE_P(opts_arg), kind, &opts, &nopts);
	else if (kind != JEV_KIND_NOUL)
		jev_parse_text_array(NULL, kind, &opts, &nopts);

	PG_TRY();
	{
		ans = jev_eval_core(fcinfo, query, kind, opts, nopts);
	}
	PG_CATCH();
	{
		jev_error_cleanup();
		PG_RE_THROW();
	}
	PG_END_TRY();
	return ans;
}

static float8
jev_extract_noul(Jsonb *ans)
{
	JsonbValue	vbuf;
	JsonbValue *v;

	v = getKeyJsonValueFromContainer(&ans->root, "noul", 4, &vbuf);
	if (v == NULL || v->type != jbvNumeric)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_JSON_TEXT),
				errmsg("jev: missing numeric field \"noul\"")));
	return jsonb_numeric_float8(v);
}

Datum
jev_eval(PG_FUNCTION_ARGS)
{
	char	   *kindstr;
	JevKind		kind;
	Jsonb	   *ans;
	bool		has_opts;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();
	kindstr = PG_ARGISNULL(2) ? "noul" : text_to_cstring(PG_GETARG_TEXT_PP(2));
	if (strcmp(kindstr, "noul") == 0)
		kind = JEV_KIND_NOUL;
	else if (strcmp(kindstr, "choice") == 0)
		kind = JEV_KIND_CHOICE;
	else if (strcmp(kindstr, "score") == 0)
		kind = JEV_KIND_SCORE;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("jev: unknown kind %s", kindstr)));
	has_opts = (PG_NARGS() > 3);
	ans = jev_call_eval(fcinfo, kind, 3, has_opts);
	if (ans == NULL)
		PG_RETURN_NULL();
	PG_RETURN_JSONB_P(ans);
}

Datum
jev_prob(PG_FUNCTION_ARGS)
{
	Jsonb	   *ans = jev_call_eval(fcinfo, JEV_KIND_NOUL, -1, false);

	if (ans == NULL)
		PG_RETURN_NULL();
	PG_RETURN_FLOAT8(jev_extract_noul(ans));
}

Datum
jev(PG_FUNCTION_ARGS)
{
	Jsonb	   *ans;
	float8		p;
	float8		thr;

	ans = jev_call_eval(fcinfo, JEV_KIND_NOUL, -1, false);
	if (ans == NULL)
		PG_RETURN_NULL();
	p = jev_extract_noul(ans);
	if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
		thr = PG_GETARG_FLOAT8(2);
	else
		thr = jev_guc_threshold;
	PG_RETURN_BOOL(p >= thr);
}

Datum
jev_choice(PG_FUNCTION_ARGS)
{
	Jsonb	   *ans = jev_call_eval(fcinfo, JEV_KIND_CHOICE, 2, true);
	JsonbValue	vbuf;
	JsonbValue *v;

	if (ans == NULL)
		PG_RETURN_NULL();
	v = getKeyJsonValueFromContainer(&ans->root, "choice", 6, &vbuf);
	if (v == NULL || v->type != jbvString)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_JSON_TEXT),
				errmsg("jev: missing string field \"choice\"")));
	PG_RETURN_TEXT_P(cstring_to_text(jsonb_str_cstring(v)));
}

Datum
jev_score(PG_FUNCTION_ARGS)
{
	Jsonb	   *ans = jev_call_eval(fcinfo, JEV_KIND_SCORE, 2, true);
	JsonbValue	vbuf;
	JsonbValue *v;

	if (ans == NULL)
		PG_RETURN_NULL();
	v = getKeyJsonValueFromContainer(&ans->root, "score", 5, &vbuf);
	if (v == NULL || v->type != jbvNumeric)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_JSON_TEXT),
				errmsg("jev: missing numeric field \"score\"")));
	PG_RETURN_FLOAT8(jsonb_numeric_float8(v));
}

Datum
jev_score_norm(PG_FUNCTION_ARGS)
{
	Jsonb	   *ans;
	JsonbValue	vbuf;
	JsonbValue *v;
	float8		score;
	char	  **opts = NULL;
	int			nopts = 0;
	int			den;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();
	if (!PG_ARGISNULL(2))
		jev_parse_text_array(PG_GETARG_ARRAYTYPE_P(2), JEV_KIND_SCORE, &opts, &nopts);
	else
		jev_parse_text_array(NULL, JEV_KIND_SCORE, &opts, &nopts);
	ans = jev_call_eval(fcinfo, JEV_KIND_SCORE, 2, true);
	if (ans == NULL)
		PG_RETURN_NULL();
	v = getKeyJsonValueFromContainer(&ans->root, "score", 5, &vbuf);
	score = jsonb_numeric_float8(v);
	den = nopts > 1 ? nopts - 1 : 1;
	PG_RETURN_FLOAT8(score / (float8) den);
}

Datum
jev_confidence(PG_FUNCTION_ARGS)
{
	char	   *kindstr;
	JevKind		kind;
	Jsonb	   *ans;
	JsonbValue	vbuf;
	JsonbValue *v;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		PG_RETURN_NULL();
	kindstr = text_to_cstring(PG_GETARG_TEXT_PP(2));
	if (strcmp(kindstr, "noul") == 0)
		kind = JEV_KIND_NOUL;
	else if (strcmp(kindstr, "choice") == 0)
		kind = JEV_KIND_CHOICE;
	else if (strcmp(kindstr, "score") == 0)
		kind = JEV_KIND_SCORE;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("jev: unknown kind %s", kindstr)));
	ans = jev_call_eval(fcinfo, kind, 3, true);
	if (ans == NULL)
		PG_RETURN_NULL();
	if (kind == JEV_KIND_NOUL)
		PG_RETURN_NULL();
	v = getKeyJsonValueFromContainer(&ans->root, "confidence", 10, &vbuf);
	if (v == NULL || v->type != jbvNumeric)
		PG_RETURN_NULL();
	PG_RETURN_FLOAT8(jsonb_numeric_float8(v));
}

Datum
jev_stats(PG_FUNCTION_ARGS)
{
	StringInfoData buf;
	double		cost;
	int64		cached = 0;

	jev_ensure_session();
	if (jev_http)
		ts_http_pump(jev_http, 0);
	if (jev_cache_hash)
		cached = hash_get_num_entries(jev_cache_hash);
	cost = round(jev_stat_input_tokens * 0.042 / 1000000.0 * 1e6) / 1e6;
	initStringInfo(&buf);
	appendStringInfo(&buf,
					"{\"requests\": %lld, \"input_tokens\": %lld, \"output_tokens\": %lld, "
					"\"rows_evaluated\": %lld, \"cache_hits\": %lld, \"api_ms\": %.3f, "
					"\"errors\": %lld, \"retries\": %lld, \"batches\": 0, "
					"\"estimated_cost_usd\": %.6f, \"cached_answers\": %lld, "
					"\"in_flight\": %d, \"connections\": %d, \"session_bytes\": %zu, "
					"\"cancelled_requests\": %lld, \"last_cancel_stage\": ",
					(long long) jev_stat_requests, (long long) jev_stat_input_tokens,
					(long long) jev_stat_output_tokens, (long long) jev_stat_rows_evaluated,
					(long long) jev_stat_cache_hits, jev_stat_api_ms,
					(long long) jev_stat_errors, (long long) jev_stat_retries,
					cost, (long long) cached,
					jev_window(), ts_http_idle_connections(jev_http),
					jev_accounted(), (long long) jev_stat_cancelled);
	escape_json(&buf, jev_last_cancel_stage);
	appendStringInfo(&buf,
					", \"_b_cache\": %zu, \"_b_job\": %zu, \"_b_fetched\": %zu, "
					"\"_b_http\": %zu, \"_b_skipped\": %zu, \"_b_conn\": %zu, "
					"\"_reserved\": %zu, \"_http_easies\": %d}",
					jev_bucket[B_CACHE], jev_bucket[B_JOB], jev_bucket[B_FETCHED],
					jev_bucket[B_HTTP], jev_bucket[B_SKIPPED], jev_bucket[B_CONN],
					jev_reserved_bytes, ts_http_live_easies(jev_http));
	PG_RETURN_JSONB_P(DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(buf.data))));
}

Datum
jev_cache_clear(PG_FUNCTION_ARGS)
{
	HASH_SEQ_STATUS st;
	JevJobHashEnt *ent;
	List	   *jobs = NIL;
	ListCell   *lc;

	jev_ensure_session();
	if (jev_job_hash)
	{
		hash_seq_init(&st, jev_job_hash);
		while ((ent = hash_seq_search(&st)) != NULL)
			jobs = lappend(jobs, ent->job);
	}
	foreach(lc, jobs)
		jev_destroy_job((JevJob *) lfirst(lc), true);
	list_free(jobs);
	if (jev_http)
		ts_http_finalize_all(jev_http);
	if (JevCacheContext)
		MemoryContextReset(JevCacheContext);
	jev_cache_hash = NULL;
	jev_ensure_hashes();
	jev_bucket[B_CACHE] = 0;
	jev_bucket[B_JOB] = 0;
	jev_bucket[B_FETCHED] = 0;
	jev_bucket[B_SKIPPED] = 0;
	jev_bucket[B_HTTP] = 0;
	jev_reserved_bytes = 0;
	jev_sync_conn_acct();
	PG_RETURN_VOID();
}

Datum
jev_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(JEV_COMPAT_VERSION));
}

