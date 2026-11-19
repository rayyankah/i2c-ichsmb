# SPDX-License-Identifier: Apache-2.0
#
# Host unit tests for driver/core. No QNX kit needed: any C99 compiler.
#   Linux / WSL:  make            (or: make CC=clang)
#   Windows:      mingw32-make
CFLAGS  ?= -std=c99 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror
CORE     = ../../driver/core
SRCS     = test_core.c sim.c $(CORE)/smb_core.c $(CORE)/smb_map.c
HDRS     = sim.h $(CORE)/smb_core.h $(CORE)/ichsmb_regs.h

ifeq ($(OS),Windows_NT)
EXE = .exe
CC  = gcc
endif

test: test_core$(EXE)
	./test_core$(EXE)

test_core$(EXE): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -I$(CORE) -o $@ $(SRCS)

clean:
	rm -f test_core test_core.exe

.PHONY: test clean
