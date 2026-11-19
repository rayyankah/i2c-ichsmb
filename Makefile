# SPDX-License-Identifier: Apache-2.0
#
#   make host-test   host unit tests of driver/core (any C compiler)
#   make qnx         build the driver, smbctl and the test client
#                    (needs: source ~/qnx800/qnxsdp-env.sh)
#   make test        on-target tests in QEMU, q35 machine (ICH9)
#   make test-pc     on-target tests in QEMU, pc machine (PIIX4)
#
# Test output is saved under docs/results/.

.PHONY: all host-test qnx test test-pc clean

all: host-test qnx

host-test:
	$(MAKE) -C tests/host

qnx:
	$(MAKE) -C driver/qnx
	$(MAKE) -C tools/smbctl
	$(MAKE) -C tests/target

test: qnx
	@mkdir -p docs/results
	python3 tests/run_tests.py --machine q35 | tee docs/results/tests-q35.txt; exit $${PIPESTATUS[0]}

test-pc: qnx
	@mkdir -p docs/results
	python3 tests/run_tests.py --machine pc | tee docs/results/tests-pc.txt; exit $${PIPESTATUS[0]}

clean:
	$(MAKE) -C tests/host clean
	$(MAKE) -C driver/qnx clean
	$(MAKE) -C tools/smbctl clean
	$(MAKE) -C tests/target clean
