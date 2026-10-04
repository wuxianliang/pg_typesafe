/*-------------------------------------------------------------------------
 *
 * typesafe_http.c
 *		Shared curl_multi transport.  libcurl callbacks never ereport.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "typesafe_http.h"
#include "typesafe_internal.h"

#include "common/hashfn.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "port.h"
#include "storage/latch.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

static bool ts_host_is_loopback_name(const char *host);
static bool ts_addr_is_loopback(const struct sockaddr *sa, socklen_t len);
static long ts_legacy_retry_delay_ms(TsHttpRequest *req);
static long ts_jev_retry_delay_ms(TsHttpRequest *req);
static void ts_http_complete_easy(TsHttpSession *session, TsHttpRequest *req,
								 CURLMsg *msg);
static void ts_http_remove_easy(TsHttpSession *session, TsHttpRequest *req);
static void ts_http_prep_easy(TsHttpSession *session, TsHttpRequest *req);
static curl_socket_t ts_opensocket(void *clientp, curlsocktype purpose,
								   struct curl_sockaddr *address);
static int ts_sockopt(void *clientp, curl_socket_t curlfd, curlsocktype purpose);
static int ts_closesocket(void *clientp, curl_socket_t item);
static size_t ts_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
static size_t ts_header_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
static TsConn *ts_conn_by_fd(TsHttpSession *session, curl_socket_t fd);
static TsConn *ts_conn_alloc_slot(TsHttpSession *session, curl_socket_t fd);
static void ts_conn_close_slot(TsHttpSession *session, TsConn *conn);
static void ts_http_mark_idle(TsHttpSession *session, TsHttpRequest *req);
static void ts_http_sweep_stale(TsHttpSession *session);
static bool ts_http_asynch_dns_ok(void);
static void ts_http_free_owned(TsHttpSession *session, TsHttpRequest *req,
							   bool destroy_easy);
static CURL *ts_http_acquire_easy(TsHttpSession *session);
static void ts_http_release_easy(TsHttpSession *session, CURL *easy);
static void ts_http_cleanup_idle_easies(TsHttpSession *session);
static void ts_http_setup_multi(CURLM *multi);

void (*ts_http_success_hook)(void) = NULL;

uint32
ts_http_string_hash(const char *s)
{
	return (uint32) hash_bytes((const unsigned char *) (s ? s : ""),
							   s ? strlen(s) : 0);
}

void
ts_http_reject_header_injection(const char *credential)
{
	const char *p;

	if (credential == NULL)
		return;
	for (p = credential; *p != '\0'; p++)
	{
		if (*p == '\n' || *p == '\r')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("API key contains invalid characters")));
	}
}

static bool
ts_host_is_loopback_name(const char *host)
{
	if (host == NULL)
		return false;
	if (pg_strcasecmp(host, "localhost") == 0)
		return true;
	if (strcmp(host, "127.0.0.1") == 0)
		return true;
	if (strcmp(host, "::1") == 0 || strcmp(host, "[::1]") == 0)
		return true;
	return false;
}

static bool
ts_addr_is_loopback(const struct sockaddr *sa, socklen_t len)
{
	if (sa == NULL)
		return false;
	if (sa->sa_family == AF_INET && len >= (socklen_t) sizeof(struct sockaddr_in))
	{
		const struct sockaddr_in *in = (const struct sockaddr_in *) sa;
		uint32		addr = ntohl(in->sin_addr.s_addr);

		return (addr & 0xFF000000u) == 0x7F000000u;
	}
#ifdef AF_INET6
	if (sa->sa_family == AF_INET6 && len >= (socklen_t) sizeof(struct sockaddr_in6))
	{
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) sa;

		return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
	}
#endif
	return false;
}

void
ts_http_check_endpoint(const char *endpoint, const char *guc_name)
{
	CURLU	   *u;
	CURLUcode	rc;
	char	   *scheme = NULL;
	char	   *host = NULL;
	char	   *user = NULL;
	char	   *password = NULL;
	char	   *fragment = NULL;
	const char *p;
	bool		ok = false;
	bool		https;
	bool		http;
	bool		reject = false;

	if (guc_name == NULL)
		guc_name = "endpoint";

	if (endpoint == NULL || endpoint[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s is not set", guc_name)));

	for (p = endpoint; *p != '\0'; p++)
	{
		if ((unsigned char) *p < 0x20 || *p == 0x7f)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s contains control characters", guc_name)));
	}

	u = curl_url();
	if (u == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	rc = curl_url_set(u, CURLUPART_URL, endpoint, 0);
	if (rc != CURLUE_OK)
	{
		curl_url_cleanup(u);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s is not a valid URL: %s", guc_name, endpoint)));
	}

	(void) curl_url_get(u, CURLUPART_SCHEME, &scheme, 0);
	(void) curl_url_get(u, CURLUPART_HOST, &host, 0);
	(void) curl_url_get(u, CURLUPART_USER, &user, 0);
	(void) curl_url_get(u, CURLUPART_PASSWORD, &password, 0);
	(void) curl_url_get(u, CURLUPART_FRAGMENT, &fragment, 0);

	https = (scheme != NULL && pg_strcasecmp(scheme, "https") == 0);
	http = (scheme != NULL && pg_strcasecmp(scheme, "http") == 0);

	if (user != NULL && user[0] != '\0')
		reject = true;
	if (password != NULL && password[0] != '\0')
		reject = true;
	if (fragment != NULL && fragment[0] != '\0')
		reject = true;
	if (strchr(endpoint, '#') != NULL)
		reject = true;
	if (host == NULL || host[0] == '\0')
		reject = true;

	if (!reject)
	{
		if (https)
			ok = true;
		else if (http && ts_host_is_loopback_name(host))
			ok = true;
	}

	if (scheme)
		curl_free(scheme);
	if (host)
		curl_free(host);
	if (user)
		curl_free(user);
	if (password)
		curl_free(password);
	if (fragment)
		curl_free(fragment);
	curl_url_cleanup(u);

	if (!ok)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s must use https (or http to localhost): %s",
						guc_name, endpoint)));
}

static bool
ts_http_asynch_dns_ok(void)
{
	curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);

	return info != NULL && (info->features & CURL_VERSION_ASYNCHDNS) != 0;
}

static TsConn *
ts_conn_by_fd(TsHttpSession *session, curl_socket_t fd)
{
	int			i;

	if (fd == CURL_SOCKET_BAD)
		return NULL;
	for (i = 0; i < TS_HTTP_MAX_CONNS; i++)
	{
		if (session->conns[i].slot_used && session->conns[i].fd == fd &&
			session->conns[i].state != TS_CONN_CLOSED)
			return &session->conns[i];
	}
	return NULL;
}

static TsConn *
ts_conn_alloc_slot(TsHttpSession *session, curl_socket_t fd)
{
	int			i;

	for (i = 0; i < TS_HTTP_MAX_CONNS; i++)
	{
		if (!session->conns[i].slot_used || session->conns[i].state == TS_CONN_CLOSED)
		{
			memset(&session->conns[i], 0, sizeof(TsConn));
			session->conns[i].fd = fd;
			session->conns[i].state = TS_CONN_OPENING;
			session->conns[i].slot_used = true;
			INSTR_TIME_SET_CURRENT(session->conns[i].last_use);
			return &session->conns[i];
		}
	}
	return NULL;
}

static void
ts_conn_close_slot(TsHttpSession *session, TsConn *conn)
{
	(void) session;
	if (conn == NULL)
		return;
	conn->state = TS_CONN_CLOSED;
	conn->fd = CURL_SOCKET_BAD;
	conn->slot_used = false;
}

int
ts_http_idle_connections(TsHttpSession *session)
{
	int			i;
	int			n = 0;

	if (session == NULL)
		return 0;
	for (i = 0; i < TS_HTTP_MAX_CONNS; i++)
	{
		if (session->conns[i].slot_used && session->conns[i].state == TS_CONN_IDLE)
			n++;
	}
	return n;
}

Size
ts_http_conn_bytes(TsHttpSession *session)
{
	int			i;
	int			n = 0;

	if (session == NULL)
		return 0;
	for (i = 0; i < TS_HTTP_MAX_CONNS; i++)
	{
		if (session->conns[i].slot_used && session->conns[i].state != TS_CONN_CLOSED)
			n++;
	}
	return (Size) n * sizeof(TsConn);
}

int
ts_http_window_count(TsHttpSession *session)
{
	ListCell   *lc;
	int			n = 0;

	if (session == NULL)
		return 0;
	foreach(lc, session->requests)
	{
		TsHttpRequest *r = (TsHttpRequest *) lfirst(lc);

		if (r->state == TS_HTTP_QUEUED || r->state == TS_HTTP_ACTIVE ||
			r->state == TS_HTTP_BACKOFF)
			n++;
	}
	return n;
}

int
ts_http_active_count(TsHttpSession *session)
{
	return ts_http_window_count(session);
}

int
ts_http_live_easies(TsHttpSession *session)
{
	ListCell   *lc;
	int			n;

	if (session == NULL)
		return 0;
	n = list_length(session->idle_easies);
	foreach(lc, session->requests)
	{
		TsHttpRequest *r = (TsHttpRequest *) lfirst(lc);

		if (r->easy != NULL)
			n++;
	}
	return n;
}

bool
ts_http_request_terminal(TsHttpRequest *request)
{
	return request != NULL &&
		(request->state == TS_HTTP_SUCCEEDED ||
		 request->state == TS_HTTP_FAILED ||
		 request->state == TS_HTTP_CANCELLED);
}

bool
ts_http_request_succeeded(TsHttpRequest *request)
{
	return request != NULL && request->state == TS_HTTP_SUCCEEDED;
}

static curl_socket_t
ts_opensocket(void *clientp, curlsocktype purpose, struct curl_sockaddr *address)
{
	TsHttpRequest *req = (TsHttpRequest *) clientp;
	curl_socket_t s;
	TsConn	   *conn;

	(void) purpose;
	if (req == NULL || req->session == NULL || address == NULL)
		return CURL_SOCKET_BAD;

	if (pg_strncasecmp(req->policy.endpoint, "http://", 7) == 0 &&
		!ts_addr_is_loopback((struct sockaddr *) &address->addr, (socklen_t) address->addrlen))
	{
		req->loopback_rejected = true;
		return CURL_SOCKET_BAD;
	}

	s = socket(address->family, address->socktype, address->protocol);
	if (s == CURL_SOCKET_BAD)
		return CURL_SOCKET_BAD;

	conn = ts_conn_alloc_slot(req->session, s);
	if (conn == NULL)
	{
		closesocket(s);
		return CURL_SOCKET_BAD;
	}
	conn->endpoint_hash = ts_http_string_hash(req->policy.endpoint);
	conn->cred_hash = ts_http_string_hash(req->policy.credential);
	return s;
}

static int
ts_sockopt(void *clientp, curl_socket_t curlfd, curlsocktype purpose)
{
	TsHttpRequest *req = (TsHttpRequest *) clientp;
	TsConn	   *conn;
	struct sockaddr_storage peer;
	socklen_t	plen = sizeof(peer);
	int			one = 1;

	(void) purpose;
	if (req == NULL || req->session == NULL)
		return CURL_SOCKOPT_OK;

	conn = ts_conn_by_fd(req->session, curlfd);
	if (conn != NULL && conn->state == TS_CONN_OPENING)
		conn->state = TS_CONN_ACTIVE;

	if (pg_strncasecmp(req->policy.endpoint, "http://", 7) == 0)
	{
		if (getpeername(curlfd, (struct sockaddr *) &peer, &plen) == 0 &&
			!ts_addr_is_loopback((struct sockaddr *) &peer, plen))
		{
			req->loopback_rejected = true;
			return CURL_SOCKOPT_ERROR;
		}
	}

#ifdef SO_KEEPALIVE
	(void) setsockopt(curlfd, SOL_SOCKET, SO_KEEPALIVE, (const char *) &one, sizeof(one));
#endif
#ifdef TCP_KEEPIDLE
	{
		int			idle = 30;

		(void) setsockopt(curlfd, IPPROTO_TCP, TCP_KEEPIDLE, (const char *) &idle, sizeof(idle));
	}
#endif
#ifdef TCP_KEEPINTVL
	{
		int			intvl = 10;

		(void) setsockopt(curlfd, IPPROTO_TCP, TCP_KEEPINTVL, (const char *) &intvl, sizeof(intvl));
	}
#endif
#ifdef TCP_KEEPCNT
	{
		int			cnt = 3;

		(void) setsockopt(curlfd, IPPROTO_TCP, TCP_KEEPCNT, (const char *) &cnt, sizeof(cnt));
	}
#endif
	return CURL_SOCKOPT_OK;
}

static int
ts_closesocket(void *clientp, curl_socket_t item)
{
	TsHttpSession *session = (TsHttpSession *) clientp;
	TsConn	   *conn;

	if (session != NULL)
	{
		conn = ts_conn_by_fd(session, item);
		ts_conn_close_slot(session, conn);
	}
	if (item != CURL_SOCKET_BAD)
		closesocket(item);
	return 0;
}

static size_t
ts_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	TsHttpRequest *req = (TsHttpRequest *) userdata;
	size_t		nbytes = size * nmemb;
	size_t		need;
	char	   *nbuf;

	if (req == NULL)
		return 0;
	if (req->resp_overflow || req->resp_oom)
		return 0;

	need = req->resp_len + nbytes;
	if (need > req->policy.max_response)
	{
		req->resp_overflow = true;
		return 0;
	}
	if (need + 1 > req->resp_cap)
	{
		size_t		ncap = req->resp_cap == 0 ? 4096 : req->resp_cap * 2;

		while (ncap < need + 1)
			ncap *= 2;
		if (ncap > req->policy.max_response + 1)
			ncap = req->policy.max_response + 1;
		nbuf = (char *) realloc(req->resp_data, ncap);
		if (nbuf == NULL)
		{
			req->resp_oom = true;
			return 0;
		}
		req->resp_data = nbuf;
		req->resp_cap = ncap;
	}
	memcpy(req->resp_data + req->resp_len, ptr, nbytes);
	req->resp_len += nbytes;
	req->resp_data[req->resp_len] = '\0';
	return nbytes;
}

static size_t
ts_header_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	TsHttpRequest *req = (TsHttpRequest *) userdata;
	size_t		nbytes = size * nmemb;

	if (req == NULL || nbytes == 0)
		return nbytes;

	if (nbytes >= 12 && pg_strncasecmp(ptr, "Retry-After:", 12) == 0)
	{
		size_t		off = 12;
		size_t		len;

		while (off < nbytes && (ptr[off] == ' ' || ptr[off] == '\t'))
			off++;
		len = nbytes - off;
		if (len >= sizeof(req->header_retry_after))
			len = sizeof(req->header_retry_after) - 1;
		memcpy(req->header_retry_after, ptr + off, len);
		while (len > 0 && (req->header_retry_after[len - 1] == '\n' ||
						   req->header_retry_after[len - 1] == '\r'))
			len--;
		req->header_retry_after[len] = '\0';
	}
	else if (nbytes >= 15 && pg_strncasecmp(ptr, "Retry-After-Ms:", 15) == 0)
	{
		size_t		off = 15;
		size_t		len;

		while (off < nbytes && (ptr[off] == ' ' || ptr[off] == '\t'))
			off++;
		len = nbytes - off;
		if (len >= sizeof(req->header_retry_after_ms))
			len = sizeof(req->header_retry_after_ms) - 1;
		memcpy(req->header_retry_after_ms, ptr + off, len);
		while (len > 0 && (req->header_retry_after_ms[len - 1] == '\n' ||
						   req->header_retry_after_ms[len - 1] == '\r'))
			len--;
		req->header_retry_after_ms[len] = '\0';
	}
	return nbytes;
}

static long
ts_legacy_retry_delay_ms(TsHttpRequest *req)
{
	long		delay = 200L * (1L << Min(req->attempts, 8));

#if LIBCURL_VERSION_NUM >= 0x074200
	{
		curl_off_t	ra = 0;

		if (req->easy != NULL &&
			curl_easy_getinfo(req->easy, CURLINFO_RETRY_AFTER, &ra) == CURLE_OK &&
			ra > 0)
		{
			if (ra > TYPESAFE_RETRY_MAX_WAIT_MS / 1000)
				ra = TYPESAFE_RETRY_MAX_WAIT_MS / 1000;
			if (ra * 1000 > delay)
				delay = (long) ra * 1000;
		}
	}
#endif
	if (delay > TYPESAFE_RETRY_MAX_WAIT_MS)
		delay = TYPESAFE_RETRY_MAX_WAIT_MS;
	return delay;
}

static long
ts_jev_retry_delay_ms(TsHttpRequest *req)
{
	long		delay;
	long		exp;
	long		jitter;
	char	   *end;

	exp = 500L * (1L << Min(req->attempts, 4));
	if (exp > 8000)
		exp = 8000;
	delay = exp;

	if (req->header_retry_after_ms[0] != '\0')
	{
		long		ms = strtol(req->header_retry_after_ms, &end, 10);

		if (end != req->header_retry_after_ms && ms > 0)
			delay = ms;
	}
	else if (req->header_retry_after[0] != '\0')
	{
		curl_off_t	ra = 0;

#if LIBCURL_VERSION_NUM >= 0x074200
		if (req->easy != NULL &&
			curl_easy_getinfo(req->easy, CURLINFO_RETRY_AFTER, &ra) == CURLE_OK &&
			ra > 0)
			delay = (long) ra * 1000;
		else
#endif
		{
			double		sec = strtod(req->header_retry_after, &end);

			if (end != req->header_retry_after && sec > 0)
				delay = (long) (sec * 1000.0);
		}
	}

	if (delay > 30000)
		delay = 30000;

	jitter = (long) (random() % 251);
	delay += jitter;
	return delay;
}

static void
ts_http_remove_easy(TsHttpSession *session, TsHttpRequest *req)
{
	if (session != NULL && session->multi != NULL && req->easy != NULL && req->on_multi)
	{
		curl_multi_remove_handle(session->multi, req->easy);
		req->on_multi = false;
		if (session->in_flight > 0 && req->state == TS_HTTP_ACTIVE)
			session->in_flight--;
	}
}

static void
ts_http_prep_easy(TsHttpSession *session, TsHttpRequest *req)
{
	char	   *auth;
	struct curl_slist *headers = NULL;

	if (req->easy == NULL)
	{
		req->easy = ts_http_acquire_easy(session);
		if (req->easy == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not initialize HTTP client")));
	}

	if (req->headers != NULL)
	{
		curl_slist_free_all(req->headers);
		req->headers = NULL;
	}

	headers = curl_slist_append(NULL, "Content-Type: application/json");
	if (headers == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	headers = curl_slist_append(headers, "Accept: application/json");
	headers = curl_slist_append(headers, req->policy.user_agent);
	headers = curl_slist_append(headers, "Expect:");
	auth = psprintf("Authorization: Bearer %s", req->policy.credential);
	headers = curl_slist_append(headers, auth);
	pfree(auth);
	req->headers = headers;

	req->resp_len = 0;
	req->resp_overflow = false;
	req->resp_oom = false;
	req->loopback_rejected = false;
	req->header_retry_after[0] = '\0';
	req->header_retry_after_ms[0] = '\0';
	req->http_status = 0;
	req->curl_rc = CURLE_OK;

	if (curl_easy_setopt(req->easy, CURLOPT_URL, req->policy.endpoint) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_POST, 1L) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_HTTPHEADER, req->headers) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_POSTFIELDS, req->payload) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_POSTFIELDSIZE, (long) req->payload_len) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_TIMEOUT_MS, req->policy.timeout_ms) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_TCP_KEEPALIVE, 1L) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_SSL_VERIFYPEER, 1L) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_SSL_VERIFYHOST, 2L) != CURLE_OK ||
#if LIBCURL_VERSION_NUM >= 0x075500
		curl_easy_setopt(req->easy, CURLOPT_PROTOCOLS_STR, "http,https") != CURLE_OK ||
#else
		curl_easy_setopt(req->easy, CURLOPT_PROTOCOLS, (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS)) != CURLE_OK ||
#endif
		curl_easy_setopt(req->easy, CURLOPT_HTTP_VERSION,
						 req->policy.http11 ? (long) CURL_HTTP_VERSION_1_1
						 : (long) CURL_HTTP_VERSION_2TLS) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_WRITEFUNCTION, (curl_write_callback) ts_write_cb) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_WRITEDATA, req) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_HEADERFUNCTION, (curl_write_callback) ts_header_cb) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_HEADERDATA, req) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_PRIVATE, req) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_OPENSOCKETFUNCTION, ts_opensocket) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_OPENSOCKETDATA, req) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_SOCKOPTFUNCTION, ts_sockopt) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_SOCKOPTDATA, req) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_CLOSESOCKETFUNCTION, ts_closesocket) != CURLE_OK ||
		curl_easy_setopt(req->easy, CURLOPT_CLOSESOCKETDATA, session) != CURLE_OK)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not configure HTTP client")));

#ifdef CURLOPT_MAXAGE_CONN
	if (req->policy.keepalive_s > 0)
		(void) curl_easy_setopt(req->easy, CURLOPT_MAXAGE_CONN, (long) req->policy.keepalive_s);
#endif
	if (req->policy.keepalive_s <= 0)
		(void) curl_easy_setopt(req->easy, CURLOPT_FORBID_REUSE, 1L);
	else
		(void) curl_easy_setopt(req->easy, CURLOPT_FORBID_REUSE, 0L);

	(void) session;
}

static void
ts_http_mark_idle(TsHttpSession *session, TsHttpRequest *req)
{
	curl_socket_t fd = CURL_SOCKET_BAD;
	TsConn	   *c;

	if (req->easy != NULL)
		(void) curl_easy_getinfo(req->easy, CURLINFO_ACTIVESOCKET, &fd);
	c = ts_conn_by_fd(session, fd);
	if (c == NULL)
		return;
	if (req->policy.keepalive_s <= 0)
	{
		c->state = TS_CONN_STALE;
		return;
	}
	c->state = TS_CONN_IDLE;
	INSTR_TIME_SET_CURRENT(c->last_use);
}

static void
ts_http_sweep_stale(TsHttpSession *session)
{
	int			i;

	for (i = 0; i < TS_HTTP_MAX_CONNS; i++)
	{
		TsConn	   *c = &session->conns[i];
		instr_time	now;
		double		age;

		if (!c->slot_used || c->state != TS_CONN_IDLE)
			continue;
		INSTR_TIME_SET_CURRENT(now);
		age = (double) (INSTR_TIME_GET_MICROSEC(now) - INSTR_TIME_GET_MICROSEC(c->last_use)) / 1000000.0;
		(void) age;
	}
}

static bool
ts_should_retry(TsHttpRequest *req, CURLcode rc, long status)
{
	if (req->attempts + 1 >= req->policy.max_attempts)
		return false;

	if (req->family == TS_FAMILY_LEGACY)
	{
		if (rc == CURLE_OK && (status == 429 || status == 529) &&
			req->attempts < TYPESAFE_MAX_RETRIES)
			return true;
		return false;
	}

	/* Jev: transport failures and 408/429/529/5xx. */
	if (rc != CURLE_OK)
	{
		if (rc == CURLE_WRITE_ERROR && (req->resp_overflow || req->resp_oom))
			return false;
		return true;
	}
	if (status == 408 || status == 429 || status == 529 || status >= 500)
		return true;
	return false;
}

