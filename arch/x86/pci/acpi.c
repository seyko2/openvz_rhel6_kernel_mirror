#include <linux/pci.h>
#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/dmi.h>
#include <asm/numa.h>
#include <asm/pci_x86.h>

struct pci_root_info {
	struct acpi_device *bridge;
	char name[16];
	unsigned int res_num;
	struct resource *res;
	resource_size_t *res_offset;
	struct pci_sysdata sd;
};

static bool pci_use_crs = true;

static int __init set_use_crs(const struct dmi_system_id *id)
{
	pci_use_crs = true;
	return 0;
}

static const struct dmi_system_id pci_use_crs_table[] __initconst = {
	/* http://bugzilla.kernel.org/show_bug.cgi?id=14183 */
	{
		.callback = set_use_crs,
		.ident = "IBM System x3800",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "IBM"),
			DMI_MATCH(DMI_PRODUCT_NAME, "x3800"),
		},
	},
	/* https://bugzilla.kernel.org/show_bug.cgi?id=16007 */
	/* 2006 AMD HT/VIA system with two host bridges */
        {
		.callback = set_use_crs,
		.ident = "ASRock ALiveSATA2-GLAN",
		.matches = {
			DMI_MATCH(DMI_PRODUCT_NAME, "ALiveSATA2-GLAN"),
                },
        },
	{}
};

void __init pci_acpi_crs_quirks(void)
{
	int year;

	if (dmi_get_date(DMI_BIOS_DATE, &year, NULL, NULL) && year < 2008)
		pci_use_crs = false;

	dmi_check_system(pci_use_crs_table);

	/*
	 * If the user specifies "pci=use_crs" or "pci=nocrs" explicitly, that
	 * takes precedence over anything we figured out above.
	 */
	if (pci_probe & PCI_ROOT_NO_CRS)
		pci_use_crs = false;
	else if (pci_probe & PCI_USE__CRS)
		pci_use_crs = true;

	printk(KERN_INFO "PCI: %s host bridge windows from ACPI; "
	       "if necessary, use \"pci=%s\" and report a bug\n",
	       pci_use_crs ? "Using" : "Ignoring",
	       pci_use_crs ? "nocrs" : "use_crs");
}

static acpi_status
resource_to_addr(struct acpi_resource *resource,
			struct acpi_resource_address64 *addr)
{
	acpi_status status;
	struct acpi_resource_memory24 *memory24;
	struct acpi_resource_memory32 *memory32;
	struct acpi_resource_fixed_memory32 *fixed_memory32;

	memset(addr, 0, sizeof(*addr));
	switch (resource->type) {
	case ACPI_RESOURCE_TYPE_MEMORY24:
		memory24 = &resource->data.memory24;
		addr->resource_type = ACPI_MEMORY_RANGE;
		addr->minimum = memory24->minimum;
		addr->address_length = memory24->address_length;
		addr->maximum = addr->minimum + addr->address_length - 1;
		return AE_OK;
	case ACPI_RESOURCE_TYPE_MEMORY32:
		memory32 = &resource->data.memory32;
		addr->resource_type = ACPI_MEMORY_RANGE;
		addr->minimum = memory32->minimum;
		addr->address_length = memory32->address_length;
		addr->maximum = addr->minimum + addr->address_length - 1;
		return AE_OK;
	case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
		fixed_memory32 = &resource->data.fixed_memory32;
		addr->resource_type = ACPI_MEMORY_RANGE;
		addr->minimum = fixed_memory32->address;
		addr->address_length = fixed_memory32->address_length;
		addr->maximum = addr->minimum + addr->address_length - 1;
		return AE_OK;
	case ACPI_RESOURCE_TYPE_ADDRESS16:
	case ACPI_RESOURCE_TYPE_ADDRESS32:
	case ACPI_RESOURCE_TYPE_ADDRESS64:
		status = acpi_resource_to_address64(resource, addr);
		if (ACPI_SUCCESS(status) &&
		    (addr->resource_type == ACPI_MEMORY_RANGE ||
		    addr->resource_type == ACPI_IO_RANGE) &&
		    addr->address_length > 0) {
			return AE_OK;
		}
		break;
	}
	return AE_ERROR;
}

static acpi_status
count_resource(struct acpi_resource *acpi_res, void *data)
{
	struct pci_root_info *info = data;
	struct acpi_resource_address64 addr;
	acpi_status status;

	status = resource_to_addr(acpi_res, &addr);
	if (ACPI_SUCCESS(status))
		info->res_num++;
	return AE_OK;
}

