CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -O2
SOURCES = src/server.c src/http.c src/response.c src/proxy.c
PORT ?= 8080
PROXY_PREFIX ?= /proxy
BACKEND_HOST ?= 127.0.0.1
BACKEND_PORT ?= 8081

webserver: $(SOURCES) src/http.h src/response.h src/proxy.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)

run: webserver
	@echo "http://localhost:$(PORT)  (Ctrl+C to stop)"
	@./webserver "$(PORT)" public 0.0.0.0 "$(PROXY_PREFIX)" "$(BACKEND_HOST)" "$(BACKEND_PORT)"

backend: webserver
	@./webserver "$(BACKEND_PORT)" public 127.0.0.1

check: webserver parser-check proxy-unit
	./parser-check
	./proxy-unit
	python3 checks/server_check.py ./webserver
	python3 checks/proxy_check.py ./webserver

parser-check: checks/parser_check.c src/http.c src/http.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ checks/parser_check.c src/http.c

proxy-unit: checks/proxy_unit.c src/proxy.c src/http.c src/response.c src/proxy.h src/http.h src/response.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^)

clean:
	$(RM) webserver parser-check proxy-unit

.PHONY: run backend check clean
