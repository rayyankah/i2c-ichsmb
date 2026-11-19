# SPDX-License-Identifier: Apache-2.0
# Builds the on-target test client. Needs: source ~/qnx800/qnxsdp-env.sh
CC      = qcc -Vgcc_ntox86_64
CFLAGS  = -std=gnu99 -O2 -g -Wall -Wextra

t_xfer: t_xfer.c
	$(CC) $(CFLAGS) -o $@ t_xfer.c

clean:
	rm -f t_xfer

.PHONY: clean
