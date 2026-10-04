# pg_typesafe — PostgreSQL client for TypeSafe AI (Jev)

MODULE_big = typesafe
OBJS = \
	$(WIN32RES) \
	typesafe.o \
	typesafe_http.o \
	typesafe_jev.o \
	typesafe_jev_scan.o

EXTENSION = typesafe
DATA = typesafe--0.0.1.sql typesafe--0.1.0.sql typesafe--0.0.1--0.1.0.sql
PGFILEDESC = "typesafe - TypeSafe AI client for categorical classification"

REGRESS = typesafe
TAP_TESTS = 1

# Standalone checkout uses PGXS.  In the Postgres tree, keep the contrib path.
ifeq ($(wildcard ../../src/Makefile.global),)
  USE_PGXS ?= 1
endif

ifdef USE_PGXS
PG_CONFIG ?= pg_config
# 计划合同：TYPESAFE_CURL_LIBS 非空才走覆盖；空字符串不得 ifdef 命中后仍调 pkg-config
ifneq ($(TYPESAFE_CURL_LIBS),)
PG_CPPFLAGS += $(TYPESAFE_CURL_CFLAGS)
SHLIB_LINK  += $(TYPESAFE_CURL_LIBS)
else
PG_CPPFLAGS += $(shell pkg-config --cflags libcurl 2>/dev/null)
SHLIB_LINK  += $(shell pkg-config --libs libcurl 2>/dev/null)
ifeq ($(SHLIB_LINK),)
SHLIB_LINK += -lcurl
endif
endif
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
ifneq ($(TYPESAFE_CURL_LIBS),)
PG_CPPFLAGS += $(TYPESAFE_CURL_CFLAGS)
SHLIB_LINK  += $(TYPESAFE_CURL_LIBS)
else
PG_CPPFLAGS += $(LIBCURL_CPPFLAGS)
SHLIB_LINK += $(LIBCURL_LDFLAGS) $(LIBCURL_LDLIBS)
endif
subdir = contrib/typesafe
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
