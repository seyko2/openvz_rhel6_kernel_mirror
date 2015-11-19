#
# kbuild file for firmware/
#

# Create $(fwabs) from $(CONFIG_EXTRA_FIRMWARE_DIR) -- if it doesn't have a
# leading /, it's relative to $(srctree).
fwdir := $(subst ",,$(CONFIG_EXTRA_FIRMWARE_DIR))
fwabs := $(addprefix $(srctree)/,$(filter-out /%,$(fwdir)))$(filter /%,$(fwdir))

fw-external-y := $(subst ",,$(CONFIG_EXTRA_FIRMWARE))

# There are three cases to care about:
# 1. Building kernel with CONFIG_FIRMWARE_IN_KERNEL=y -- $(fw-shipped-y) should
#    include the firmware files to include, according to .config
# 2. 'make modules_install', which will install firmware for modules, and 
#    _also_ for the in-kernel drivers when CONFIG_FIRMWARE_IN_KERNEL=n
# 3. 'make firmware_install', which installs all firmware, unconditionally.

# For the former two cases we want $(fw-shipped-y) and $(fw-shipped-m) to be
# accurate. In the latter case it doesn't matter -- it'll use $(fw-shipped-all).
# But be aware that the config file might not be included at all.

# Without this, built-in.o won't be created when it's empty, and the
# final vmlinux link will fail.
obj-n := dummy
