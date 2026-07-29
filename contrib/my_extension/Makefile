MODULE_big = my_extension
OBJS = my_extension.o rest_server.o http_handle.o
PGFILEDESC = "my_extension with C func"

EXTENSION = my_extension
DATA = my_extension--1.0.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)