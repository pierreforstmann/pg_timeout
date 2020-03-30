# pg_timeout Makefile

MODULES = pg_timeout 

EXTENSION = pg_timeout
DATA = pg_timeout--1.0.sql
PGFILEDESC = "pg_timeout - backgroud worker to enable session timeout"

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