static acpi_status
setup_resource(struct acpi_resource *acpi_res, void *data)
{
	struct pci_root_info *info = data;
	struct resource *res;
	struct acpi_resource_address64 addr;
	acpi_status status;
	unsigned long flags;
	u64 start, orig_end, end;

	status = resource_to_addr(acpi_res, &addr);
	if (!ACPI_SUCCESS(status))
		return AE_OK;

	if (addr.resource_type == ACPI_MEMORY_RANGE) {
		flags = IORESOURCE_MEM;
		if (addr.info.mem.caching == ACPI_PREFETCHABLE_MEMORY)
			flags |= IORESOURCE_PREFETCH;
	} else if (addr.resource_type == ACPI_IO_RANGE) {
		flags = IORESOURCE_IO;
	} else
		return AE_OK;

	start = addr.minimum + addr.translation_offset;
	orig_end = end = addr.maximum + addr.translation_offset;

	/* Exclude non-addressable range or non-addressable portion of range */
	end = min(end, (u64)iomem_resource.end);
	if (end <= start) {
		dev_info(&info->bridge->dev,
			"host bridge window [%#llx-%#llx] "
			"(ignored, not CPU addressable)\n", start, orig_end);
		return AE_OK;
	} else if (orig_end != end) {
		dev_info(&info->bridge->dev,
			"host bridge window [%#llx-%#llx] "
			"([%#llx-%#llx] ignored, not CPU addressable)\n", 
			start, orig_end, end + 1, orig_end);
	}

	res = &info->res[info->res_num];
	res->name = info->name;
	res->flags = flags;
	res->start = start;
	res->end = end;
	info->res_offset[info->res_num] = addr.translation_offset;

	if (!pci_use_crs) {
		dev_printk(KERN_DEBUG, &info->bridge->dev,
			   "host bridge window %pR (ignored)\n", res);
		return AE_OK;
	}

	info->res_num++;
	if (addr.translation_offset)
		dev_info(&info->bridge->dev, "host bridge window %pR "
			 "(PCI address [%#llx-%#llx])\n",
			 res, res->start - addr.translation_offset,
			 res->end - addr.translation_offset);
	else
		dev_info(&info->bridge->dev, "host bridge window %pR\n", res);

	return AE_OK;
}

static bool resource_contains(struct resource *res, resource_size_t point)
{
	if (res->start <= point && point <= res->end)
		return true;
	return false;
}

static void coalesce_windows(struct pci_root_info *info, int type)
{
	int i, j;
	struct resource *res1, *res2;

	for (i = 0; i < info->res_num; i++) {
		res1 = &info->res[i];
		if (!(res1->flags & type))
			continue;

		for (j = i + 1; j < info->res_num; j++) {
			res2 = &info->res[j];
			if (!(res2->flags & type))
				continue;

			/*
			 * I don't like throwing away windows because then
			 * our resources no longer match the ACPI _CRS, but
			 * the kernel resource tree doesn't allow overlaps.
			 */
			if (resource_contains(res1, res2->start) ||
			    resource_contains(res1, res2->end) ||
			    resource_contains(res2, res1->start) ||
			    resource_contains(res2, res1->end)) {
				res1->start = min(res1->start, res2->start);
				res1->end = max(res1->end, res2->end);
				dev_info(&info->bridge->dev,
					 "host bridge window expanded to %pR; %pR ignored\n",
					 res1, res2);
				res2->flags = 0;
			}
		}
	}
}

static void add_resources(struct pci_root_info *info,
			  struct list_head *resources)
{
	int i;
	struct resource *res, *root, *conflict;

	coalesce_windows(info, IORESOURCE_MEM);
	coalesce_windows(info, IORESOURCE_IO);

	for (i = 0; i < info->res_num; i++) {
		res = &info->res[i];

		if (res->flags & IORESOURCE_MEM)
			root = &iomem_resource;
		else if (res->flags & IORESOURCE_IO)
			root = &ioport_resource;
		else
			continue;

		conflict = insert_resource_conflict(root, res);
		if (conflict)
			dev_info(&info->bridge->dev,
				 "ignoring host bridge window %pR (conflicts with %s %pR)\n",
				 res, conflict->name, conflict);
		else
			pci_add_resource_offset(resources, res,
					info->res_offset[i]);
	}
}

static void free_pci_root_info_res(struct pci_root_info *info)
{
	kfree(info->res);
	info->res = NULL;
	kfree(info->res_offset);
	info->res_offset = NULL;
	info->res_num = 0;
}

static void __release_pci_root_info(struct pci_root_info *info)
{
	int i;
	struct resource *res;

	for (i = 0; i < info->res_num; i++) {
		res = &info->res[i];

		if (!res->parent)
			continue;

		if (!(res->flags & (IORESOURCE_MEM | IORESOURCE_IO)))
			continue;

		release_resource(res);
	}

	free_pci_root_info_res(info);

	kfree(info);
}
static void release_pci_root_info(struct pci_host_bridge *bridge)
{
	struct pci_root_info *info = bridge->release_data;

	__release_pci_root_info(info);
}

