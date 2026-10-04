/*-------------------------------------------------------------------------
 *
 * typesafe_internal.h
 *		Shared internals for typesafe and native Jev.
 *
 *-------------------------------------------------------------------------
 */
#ifndef TYPESAFE_INTERNAL_H
#define TYPESAFE_INTERNAL_H

#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/jsonb.h"

#define TYPESAFE_DEFAULT_ENDPOINT \
	"https://api.typesafe.ai/v1/systemone"
#define TYPESAFE_DEFAULT_MODEL	"jev-latest"
#define TYPESAFE_USER_AGENT_HDR	"User-Agent: postgres-typesafe/1.0"
#define TYPESAFE_BODY_TRUNC		1024
#define TYPESAFE_MAX_RETRIES		3
#define TYPESAFE_MAX_RESPONSE_BYTES	(8 * 1024 * 1024)
#define TYPESAFE_RETRY_MAX_WAIT_MS	60000
#define TYPESAFE_WAIT_QUANTUM_MS	200

#ifdef pg_noreturn
pg_noreturn void json_shape_error(const char *detail);
#else
void		json_shape_error(const char *detail) pg_attribute_noreturn();
#endif

extern bool jsonb_str_eq(const JsonbValue *v, const char *s);
extern char *jsonb_str_cstring(const JsonbValue *v);
extern float8 jsonb_numeric_float8(const JsonbValue *v);
extern int32 jsonb_numeric_int4(const JsonbValue *v);
extern JsonbContainer *jsonb_value_object_container(JsonbValue *v);
extern Jsonb *jsonb_value_object_to_jsonb(JsonbValue *v);

extern void jev_startup(void);

#endif							/* TYPESAFE_INTERNAL_H */
