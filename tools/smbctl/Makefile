# SPDX-License-Identifier: Apache-2.0
#
# Builds smbctl for QNX. Needs the QNX SDP 8.0 environment:
#   source ~/qnx800/qnxsdp-env.sh
CC      = qcc -Vgcc_ntox86_64
CFLAGS  = -std=gnu99 -O2 -g -Wall -Wextra

smbctl: smbctl.c
	$(CC) $(CFLAGS) -o $@ smbctl.c

clean:
	rm -f smbctl

.PHONY: clean
