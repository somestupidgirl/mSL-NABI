#
# Phase 0 spike - standalone.
#
# Not wired into the top-level Makefile: this is arm64-only and the main build
# is x86_64-only, so the two would fight over ARCH. It is also meant to be
# throwaway - once Phase 2 lands a real arm64 VMM backend, this has served its
# purpose and can go.
#
#   make        # build
#   make run    # build and run
#
OUT      := trap
SIGNCERT ?= -

CC       ?= /usr/bin/cc
CODESIGN ?= /usr/bin/codesign

CFLAGS := -arch arm64 -std=gnu11 -O0 -g \
          -Wall -Wextra -Wno-unused-parameter

all: $(OUT)

# Signed with the hypervisor entitlement, exactly as the real build does:
# hv_vm_create() returns HV_DENIED without it on Apple Silicon.
$(OUT): trap.c trap.entitlements
	$(CC) $(CFLAGS) -o $@ trap.c -framework Hypervisor
	$(CODESIGN) --force --sign $(SIGNCERT) --entitlements trap.entitlements $@

run: $(OUT)
	./$(OUT)

clean:
	rm -rf $(OUT) $(OUT).dSYM

.PHONY: all run clean
