#
# Makefile for MemoryStick host controller drivers
#

ifeq ($(CONFIG_MEMSTICK_DEBUG),y)
	EXTRA_CFLAGS			+= -DDEBUG
endif

obj-$(CONFIG_MEMSTICK_TIFM_MS)		+= tifm_ms.o
obj-$(CONFIG_MEMSTICK_JMICRON_38X)	+= jmb38x_ms.o
obj-$(CONFIG_MEMSTICK_R592)		+= r592.o
obj-$(CONFIG_MEMSTICK_REALTEK_PCI)	+= rtsx_pci_ms.o
