# FusionOS userspace Makefile

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
STAGING  = out

.PHONY: all clean test

all: \
	$(STAGING)/bin/detect-test \
	$(STAGING)/bin/fusion-run \
	$(STAGING)/bin/fsh \
	$(STAGING)/bin/fusion-pkg \
	$(STAGING)/bin/fusion-monitor \
	$(STAGING)/bin/fat-info \
	$(STAGING)/bin/init \
	$(STAGING)/bin/initramfs-init

$(STAGING)/bin:
	mkdir -p $(STAGING)/bin

$(STAGING)/detect.o: src/compat/detect.c src/compat/detect.h | $(STAGING)/bin
	$(CC) $(CFLAGS) -c $< -o $@

$(STAGING)/fat.o: src/fs/fat.c src/fs/fat.h | $(STAGING)/bin
	$(CC) $(CFLAGS) -c $< -o $@

$(STAGING)/bin/detect-test: src/compat/detect_test.c $(STAGING)/detect.o | $(STAGING)/bin
	$(CC) $(CFLAGS) $^ -o $@

$(STAGING)/bin/fusion-run: src/compat/fusion_run.c $(STAGING)/detect.o | $(STAGING)/bin
	$(CC) $(CFLAGS) $^ -o $@

$(STAGING)/bin/fsh: src/shell/shell.c $(STAGING)/detect.o | $(STAGING)/bin
	$(CC) $(CFLAGS) $^ -o $@

$(STAGING)/bin/fusion-pkg: src/pkg/fusion_pkg.c | $(STAGING)/bin
	$(CC) $(CFLAGS) $< -o $@

$(STAGING)/bin/fusion-monitor: src/monitor/monitor.c | $(STAGING)/bin
	$(CC) $(CFLAGS) $< -o $@

$(STAGING)/bin/fat-info: src/fs/fat_info.c $(STAGING)/fat.o | $(STAGING)/bin
	$(CC) $(CFLAGS) $^ -o $@

$(STAGING)/bin/init: src/init/init.c | $(STAGING)/bin
	$(CC) $(CFLAGS) $< -o $@

$(STAGING)/bin/initramfs-init: src/init/initramfs_init.c | $(STAGING)/bin
	$(CC) $(CFLAGS) $< -o $@

test: $(STAGING)/bin/detect-test $(STAGING)/bin/fsh
	@echo "=== Running binary detection tests ==="
	$(STAGING)/bin/detect-test $(STAGING)/bin/fsh
	$(STAGING)/bin/detect-test $(STAGING)/bin/fusion-pkg
	$(STAGING)/bin/detect-test $(STAGING)/bin/fusion-run
	@echo "=== Running shell smoke test ==="
	@echo "VER" | $(STAGING)/bin/fsh
	@echo "HELP" | $(STAGING)/bin/fsh | grep -q "DIR"
	@echo "All tests passed."

clean:
	rm -rf $(STAGING)
