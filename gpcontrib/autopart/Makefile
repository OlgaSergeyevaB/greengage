# contrib/autopart/Makefile

MODULE_big = autopart

OBJS = src/init.o src/relation_info.o src/utils.o src/partition_filter.o \
	src/autopart.o src/rangeset.o \
	src/pl_funcs.o \
	src/hooks.o src/xact_handling.o \
	src/planner_tree_modification.o src/partition_creation.o \
	src/autopart_worker.o \
	src/compat/pg_compat.o src/compat/rowmarks_fix.o $(WIN32RES)

ifdef USE_PGXS
override PG_CPPFLAGS += -I$(CURDIR)/src/include
else
override PG_CPPFLAGS += -I$(top_srcdir)/$(subdir)/src/include
endif

EXTENSION = autopart

EXTVERSION = 1.0

DATA_built = autopart--$(EXTVERSION).sql

PGFILEDESC = "autopart - declarative RANGE auto-partitioning on INSERT"

ifdef USE_PGXS
PG_CONFIG=pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/autopart
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

$(EXTENSION)--$(EXTVERSION).sql: init.sql
	cat $^ > $@

clean_gcov:
	find . \
		-name "*.gcda" -delete -o \
		-name "*.gcno" -delete -o \
		-name "*.gcov" -delete
