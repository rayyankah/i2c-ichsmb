# SPDX-License-Identifier: Apache-2.0
#
# Builds the demo programs for QNX. Needs the QNX SDP 8.0 environment:
#   source ~/qnx800/qnxsdp-env.sh
CC      = qcc -Vgcc_ntox86_64
CFLAGS  = -std=gnu99 -O2 -g -Wall -Wextra -I../tools/lib
LIB     = ../tools/lib/i2cdev.c ../tools/lib/i2cdev.h

all: thermd/thermd hog/hog supervisor/smb-supervisor

thermd/thermd: thermd/thermd.c thermd/thermd.use $(LIB)
	$(CC) $(CFLAGS) -o $@ thermd/thermd.c ../tools/lib/i2cdev.c
	usemsg $@ thermd/thermd.use

hog/hog: hog/hog.c
	$(CC) $(CFLAGS) -o $@ hog/hog.c

supervisor/smb-supervisor: supervisor/smb-supervisor.c
	$(CC) $(CFLAGS) -o $@ supervisor/smb-supervisor.c

clean:
	rm -f thermd/thermd hog/hog supervisor/smb-supervisor

.PHONY: all clean
