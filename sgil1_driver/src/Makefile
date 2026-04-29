# Makefile for the SGI L1 USB driver loadable module (Linux 5.15+)
#
# Out-of-tree module build. Uses the modern M= variable; SUBDIRS= was
# removed in 5.3.

ifneq ($(KERNELRELEASE),)
# ===========================================================================
# Inner make: invoked by Kbuild from inside the kernel source tree.
# ===========================================================================

obj-m := sgil1.o

else
# ===========================================================================
# Outer make: invoked from a normal shell.
# ===========================================================================

KERNEL_VERSION ?= $(shell uname -r)
KDIR           := /lib/modules/$(KERNEL_VERSION)/build
PWD            := $(shell pwd)

.PHONY: all modules clean install uninstall help

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: modules
	@if [ "`id -u`" != "0" ]; then \
		echo "ERROR: 'make install' must be run as root."; exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a $(KERNEL_VERSION)
	@echo
	@echo "Module installed.  Load with: modprobe sgil1"
	@echo "(udev will create /dev/sgil1_N and /dev/sgil1_cs nodes)"
	@if [ -f 99-sgil1.rules ]; then \
		install -m 0644 99-sgil1.rules /etc/udev/rules.d/99-sgil1.rules ; \
		udevadm control --reload-rules ; \
		echo "Installed udev rules to /etc/udev/rules.d/99-sgil1.rules" ; \
	fi

uninstall:
	@if [ "`id -u`" != "0" ]; then \
		echo "ERROR: 'make uninstall' must be run as root."; exit 1; \
	fi
	-rmmod sgil1 2>/dev/null
	rm -f /lib/modules/$(KERNEL_VERSION)/extra/sgil1.ko
	rm -f /etc/udev/rules.d/99-sgil1.rules
	depmod -a $(KERNEL_VERSION)
	-udevadm control --reload-rules

help:
	@echo "Targets:"
	@echo "  all       - build the module (default)"
	@echo "  modules   - same as 'all'"
	@echo "  clean     - remove build artifacts"
	@echo "  install   - install module + udev rules (run as root)"
	@echo "  uninstall - remove module + udev rules (run as root)"

endif
