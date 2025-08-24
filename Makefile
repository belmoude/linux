# SPDX-License-Identifier: GPL-2.0
# Makefile for Confidential Computing Page Migration

# Kernel module configuration
obj-m += cc_page_migration.o
cc_page_migration-objs := cc_page_migration.o cc_pte_ops.o cc_syscall.o

# Kernel build directory
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Default target
all: module test

# Build kernel module
module:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

# Build test program
test: test_cc_migration
	
test_cc_migration: test_cc_migration.c
	gcc -o test_cc_migration test_cc_migration.c -lpthread -Wall -O2

# Clean build artifacts
clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f test_cc_migration

# Install module
install: module
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules_install
	depmod -a

# Load module
load:
	insmod cc_page_migration.ko

# Unload module
unload:
	rmmod cc_page_migration

# Run tests
run-test: test
	./test_cc_migration

# Development targets
dev-clean: clean
	rm -f *.symvers *.order *.mod.c

dev-check:
	scripts/checkpatch.pl --no-tree -f *.c *.h

# Help target
help:
	@echo "Available targets:"
	@echo "  all        - Build module and test program"
	@echo "  module     - Build kernel module only"
	@echo "  test       - Build test program only"
	@echo "  clean      - Clean build artifacts"
	@echo "  install    - Install kernel module"
	@echo "  load       - Load kernel module"
	@echo "  unload     - Unload kernel module"
	@echo "  run-test   - Build and run test program"
	@echo "  dev-clean  - Clean all development files"
	@echo "  dev-check  - Run code style checks"
	@echo "  help       - Show this help message"

.PHONY: all module test clean install load unload run-test dev-clean dev-check help