static void
ts_http_complete_easy(TsHttpSession *session, TsHttpRequest *req, CURLMsg *msg)
{
	long		status = 0;
	CURLcode	rc = msg->data.result;
	bool		immediate_fresh = false;

	curl_easy_getinfo(req->easy, CURLINFO_RESPONSE_CODE, &status);
	req->curl_rc = rc;
	req->http_status = status;

	if (req->loopback_rejected)
	{
		ts_http_remove_easy(session, req);
		req->state = TS_HTTP_FAILED;
		return;
	}

	if (rc == CURLE_OK && status == 200)
	{
		char	   *converted;

		if (req->resp_data == NULL)
			req->resp_data = (char *) malloc(1);
		if (req->resp_data != NULL)
			req->resp_data[req->resp_len] = '\0';
		converted = pg_any_to_server(req->resp_data ? req->resp_data : "",
									 (int) req->resp_len, PG_UTF8);
		if (converted == NULL)
			converted = "";
		req->result_body = MemoryContextStrdup(session->ctx, converted);
		ts_http_mark_idle(session, req);
		ts_http_remove_easy(session, req);
		req->state = TS_HTTP_SUCCEEDED;
		if (ts_http_success_hook)
			ts_http_success_hook();
		return;
	}

	ts_http_remove_easy(session, req);

	if (rc == CURLE_WRITE_ERROR && req->resp_overflow)
	{
		req->state = TS_HTTP_FAILED;
		return;
	}

	/* First-attempt reused-connection failure: retry immediately. */
	if (req->family == TS_FAMILY_JEV && req->attempts == 0 && rc != CURLE_OK)
		immediate_fresh = true;

	if (ts_should_retry(req, rc, status))
	{
		long		delay;

		req->attempts++;
		req->retries_done++;
		if (immediate_fresh)
		{
			req->has_retry_at = false;
			req->state = TS_HTTP_QUEUED;
			(void) curl_easy_setopt(req->easy, CURLOPT_FRESH_CONNECT, 1L);
			return;
		}
		delay = (req->family == TS_FAMILY_LEGACY)
			? ts_legacy_retry_delay_ms(req)
			: ts_jev_retry_delay_ms(req);
		{
			instr_time	now;

			INSTR_TIME_SET_CURRENT(now);
			req->retry_at_us = INSTR_TIME_GET_MICROSEC(now) + (int64) delay * 1000;
			req->has_retry_at = true;
		}
		req->state = TS_HTTP_BACKOFF;
		return;
	}

	req->state = TS_HTTP_FAILED;
}

