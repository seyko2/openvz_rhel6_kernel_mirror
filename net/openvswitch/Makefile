#
# Makefile for Open vSwitch.
#

obj-$(CONFIG_OPENVSWITCH) += openvswitch.o
obj-$(CONFIG_OVS_BRCOMPAT) += brcompat.o

openvswitch-y := \
	actions.o \
	datapath.o \
	dp_notify.o \
	flow.o \
	flow_netlink.o \
	flow_table.o \
	vport.o \
	vport-gre.o \
	vport-internal_dev.o \
	vport-netdev.o

ifneq ($(CONFIG_OPENVSWITCH_VXLAN),)
openvswitch-y += vport-vxlan.o
endif
