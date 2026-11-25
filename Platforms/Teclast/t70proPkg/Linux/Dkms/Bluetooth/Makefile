# SPDX-License-Identifier: GPL-2.0-only
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
.PHONY: all clean
all:
	$(MAKE) -C "$(KERNELDIR)" M="$(CURDIR)" modules
clean:
	$(MAKE) -C "$(KERNELDIR)" M="$(CURDIR)" clean