static void
ts_http_raise_failed(TsHttpRequest *req)
{
	if (req->resp_overflow)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("%s HTTP response exceeds %d bytes",
						req->family == TS_FAMILY_JEV ? "jev" : "typesafe",
						TYPESAFE_MAX_RESPONSE_BYTES)));

	if (req->loopback_rejected)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s must use https (or http to localhost): %s",
						req->policy.guc_name, req->policy.endpoint)));

	if (req->family == TS_FAMILY_JEV)
	{
		if (req->curl_rc != CURLE_OK)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("jev: TypeSafe API unreachable after retries: %s",
							curl_easy_strerror(req->curl_rc))));
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("jev: TypeSafe API error %ld", req->http_status)));
	}

	if (req->curl_rc != CURLE_OK)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("typesafe HTTP request failed: %s",
						curl_easy_strerror(req->curl_rc))));

	{
		const char *body = req->resp_data ? req->resp_data : "";
		int			len = (int) req->resp_len;
		char	   *trunc;

		if (len > TYPESAFE_BODY_TRUNC)
			len = TYPESAFE_BODY_TRUNC;
		trunc = pnstrdup(body, len);
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("typesafe HTTP request failed with status %ld",
						req->http_status),
				 errdetail("Response body (truncated): %s", trunc)));
	}
}

