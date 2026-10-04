/*-------------------------------------------------------------------------
 *
 * typesafe_jev.h
 *		Native Jev engine internals shared with typesafe_jev_scan.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef TYPESAFE_JEV_H
#define TYPESAFE_JEV_H

#include "postgres.h"

#include "access/htup.h"
#include "access/xact.h"
#include "common/sha2.h"
#include "executor/execdesc.h"
#include "lib/ilist.h"
#include "nodes/pg_list.h"
#include "storage/itemptr.h"
#include "utils/hsearch.h"
#include "utils/portal.h"
#include "utils/typcache.h"

#include "typesafe_http.h"

#define JEV_SPI_PAGE_ROWS		128
#define JEV_COMPAT_VERSION		"0.2.0"
#define JEV_USER_AGENT			"User-Agent: postgres-typesafe-jev/0.2.0"
#define JEV_MAX_ATTEMPTS		7
#define JEV_SHA256_LEN			PG_SHA256_DIGEST_LENGTH

typedef enum JevKind
{
	JEV_KIND_NOUL = 0,
	JEV_KIND_CHOICE,
	JEV_KIND_SCORE
} JevKind;

typedef enum JevScanMode
{
	JEV_SCAN_NONE = 0,
	JEV_SCAN_CTID,
	JEV_SCAN_OFFSET
} JevScanMode;

typedef enum JevBucket
{
	B_CACHE = 0,
	B_JOB,
	B_FETCHED,
	B_HTTP,
	B_SKIPPED,
	B_CONN,
	JEV_NBUCKETS
} JevBucket;

typedef struct JevExecScope
{
	QueryDesc  *qd;
	uint64		generation;
	Portal		portal;
	void		(*prev_portal_cleanup) (Portal portal);
	int			nest;
	bool		active;
} JevExecScope;

typedef struct JevCallContext
{
	QueryDesc  *qd;
	JevExecScope *scope;
	struct JevCallContext *prev;
} JevCallContext;

typedef struct JevCfg
{
	char	   *api_key;
	char	   *api_url;
	char	   *model;
	int			batch_size;
	int			concurrency;
	int			max_prefetch;
	bool		notices;
	double		timeout_s;
	int			keepalive_s;
	int			max_rows;
	int			max_chars;
	int			max_session_memory_kb;
	double		threshold;
	TimestampTz stmt_ts;
	uint8		cred_fp[JEV_SHA256_LEN];
} JevCfg;

typedef struct JevRowNode
{
	dlist_node	node;
	uint8		digest[JEV_SHA256_LEN];
	char	   *json;
	Size		nbytes;
	JevBucket	bucket;
	TsHttpRequest *req;
	int			qid;
	bool		on_queue;
} JevRowNode;

typedef struct JevPendingKey
{
	uint8		digest[JEV_SHA256_LEN];
} JevPendingKey;

typedef struct JevPendingEnt
{
	JevPendingKey key;
	JevRowNode *row;
	TsHttpRequest *req;
} JevPendingEnt;

typedef struct JevJob
{
	MemoryContext ctx;
	uint8		ident_digest[JEV_SHA256_LEN];
	char	   *ident_canon;
	Size		ident_bytes;
	JevKind		kind;
	char	   *query;
	char	  **options;
	int			noptions;
	Oid			typid;
	int32		typmod;
	Oid			relid;
	char	   *rel_qname;
	JevScanMode mode;
	ItemPointerData next_tid;
	bool		have_tid;
	int64		offset;
	bool		exhausted;
	int64		est_rows;
	int64		nblocks;
	double		rows_per_block;
	int64		block;
	dlist_head	fetched;
	dlist_head	skipped;
	int			n_fetched;
	int			n_skipped;
	HTAB	   *skipped_map;
	HTAB	   *pending;
	List	   *batches;
	QueryDesc  *qd;
	uint64		generation;
	uint64		call_generation;
	uint64		curl_generation;
	int			nest;
	SubTransactionId owner_subid;
	JevCfg		cfg;
	TimestampTz restart_ts;
	int64		done_reqs;
	int64		done_rows;
	int64		tokens;
	int64		reported_reqs;
	bool		summary_done;
	instr_time	t0;
	Size		job_acct;
	bool		destroyed;
} JevJob;

typedef struct JevCacheKey
{
	uint8		ident[JEV_SHA256_LEN];
	uint8		row[JEV_SHA256_LEN];
} JevCacheKey;

typedef struct JevCacheEnt
{
	JevCacheKey key;
	char	   *row_json;
	Size		row_nbytes;
	char	   *answer_json;
	Size		answer_nbytes;
} JevCacheEnt;

extern int	jev_spi_depth;
extern uint64 jev_call_generation;
extern uint64 jev_curl_generation;
extern JevCallContext *jev_call_context;

extern Size jev_bucket[JEV_NBUCKETS];
extern Size jev_reserved_bytes;

extern MemoryContext JevSessionContext;
extern MemoryContext JevCacheContext;
extern MemoryContext JevCurlContext;
extern TsHttpSession *jev_http;

extern void jev_reserve(Size n);
extern void jev_commit(Size n, JevBucket bucket);
extern void jev_release_reserve(Size n);
extern void jev_free_committed(Size n, JevBucket bucket);
extern Size jev_accounted(void);
extern Size jev_cap_bytes(void);
extern char *jev_ctx_strdup(MemoryContext ctx, const char *s, JevBucket bucket);
extern void jev_sha256(const void *data, Size len, uint8 *out);
extern void jev_sha256_row(const char *json, uint8 *out);
extern char *jev_canon_row_json(const char *json);

extern JevExecScope *jev_scope_lookup_exact(QueryDesc *qd);
extern JevJob *jev_job_lookup_ident(const uint8 *ident);
extern void jev_x2_maybe_fault(void);
extern void jev_x2_on_http_complete(void);
extern void jev_harvest_job(JevJob *job);

extern int jev_scan_fetch_page(JevJob *job, int max_rows);
extern void jev_scan_prepare(JevJob *job);
extern void jev_relkind_error(char relkind);

#endif							/* TYPESAFE_JEV_H */
