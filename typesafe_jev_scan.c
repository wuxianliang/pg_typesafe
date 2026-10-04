/*-------------------------------------------------------------------------
 *
 * typesafe_jev_scan.c
 *		Invoker-rights SPI read-ahead for native Jev.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/itemptr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"

#include "typesafe_internal.h"
#include "typesafe_http.h"
#include "typesafe_jev.h"

void
jev_relkind_error(char relkind)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("jev: cannot scan relation of relkind '%c'", relkind)));
}

void
jev_scan_prepare(JevJob *job)
{
	Oid			relid;
	char		relkind;
	char	   *nsp;
	char	   *rel;
	MemoryContext old;

	relid = get_typ_typrelid(job->typid);
	job->relid = InvalidOid;
	job->mode = JEV_SCAN_NONE;
	job->exhausted = true;

	if (!OidIsValid(relid))
		return;

	relkind = get_rel_relkind(relid);
	if (relkind == RELKIND_RELATION || relkind == RELKIND_MATVIEW)
		job->mode = JEV_SCAN_CTID;
	else if (relkind == RELKIND_VIEW || relkind == RELKIND_PARTITIONED_TABLE ||
			 relkind == RELKIND_FOREIGN_TABLE)
		job->mode = JEV_SCAN_OFFSET;
	else
		jev_relkind_error(relkind);

	job->relid = relid;
	nsp = get_namespace_name(get_rel_namespace(relid));
	rel = get_rel_name(relid);
	old = MemoryContextSwitchTo(job->ctx);
	job->rel_qname = quote_qualified_identifier(nsp, rel);
	MemoryContextSwitchTo(old);
	job->exhausted = false;
	job->have_tid = false;
	job->offset = 0;
	job->block = 0;
	ItemPointerSet(&job->next_tid, 0, 0);
}

static void
jev_scan_admit(JevJob *job, int *max_rows)
{
	Size		cap = jev_cap_bytes();
	Size		acc = jev_accounted() + jev_reserved_bytes;
	Size		last = (Size) job->rows_per_block;
	Size		est;
	int			tries;

	if (last < 256)
		last = 256;
	if (cap > acc)
	{
		Size		room = cap - acc;
		int			byroom = (int) (room / (last + 128));

		if (byroom < 1)
			byroom = 1;
		*max_rows = Min(*max_rows, byroom);
	}

	est = last * (Size) (*max_rows);
	for (tries = 0; tries < 64; tries++)
	{
		jev_harvest_job(job);
		acc = jev_accounted() + jev_reserved_bytes;
		if (acc + est <= cap && ts_http_active_count(jev_http) < job->cfg.concurrency)
			return;
		if (jev_http == NULL || ts_http_window_count(jev_http) == 0)
			break;
		ts_http_pump(jev_http, TYPESAFE_WAIT_QUANTUM_MS);
		acc = jev_accounted() + jev_reserved_bytes;
	}
	if (jev_accounted() + jev_reserved_bytes + est > cap)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("jev session memory limit exceeded"),
				 errhint("Call jev_cache_clear() or raise jev.max_session_memory_kb.")));
}

static void
jev_append_fetched(JevJob *job, const char *json, ItemPointer tid)
{
	JevRowNode *row;

	row = (JevRowNode *) MemoryContextAllocZero(job->ctx, sizeof(JevRowNode));
	{
		char	   *canon = jev_canon_row_json(json);
		Size		n = strlen(canon) + 1;

		jev_reserve(n);
		row->json = MemoryContextStrdup(job->ctx, canon);
		row->nbytes = n;
		row->bucket = B_FETCHED;
		jev_sha256_row(row->json, row->digest);
		jev_commit(n, B_FETCHED);
		job->rows_per_block = (double) n;
	}
	(void) tid;
	dlist_push_tail(&job->fetched, &row->node);
	row->on_queue = true;
	job->n_fetched++;
}

int
jev_scan_fetch_page(JevJob *job, int max_rows)
{
	int			n = 0;
	bool		connected = false;
	Oid			argtypes[2];
	Datum		values[2];
	char		nulls[2];
	char	   *sql;
	int			rc;
	int			i;
	MemoryContext oldcontext;
	ResourceOwner oldowner;

	if (job->exhausted || job->mode == JEV_SCAN_NONE || job->rel_qname == NULL)
	{
		job->exhausted = true;
		return 0;
	}

	jev_x2_maybe_fault();

	if (max_rows <= 0)
		max_rows = JEV_SPI_PAGE_ROWS;
	if (max_rows > JEV_SPI_PAGE_ROWS)
		max_rows = JEV_SPI_PAGE_ROWS;

	jev_scan_admit(job, &max_rows);

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);
	jev_spi_depth++;
	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		connected = true;

		memset(nulls, ' ', sizeof(nulls));
		if (job->mode == JEV_SCAN_CTID)
		{
			argtypes[0] = TIDOID;
			argtypes[1] = INT4OID;
			if (!job->have_tid)
			{
				ItemPointerSet(&job->next_tid, 0, 0);
				sql = psprintf("SELECT ctid, to_json(t)::text FROM %s t WHERE ctid >= $1 ORDER BY ctid LIMIT $2",
							  job->rel_qname);
			}
			else
				sql = psprintf("SELECT ctid, to_json(t)::text FROM %s t WHERE ctid > $1 ORDER BY ctid LIMIT $2",
							  job->rel_qname);
			values[0] = PointerGetDatum(&job->next_tid);
			values[1] = Int32GetDatum(max_rows);
			rc = SPI_execute_with_args(sql, 2, argtypes, values, nulls, true, max_rows);
			pfree(sql);
			if (rc != SPI_OK_SELECT)
				elog(ERROR, "jev SPI scan failed");
			for (i = 0; i < (int) SPI_processed; i++)
			{
				bool		isnull;
				Datum		tid_d;
				char	   *json;
				ItemPointer tid;

				tid_d = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);
				json = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2);
				if (json == NULL)
					continue;
				tid = (ItemPointer) DatumGetPointer(tid_d);
				jev_append_fetched(job, json, tid);
				if (!isnull && tid != NULL)
				{
					job->next_tid = *tid;
					job->have_tid = true;
				}
				n++;
			}
			if ((int) SPI_processed < max_rows)
				job->exhausted = true;
		}
		else
		{
			argtypes[0] = INT8OID;
			argtypes[1] = INT8OID;
			sql = psprintf("SELECT to_json(t)::text FROM %s t OFFSET $1 LIMIT $2",
						  job->rel_qname);
			values[0] = Int64GetDatum(job->offset);
			values[1] = Int64GetDatum(max_rows);
			rc = SPI_execute_with_args(sql, 2, argtypes, values, nulls, true, max_rows);
			pfree(sql);
			if (rc != SPI_OK_SELECT)
				elog(ERROR, "jev SPI scan failed");
			for (i = 0; i < (int) SPI_processed; i++)
			{
				char	   *json = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);

				if (json == NULL)
					continue;
				jev_append_fetched(job, json, NULL);
				n++;
			}
			job->offset += SPI_processed;
			if ((int) SPI_processed < max_rows)
				job->exhausted = true;
		}

		SPI_finish();
		connected = false;
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		jev_spi_depth--;
	}
	PG_CATCH();
	{
		if (connected)
			(void) SPI_finish();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		jev_spi_depth--;
		PG_RE_THROW();
	}
	PG_END_TRY();

	return n;
}