TsHttpSession *
ts_http_session_create(MemoryContext parent)
{
	MemoryContext old;
	TsHttpSession *session;
	int			i;

	old = MemoryContextSwitchTo(parent);
	session = (TsHttpSession *) palloc0(sizeof(TsHttpSession));
	session->ctx = parent;
	session->multi = curl_multi_init();
	if (session->multi == NULL)
	{
		MemoryContextSwitchTo(old);
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not initialize HTTP client")));
	}
	ts_http_setup_multi(session->multi);
	for (i = 0; i < TS_HTTP_MAX_CONNS; i++)
	{
		session->conns[i].fd = CURL_SOCKET_BAD;
		session->conns[i].state = TS_CONN_CLOSED;
	}
	MemoryContextSwitchTo(old);
	return session;
}

void
ts_http_session_cancel_inflight(TsHttpSession *session)
{
	ListCell   *lc;

	if (session == NULL)
		return;

	foreach(lc, session->requests)
	{
		TsHttpRequest *r = (TsHttpRequest *) lfirst(lc);

		if (r->state == TS_HTTP_ACTIVE || r->state == TS_HTTP_QUEUED ||
			r->state == TS_HTTP_BACKOFF)
			ts_http_cancel_request(session, r);
	}
}

void
ts_http_session_rebuild_multi(TsHttpSession *session)
{
	int			i;

	if (session == NULL)
		return;

	if (session->multi != NULL)
	{
		curl_multi_cleanup(session->multi);
		session->multi = NULL;
	}
	{
		ListCell   *lc;

		foreach(lc, session->requests)
		{
			TsHttpRequest *r = (TsHttpRequest *) lfirst(lc);

			r->on_multi = false;
		}
	}
	for (i = 0; i < TS_HTTP_MAX_CONNS; i++)
	{
		session->conns[i].slot_used = false;
		session->conns[i].state = TS_CONN_CLOSED;
		session->conns[i].fd = CURL_SOCKET_BAD;
	}
	session->in_flight = 0;
	ts_http_cleanup_idle_easies(session);
	session->multi = curl_multi_init();
	if (session->multi == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not initialize HTTP client")));
	ts_http_setup_multi(session->multi);
	session->have_identity = false;
}

void
ts_http_session_reset_multi(TsHttpSession *session)
{
	ts_http_session_cancel_inflight(session);
	ts_http_session_rebuild_multi(session);
}

static void
ts_http_setup_multi(CURLM *multi)
{
	if (multi == NULL)
		return;
	(void) curl_multi_setopt(multi, CURLMOPT_MAXCONNECTS, (long) TS_HTTP_MAX_CONNS);
#ifdef CURLMOPT_MAX_TOTAL_CONNECTIONS
	(void) curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
							(long) TS_HTTP_MAX_CONNS);
#endif
#ifdef CURLMOPT_MAX_HOST_CONNECTIONS
	(void) curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS,
							(long) TS_HTTP_MAX_CONNS);
