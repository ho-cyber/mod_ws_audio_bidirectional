# mod_ws_audio Makefile
#
# Build requirements:
#   apt install freeswitch-dev libwebsockets-dev libssl-dev
#
# Usage:
#   make            – build mod_ws_audio.so
#   make install    – copy to FreeSWITCH modules directory
#   make clean      – remove build artefacts

CC       ?= gcc
MODULE   = mod_ws_audio

# ── FreeSWITCH ────────────────────────────────────────────────────────────────
# Detect FS include path: prefer pkg-config, fall back to common locations.
FS_CFLAGS := $(shell pkg-config --cflags freeswitch 2>/dev/null)
ifeq ($(FS_CFLAGS),)
  FS_INCLUDE_CANDIDATES := \
    /usr/include/freeswitch \
    /usr/local/include/freeswitch \
    /usr/local/freeswitch/include \
    /opt/freeswitch/include
  FS_INCLUDE := $(firstword $(wildcard $(FS_INCLUDE_CANDIDATES)))
  ifeq ($(FS_INCLUDE),)
    $(error Cannot find FreeSWITCH headers. Install freeswitch-dev or set FS_INCLUDE manually.)
  endif
  FS_CFLAGS := -I$(FS_INCLUDE)
endif

FS_MODDIR := $(shell pkg-config --variable=modulesdir freeswitch 2>/dev/null)
ifeq ($(FS_MODDIR),)
  FS_MODDIR_CANDIDATES := \
    /usr/lib/freeswitch/mod \
    /usr/local/lib/freeswitch/mod \
    /usr/local/freeswitch/mod \
    /opt/freeswitch/mod
  FS_MODDIR := $(firstword $(wildcard $(FS_MODDIR_CANDIDATES)))
  ifeq ($(FS_MODDIR),)
    FS_MODDIR := /usr/lib/freeswitch/mod
    $(warning Cannot auto-detect FS module dir; defaulting to $(FS_MODDIR))
  endif
endif

# ── libwebsockets ─────────────────────────────────────────────────────────────
LWS_CFLAGS := $(shell pkg-config --cflags libwebsockets 2>/dev/null)
LWS_LIBS   := $(shell pkg-config --libs   libwebsockets 2>/dev/null)
ifeq ($(LWS_LIBS),)
  # pkg-config not available; try direct link
  LWS_LIBS := -lwebsockets
endif

# ── Compiler flags ─────────────────────────────────────────────────────────────
CFLAGS  := -fPIC -shared -O2 -Wall -Wextra \
           $(FS_CFLAGS) $(LWS_CFLAGS) \
           -DSWITCH_API_VERSION=5 \
           -D_GNU_SOURCE

LDFLAGS := $(LWS_LIBS) -lssl -lcrypto -lpthread

SRC     := src/mod_ws_audio.c
OBJ     := $(MODULE).so

# ── Targets ───────────────────────────────────────────────────────────────────
.PHONY: all install clean

all: $(OBJ)

$(OBJ): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $@"

install: $(OBJ)
	install -m 0755 $(OBJ) $(FS_MODDIR)/
	@echo "Installed to $(FS_MODDIR)/$(OBJ)"
	@echo "Remember to add <load module=\"$(MODULE)\"/> to modules.conf.xml"
	@echo "Then: fs_cli -x 'reload mod_$(MODULE)' or restart FreeSWITCH"

clean:
	rm -f $(OBJ)