static void
probe_pci_root_info(struct pci_root_info *info, struct acpi_device *device,
		    int busnum, int domain)
{
	size_t size;

	sprintf(info->name, "PCI Bus %04x:%02x", domain, busnum);
	info->bridge = device;

	info->res_num = 0;
	acpi_walk_resources(device->handle, METHOD_NAME__CRS, count_resource,
				info);
	if (!info->res_num)
		return;

	size = sizeof(*info->res) * info->res_num;
	info->res = kzalloc(size, GFP_KERNEL);
	if (!info->res) {
		info->res_num = 0;
		return;
	}

	size = sizeof(*info->res_offset) * info->res_num;
	info->res_num = 0;
	info->res_offset = kzalloc(size, GFP_KERNEL);
	if (!info->res_offset) {
		kfree(info->res);
		info->res = NULL;
		return;
	}

	acpi_walk_resources(device->handle, METHOD_NAME__CRS, setup_resource,
				info);
}

struct pci_bus * __devinit pci_acpi_scan_root(struct acpi_pci_root *root)
{
	struct acpi_device *device = root->device;
	struct pci_root_info *info = NULL;
	int domain = root->segment;
	int busnum = root->secondary.start;
	LIST_HEAD(resources);
	struct pci_bus *bus;
	struct pci_sysdata *sd;
	int node;

	if (domain && !pci_domains_supported) {
		printk(KERN_WARNING "pci_bus %04x:%02x: "
		       "ignored (multiple domains not supported)\n",
		       domain, busnum);
		return NULL;
	}

	node = acpi_get_node(device->handle);
	if (node == NUMA_NO_NODE) {
		node = x86_pci_root_bus_node(busnum);
		if (node != 0 && node != NUMA_NO_NODE)
			dev_info(&device->dev, FW_BUG "no _PXM; falling back to node %d from hardware (may be inconsistent with ACPI node numbers)\n",
				node);
	}

	if (node != NUMA_NO_NODE && !node_online(node))
		node = NUMA_NO_NODE;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		printk(KERN_WARNING "pci_bus %04x:%02x: "
		       "ignored (out of memory)\n", domain, busnum);
		return NULL;
	}

	sd = &info->sd;
	sd->domain = domain;
	sd->node = node;
	/*
	 * Maybe the desired pci bus has been already scanned. In such case
	 * it is unnecessary to scan the pci bus with the given domain,busnum.
	 */
	bus = pci_find_bus(domain, busnum);
	if (bus) {
		/*
		 * If the desired bus exits, the content of bus->sysdata will
		 * be replaced by sd.
		 */
		memcpy(bus->sysdata, sd, sizeof(*sd));
		kfree(info);
	} else {
		probe_pci_root_info(info, device, busnum, domain);

		/*
		 * _CRS with no apertures is normal, so only fall back to
		 * defaults or native bridge info if we're ignoring _CRS.
		 */
		if (pci_use_crs)
			add_resources(info, &resources);
		else {
			free_pci_root_info_res(info);
			x86_pci_root_bus_resources(busnum, &resources);
		}

		bus = pci_create_root_bus(NULL, busnum, &pci_root_ops, sd,
					  &resources);
		if (bus) {
			pci_scan_child_bus(bus);
			bus->subordinate = root->secondary.end;
			pci_set_host_bridge_release(
				to_pci_host_bridge(bus->bridge),
				release_pci_root_info, info);
		} else {
			pci_free_resource_list(&resources);
			__release_pci_root_info(info);
		}
	}

	/* After the PCI-E bus has been walked and all devices discovered,
	 * configure any settings of the fabric that might be necessary.
	 */
	if (bus) {
		struct pci_bus *child;
		list_for_each_entry(child, &bus->children, node) {
			struct pci_dev *self = child->self;
			if (!self)
				continue;

			pcie_bus_configure_settings(child, rh_get_mpss(self));
		}
	}

	if (bus && node != NUMA_NO_NODE)
		dev_printk(KERN_DEBUG, &bus->dev, "on NUMA node %d\n", node);

	return bus;
}

int __init pci_acpi_init(void)
{
	struct pci_dev *dev = NULL;

	if (pcibios_scanned)
		return 0;

	if (acpi_noirq)
		return 0;

	printk(KERN_INFO "PCI: Using ACPI for IRQ routing\n");
	acpi_irq_penalty_init();
	pcibios_scanned++;
	pcibios_enable_irq = acpi_pci_irq_enable;
	pcibios_disable_irq = acpi_pci_irq_disable;

	if (pci_routeirq) {
		/*
		 * PCI IRQ routing is set up by pci_enable_device(), but we
		 * also do it here in case there are still broken drivers that
		 * don't use pci_enable_device().
		 */
		printk(KERN_INFO "PCI: Routing PCI interrupts for all devices because \"pci=routeirq\" specified\n");
		for_each_pci_dev(dev)
			acpi_pci_irq_enable(dev);
	}

	return 0;
}
