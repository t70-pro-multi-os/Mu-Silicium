ifneq ($(KERNELRELEASE),)
obj-m := t70-mt6789-pinctrl.o
t70-mt6789-pinctrl-y := t70-paris.o
ccflags-y += -I$(T70_PARIS_INCLUDE)
else
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
.PHONY: all
all:
	@test -s "$(CURDIR)/t70-paris-pad-data.h" || { echo "Generate the pinned MT6789 pad data first"; exit 1; }
	python3 "$(CURDIR)/check-paris-kernel.py" "$(KERNELDIR)" --paris-include "$(T70_PARIS_INCLUDE)"
	$(MAKE) -C "$(KERNELDIR)" M="$(CURDIR)" T70_PARIS_INCLUDE="$(T70_PARIS_INCLUDE)" modules
endif
