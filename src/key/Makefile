# SPDX-License-Identifier: GPL-2.0-or-later
# Makefile for SM2 sign/verify test (user-space and kernel module)

.PHONY: all help genkey clean user-test kern-test check-openssl

# Default target: show help
default: help

# Verbosity control: set V=1 to show full commands
Q = @
ifeq ($(V),1)
	Q =
endif

# Build settings
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -std=c99 -fPIE
LDFLAGS ?= -pie
LIBS    = -lssl -lcrypto

# File names
TARGET     = user_sm2_test
SRC        = $(TARGET).c
HEADER     = sm2_signature.h
PRIVKEY    = sm2_private_key.pem
PUBKEY     = sm2_public_key.pem
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

# Show usage
help:
	@echo "Usage:"
	@echo "  make genkey         # Generate SM2 key pair"
	@echo "  make user-test      # Build and run user-space test"
	@echo "  make kern-test      # Build and load kernel module test"
	@echo "  make clean          # Remove all generated files"

# Ensure OpenSSL supports SM2 (OpenSSL >= 3.0 required)
check-openssl:
	@openssl version
	$(Q)if ! openssl list -public-key-algorithms 2>/dev/null | grep -qi 'sm2'; then \
		echo "ERROR: OpenSSL does not support SM2. Use OpenSSL >= 3.0 with SM2 enabled."; \
		exit 1; \
	fi
	$(Q)echo "OK: OpenSSL supports SM2."

# Generate SM2 private/public key pair
genkey: check-openssl
	$(Q)echo "Generating SM2 key pair..."
	$(Q)openssl genpkey -algorithm SM2 -out $(PRIVKEY)
	$(Q)openssl pkey -in $(PRIVKEY) -pubout -out $(PUBKEY)
	$(Q)md5sum $(PRIVKEY) $(PUBKEY)
	$(Q)echo "Private key ASN.1 structure (first 10 lines):"
	$(Q)openssl asn1parse -in $(PRIVKEY) | head -n 10

$(PRIVKEY) $(PUBKEY): genkey

# Build user-space test program
$(TARGET): $(SRC)
	$(Q)echo "  CC $<"
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

# Run user-space test and generate header with signature
user-test: $(TARGET) $(PRIVKEY) $(PUBKEY)
	$(Q)echo "Running user-space SM2 sign/verify test..."
	$(Q)./$(TARGET)

# Build kernel module
kern_sm2_test.ko: kern_sm2_test.c
	$(Q)echo "Building kernel module..."
	$(Q)$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

$(HEADER): user-test

# Load kernel module and show recent dmesg
kern-test: $(HEADER) kern_sm2_test.ko
	@rmmod kern_sm2_test 2>/dev/null || true
	dmesg -c > /dev/null
	insmod kern_sm2_test.ko
	dmesg

all-test: user-test kern-test

# Clean all generated files
clean:
	$(Q)rm -f $(PRIVKEY) $(PUBKEY) $(TARGET) $(HEADER)
	$(Q)rm -f *.o *.ko *.mod.c *.mod.o .*.cmd *.order *.symvers *.mod
	$(Q)rm -rf .tmp_versions Modules.symvers Module.markers

