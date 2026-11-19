# SPDX-License-Identifier: Apache-2.0
#
# Builds the QNX driver. Needs the QNX SDP 8.0 environment:
#   source ~/qnx800/qnxsdp-env.sh
CC      = qcc -Vgcc_ntox86_64
CFLAGS  = -std=gnu99 -O2 -g -Wall -Wextra -I../core
LIBS    = -li2c-master -lsecpol -lpci
SRCS    = i2c-ichsmb.c ../core/smb_core.c ../core/smb_map.c

i2c-ichsmb: $(SRCS) ../core/smb_core.h ../core/ichsmb_regs.h i2c-ichsmb.use
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS)
	usemsg $@ i2c-ichsmb.use

clean:
	rm -f i2c-ichsmb

.PHONY: clean