#endif
}

static CURL *
ts_http_acquire_easy(TsHttpSession *session)
{
	CURL	   *easy;

	if (session->idle_easies != NIL)
	{
		easy = (CURL *) linitial(session->idle_easies);
		session->idle_easies = list_delete_first(session->idle_easies);
		curl_easy_reset(easy);
		return easy;
	}
	return curl_easy_init();
}

static void
ts_http_release_easy(TsHttpSession *session, CURL *easy)
{
	MemoryContext old;

	if (easy == NULL)
		return;
	(void) curl_easy_setopt(easy, CURLOPT_PRIVATE, NULL);
	(void) curl_easy_setopt(easy, CURLOPT_WRITEDATA, NULL);
	(void) curl_easy_setopt(easy, CURLOPT_HEADERDATA, NULL);
	(void) curl_easy_setopt(easy, CURLOPT_OPENSOCKETFUNCTION, NULL);
	(void) curl_easy_setopt(easy, CURLOPT_OPENSOCKETDATA, NULL);
	(void) curl_easy_setopt(easy, CURLOPT_SOCKOPTFUNCTION, NULL);
	(void) curl_easy_setopt(easy, CURLOPT_SOCKOPTDATA, NULL);
	(void) curl_easy_setopt(easy, CURLOPT_CLOSESOCKETFUNCTION, ts_closesocket);
	(void) curl_easy_setopt(easy, CURLOPT_CLOSESOCKETDATA, session);
	if (session != NULL &&
		list_length(session->idle_easies) < TS_HTTP_MAX_IDLE_EASIES)
	{
		curl_easy_reset(easy);
		old = MemoryContextSwitchTo(session->ctx);
		session->idle_easies = lappend(session->idle_easies, easy);
		MemoryContextSwitchTo(old);
		return;
	}
	curl_easy_cleanup(easy);
}

