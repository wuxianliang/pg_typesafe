/*-------------------------------------------------------------------------
 *
 * typesafe_http.h
 *		Shared libcurl multi transport for typesafe_* and jev_*.
 *
 *-------------------------------------------------------------------------
 */
#ifndef TYPESAFE_HTTP_H
#define TYPESAFE_HTTP_H

#include "postgres.h"

#include <curl/curl.h>

#include "nodes/pg_list.h"
#include "portability/instr_time.h"
#include "utils/timestamp.h"

#if LIBCURL_VERSION_NUM < 0x074200
#error "pg_typesafe requires libcurl 7.66 or later"
#endif

typedef enum TsHttpFamily
{
	TS_FAMILY_LEGACY = 0,
	TS_FAMILY_JEV = 1
} TsHttpFamily;

typedef enum TsHttpRequestState
{
	TS_HTTP_QUEUED = 0,
	TS_HTTP_ACTIVE,
	TS_HTTP_BACKOFF,
	TS_HTTP_SUCCEEDED,
	TS_HTTP_FAILED,
	TS_HTTP_CANCELLED
} TsHttpRequestState;

typedef enum TsConnState
{
	TS_CONN_OPENING = 0,
	TS_CONN_ACTIVE,
	TS_CONN_IDLE,
	TS_CONN_STALE,
	TS_CONN_CLOSED
} TsConnState;

typedef struct TsHttpPolicy
{
	const char *endpoint;
	const char *credential;
	const char *guc_name;
	const char *user_agent;
	long		timeout_ms;
	int			max_attempts;
	bool		retry_5xx;
	bool		http11;
	int			keepalive_s;
	int			concurrency;
	size_t		max_response;
	TsHttpFamily family;
	bool		require_asynch_dns;
	uint64		owner_generation;
} TsHttpPolicy;

typedef struct TsConn TsConn;
typedef struct TsHttpRequest TsHttpRequest;
typedef struct TsHttpSession TsHttpSession;

struct TsConn
{
	curl_socket_t fd;
	TsConnState state;
	instr_time	last_use;
	uint32		endpoint_hash;
	uint32		cred_hash;
	bool		slot_used;
};

struct TsHttpRequest
{
	TsHttpRequestState state;
	TsHttpPolicy policy;
	char	   *payload;
	size_t		payload_len;
	char	   *resp_data;
	size_t		resp_len;
	size_t		resp_cap;
	bool		resp_overflow;
	bool		resp_oom;
	CURL	   *easy;
	struct curl_slist *headers;
	int			attempts;
	int64		retry_at_us;
	bool		has_retry_at;
	long		http_status;
	CURLcode	curl_rc;
	char		header_retry_after[128];
	char		header_retry_after_ms[32];
	bool		reused_conn;
	bool		loopback_rejected;
	char	   *result_body;
	long		elapsed_ms;
	int			retries_done;
	uint64		owner_generation;
	TsHttpFamily family;
	Size		http_acct;
	void	   *owner;
	TsHttpSession *session;
	bool		harvested;
	bool		on_multi;
};

#define TS_HTTP_MAX_CONNS 128
#define TS_HTTP_MAX_IDLE_EASIES 64

struct TsHttpSession
{
	MemoryContext ctx;
	CURLM	   *multi;
	TsConn		conns[TS_HTTP_MAX_CONNS];
	List	   *requests;
	List	   *idle_easies;
	int			in_flight;
	bool		asynch_dns_ok;
	bool		asynch_dns_checked;
	uint32		last_endpoint_hash;
	uint32		last_cred_hash;
	bool		have_identity;
	bool		pause_admit;
};

extern void ts_http_check_endpoint(const char *endpoint, const char *guc_name);
extern void ts_http_reject_header_injection(const char *credential);
extern uint32 ts_http_string_hash(const char *s);

extern TsHttpSession *ts_http_session_create(MemoryContext parent);
extern void ts_http_session_cancel_inflight(TsHttpSession *session);
extern void ts_http_session_rebuild_multi(TsHttpSession *session);
extern void ts_http_session_reset_multi(TsHttpSession *session);
extern void ts_http_session_destroy(TsHttpSession *session);
extern void ts_http_finalize_request(TsHttpSession *session, TsHttpRequest *request);
extern void ts_http_recycle_request(TsHttpSession *session, TsHttpRequest *request);
extern void ts_http_finalize_all(TsHttpSession *session);
extern void ts_http_finalize_cancelled(TsHttpSession *session);

extern TsHttpRequest *ts_http_submit(TsHttpSession *session,
									 const TsHttpPolicy *policy,
									 const char *payload,
									 uint64 owner_generation);
extern void ts_http_pump(TsHttpSession *session, long maximum_wait_ms);
extern void ts_http_await(TsHttpSession *session, TsHttpRequest *request);
extern void ts_http_cancel_request(TsHttpSession *session, TsHttpRequest *request);
extern void ts_http_cancel_owner(TsHttpSession *session, uint64 generation);
extern void ts_http_cancel_all(TsHttpSession *session);

extern int ts_http_idle_connections(TsHttpSession *session);
extern int ts_http_window_count(TsHttpSession *session);
extern int ts_http_active_count(TsHttpSession *session);
extern int ts_http_live_easies(TsHttpSession *session);
extern Size ts_http_conn_bytes(TsHttpSession *session);

extern bool ts_http_request_terminal(TsHttpRequest *request);
extern bool ts_http_request_succeeded(TsHttpRequest *request);

extern void (*ts_http_success_hook)(void);

#endif							/* TYPESAFE_HTTP_H */
