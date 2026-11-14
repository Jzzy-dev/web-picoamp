# PicoAmp for the web
#
#   make            web/picoamp.wasm (with wasi-sdk's clang: WASI_SDK=/path/to/wasi-sdk)
#   make serve      the page at http://localhost:8000
#   make test       the tests in headless Chrome (Playwright)

WASI_SDK ?= $(firstword $(wildcard /opt/wasi-sdk $(HOME)/tools/wasi-sdk-25.0-x86_64-linux /home/claude/tools/wasi-sdk-25.0-x86_64-linux))
CC       := $(WASI_SDK)/bin/clang
VERSION  ?= 0.1.0

SRC     = src/web.c src/audio_web.c src/news_web.c src/gfx.c src/ui.c src/fullscreen.c src/clock.c src/weather.c \
          src/wxsvc.c src/viz.c src/eq.c src/library.c src/radiodir.c src/json.c src/config.c src/tags.c \
          src/util.c src/script.c src/cover.c src/pva_cover.c third_party/tjpgd/tjpgd.c
HEADERS = $(wildcard src/*.h)
CFLAGS  = --target=wasm32-wasip1 -mexec-model=reactor -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -Wno-missing-field-initializers -DPICOAMP_VERSION=\"$(VERSION)\" -D_WASI_EMULATED_SIGNAL -Ithird_party/tjpgd
LDFLAGS = -Wl,--export=malloc -Wl,--export=free -Wl,--initial-memory=33554432 -Wl,--max-memory=268435456 \
          -Wl,--allow-undefined-file=src/imports.txt -lm

all: web/picoamp.wasm

web/picoamp.wasm: $(SRC) $(HEADERS) src/imports.txt
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $@

src/imports.txt: src/web.h
	grep -o 'JS([a-z_]*)' src/web.h | sed 's/JS(\(.*\))/\1/' > $@

serve: all
	cd web && python3 -m http.server 8000

test: all
	python3 tests/run_web.py

clean:
	rm -f web/picoamp.wasm src/imports.txt

.PHONY: all serve test clean