static void
ts_http_cleanup_idle_easies(TsHttpSession *session)
{
	ListCell   *lc;

	if (session == NULL)
		return;
	foreach(lc, session->idle_easies)
		curl_easy_cleanup((CURL *) lfirst(lc));
	list_free(session->idle_easies);
	session->idle_easies = NIL;
}

static void
ts_http_free_owned(TsHttpSession *session, TsHttpRequest *req, bool destroy_easy)
{
	CURL	   *easy;

	if (req == NULL)
		return;

	ts_http_remove_easy(session, req);
	easy = req->easy;
	req->easy = NULL;
	req->on_multi = false;
	if (easy)
	{
		if (destroy_easy)
		{
			(void) curl_easy_setopt(easy, CURLOPT_CLOSESOCKETFUNCTION, ts_closesocket);
			(void) curl_easy_setopt(easy, CURLOPT_CLOSESOCKETDATA, session);
			curl_easy_cleanup(easy);
		}
		else
			ts_http_release_easy(session, easy);
	}
	if (req->headers)
	{
		curl_slist_free_all(req->headers);
		req->headers = NULL;
	}
	if (req->resp_data)
	{
		free(req->resp_data);
		req->resp_data = NULL;
		req->resp_cap = 0;
		req->resp_len = 0;
	}
	if (req->payload)
	{
		pfree(req->payload);
		req->payload = NULL;
		req->payload_len = 0;
	}
	if (req->result_body)
	{
		pfree(req->result_body);
		req->result_body = NULL;
	}
	if (req->policy.endpoint)
	{
		pfree((void *) req->policy.endpoint);
		req->policy.endpoint = NULL;
	}
	if (req->policy.credential)
	{
		pfree((void *) req->policy.credential);
		req->policy.credential = NULL;
	}
	if (req->policy.guc_name)
	{
		pfree((void *) req->policy.guc_name);
		req->policy.guc_name = NULL;
	}
	if (req->policy.user_agent)
	{
		pfree((void *) req->policy.user_agent);
		req->policy.user_agent = NULL;
	}
}

