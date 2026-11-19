# SPDX-License-Identifier: Apache-2.0
#
# Builds smbctl for QNX. Needs the QNX SDP 8.0 environment:
#   source ~/qnx800/qnxsdp-env.sh
CC      = qcc -Vgcc_ntox86_64
CFLAGS  = -std=gnu99 -O2 -g -Wall -Wextra -I../lib
SRCS    = smbctl.c ../lib/i2cdev.c

smbctl: $(SRCS) ../lib/i2cdev.h smbctl.use
	$(CC) $(CFLAGS) -o $@ $(SRCS)
	usemsg $@ smbctl.use

clean:
	rm -f smbctl

.PHONY: clean