void
ts_http_finalize_request(TsHttpSession *session, TsHttpRequest *request)
{
	if (request == NULL)
		return;
	if (session == NULL)
		session = request->session;
	if (session == NULL)
		return;

	if (!ts_http_request_terminal(request))
		ts_http_cancel_request(session, request);
	ts_http_free_owned(session, request, true);
	session->requests = list_delete_ptr(session->requests, request);
	request->session = NULL;
	request->owner = NULL;
	pfree(request);
}

void
ts_http_recycle_request(TsHttpSession *session, TsHttpRequest *request)
{
	if (request == NULL)
		return;
	if (session == NULL)
		session = request->session;
	if (session == NULL)
		return;

	if (!ts_http_request_terminal(request))
		ts_http_cancel_request(session, request);
	ts_http_free_owned(session, request, false);
	session->requests = list_delete_ptr(session->requests, request);
	request->session = NULL;
	request->owner = NULL;
	pfree(request);
}

void
ts_http_finalize_all(TsHttpSession *session)
{
	if (session == NULL)
		return;
	while (session->requests != NIL)
		ts_http_finalize_request(session,
									(TsHttpRequest *) linitial(session->requests));
}

void
ts_http_finalize_cancelled(TsHttpSession *session)
{
	List	   *dead = NIL;
	ListCell   *lc;

	if (session == NULL)
		return;
	foreach(lc, session->requests)
	{
		TsHttpRequest *r = (TsHttpRequest *) lfirst(lc);

		if (r->state == TS_HTTP_CANCELLED)
			dead = lappend(dead, r);
	}
	foreach(lc, dead)
		ts_http_finalize_request(session, (TsHttpRequest *) lfirst(lc));
	list_free(dead);
}

void
ts_http_session_destroy(TsHttpSession *session)
{
	if (session == NULL)
		return;

	ts_http_finalize_all(session);
	ts_http_cleanup_idle_easies(session);
	if (session->multi)
	{
		curl_multi_cleanup(session->multi);
		session->multi = NULL;
	}
}

TsHttpRequest *
ts_http_submit(TsHttpSession *session, const TsHttpPolicy *policy,
			   const char *payload, uint64 owner_generation)
{
	MemoryContext old;
	TsHttpRequest *req;
	char	   *utf8;

	ts_http_check_endpoint(policy->endpoint, policy->guc_name);
	ts_http_reject_header_injection(policy->credential);

	if (policy->require_asynch_dns && !ts_http_asynch_dns_ok())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("jev requires libcurl built with asynchronous DNS")));

	if (payload != NULL && strlen(payload) > TYPESAFE_MAX_RESPONSE_BYTES)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("jev request exceeds %d bytes", TYPESAFE_MAX_RESPONSE_BYTES)));

	old = MemoryContextSwitchTo(session->ctx);
	req = (TsHttpRequest *) palloc0(sizeof(TsHttpRequest));
	req->session = session;
	req->policy = *policy;
	req->policy.endpoint = pstrdup(policy->endpoint);
	req->policy.credential = pstrdup(policy->credential);
	req->policy.guc_name = pstrdup(policy->guc_name);
	req->policy.user_agent = pstrdup(policy->user_agent);
	req->family = policy->family;
	req->owner_generation = owner_generation;
	utf8 = pg_server_to_any(payload, strlen(payload), PG_UTF8);
	req->payload = MemoryContextStrdup(session->ctx, utf8);
	req->payload_len = strlen(req->payload);
	req->state = TS_HTTP_QUEUED;
	session->requests = lappend(session->requests, req);
	MemoryContextSwitchTo(old);
	return req;
}

void
ts_http_cancel_request(TsHttpSession *session, TsHttpRequest *request)
{
	if (request == NULL || ts_http_request_terminal(request))
		return;
	if (request->state == TS_HTTP_ACTIVE)
		ts_http_remove_easy(session, request);
	request->state = TS_HTTP_CANCELLED;
}

void
ts_http_cancel_owner(TsHttpSession *session, uint64 generation)
{
	ListCell   *lc;

	if (session == NULL)
		return;
	foreach(lc, session->requests)
	{
		TsHttpRequest *r = (TsHttpRequest *) lfirst(lc);

		if (r->owner_generation == generation && !ts_http_request_terminal(r))
			ts_http_cancel_request(session, r);
	}
}

void
ts_http_cancel_all(TsHttpSession *session)
{
	ListCell   *lc;

	if (session == NULL)
		return;
	foreach(lc, session->requests)
	{
		TsHttpRequest *r = (TsHttpRequest *) lfirst(lc);

		ts_http_cancel_request(session, r);
	}
}

static void
ts_http_launch(TsHttpSession *session)
{
	ListCell   *lc;
	instr_time	now;

	INSTR_TIME_SET_CURRENT(now);

	foreach(lc, session->requests)
	{
		TsHttpRequest *r = (TsHttpRequest *) lfirst(lc);

		if (session->pause_admit && r->state == TS_HTTP_QUEUED)
			continue;
		if (r->state == TS_HTTP_BACKOFF && r->has_retry_at)
		{
			if (INSTR_TIME_GET_MICROSEC(now) >= r->retry_at_us)
				r->state = TS_HTTP_QUEUED;
		}
		if (r->state != TS_HTTP_QUEUED)
			continue;
		if (session->in_flight >= r->policy.concurrency)
			continue;

		ts_http_prep_easy(session, r);
		if (curl_multi_add_handle(session->multi, r->easy) != CURLM_OK)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not start HTTP request")));
		r->on_multi = true;
		r->state = TS_HTTP_ACTIVE;
		session->in_flight++;
	}
}

void
ts_http_pump(TsHttpSession *session, long maximum_wait_ms)
{
	int			still_running = 0;
	CURLMsg    *msg;
	int			left;
	long		sleep_ms;
	ListCell   *lc;
	bool		waiting_retry = false;

	if (session == NULL || session->multi == NULL)
		return;

	CHECK_FOR_INTERRUPTS();
	ts_http_sweep_stale(session);
	ts_http_launch(session);

	if (curl_multi_perform(session->multi, &still_running) != CURLM_OK)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("typesafe HTTP request failed")));

	while ((msg = curl_multi_info_read(session->multi, &left)) != NULL)
	{
		TsHttpRequest *req = NULL;

		if (msg->msg != CURLMSG_DONE)
			continue;
		curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &req);
		if (req == NULL)
			continue;
		ts_http_complete_easy(session, req, msg);
	}

	sleep_ms = maximum_wait_ms > 0 ? maximum_wait_ms : TYPESAFE_WAIT_QUANTUM_MS;
	if (sleep_ms > TYPESAFE_WAIT_QUANTUM_MS)
		sleep_ms = TYPESAFE_WAIT_QUANTUM_MS;

	foreach(lc, session->requests)
	{
		TsHttpRequest *r = (TsHttpRequest *) lfirst(lc);

		if (r->state == TS_HTTP_BACKOFF && r->has_retry_at)
		{
			instr_time	now;
			long		ms;

			waiting_retry = true;
			INSTR_TIME_SET_CURRENT(now);
			ms = (long) ((r->retry_at_us - INSTR_TIME_GET_MICROSEC(now)) / 1000);
			if (ms < 1)
				ms = 1;
			if (ms < sleep_ms)
				sleep_ms = ms;
		}
	}

	if (session->in_flight > 0)
	{
		int			numfds = 0;

		curl_multi_wait(session->multi, NULL, 0, (int) sleep_ms, &numfds);
	}
	else if (waiting_retry)
	{
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 sleep_ms,
						 PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
	}
}

void
ts_http_await(TsHttpSession *session, TsHttpRequest *request)
{
	instr_time	start;

	INSTR_TIME_SET_CURRENT(start);
	while (!ts_http_request_terminal(request))
	{
		CHECK_FOR_INTERRUPTS();
		ts_http_pump(session, TYPESAFE_WAIT_QUANTUM_MS);
	}
	{
		instr_time	now;

		INSTR_TIME_SET_CURRENT(now);
		INSTR_TIME_SUBTRACT(now, start);
		request->elapsed_ms = (long) INSTR_TIME_GET_MILLISEC(now);
	}
	if (request->state == TS_HTTP_CANCELLED)
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				 errmsg("canceling statement due to user request")));
	if (request->state != TS_HTTP_SUCCEEDED)
		ts_http_raise_failed(request);
}
