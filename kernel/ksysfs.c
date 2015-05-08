/*
 * kernel/ksysfs.c - sysfs attributes in /sys/kernel, which
 * 		     are not related to any other subsystem
 *
 * Copyright (C) 2004 Kay Sievers <kay.sievers@vrfy.org>
 * 
 * This file is release under the GPLv2
 *
 */

#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kexec.h>
#include <linux/profile.h>
#include <linux/sched.h>
#include <linux/capability.h>

#define KERNEL_ATTR_RO(_name) \
static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

#define KERNEL_ATTR_RW(_name) \
static struct kobj_attribute _name##_attr = \
	__ATTR(_name, 0644, _name##_show, _name##_store)

#if defined(CONFIG_HOTPLUG)
/* current uevent sequence number */
static ssize_t uevent_seqnum_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%llu\n", (unsigned long long)ve_uevent_seqnum);
}
KERNEL_ATTR_RO(uevent_seqnum);

/* uevent helper program, used during early boo */
static ssize_t uevent_helper_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", uevent_helper);
}
static ssize_t uevent_helper_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	if (count+1 > UEVENT_HELPER_PATH_LEN)
		return -ENOENT;
	memcpy(uevent_helper, buf, count);
	uevent_helper[count] = '\0';
	if (count && uevent_helper[count-1] == '\n')
		uevent_helper[count-1] = '\0';
	return count;
}
KERNEL_ATTR_RW(uevent_helper);
#endif

#ifdef CONFIG_PROFILING
static ssize_t profiling_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", prof_on);
}
static ssize_t profiling_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int ret;

	/*
	 * We show /sys/kernel/profiling in CT for docker's sake but profiling
	 * is system wide, so we don't allow to turn it on/off and do not
	 * allow to create /proc/profile to get profiling info
	 */
	if (!ve_is_super(get_exec_env()))
		return -ENOTSUPP;
	if (prof_on)
		return -EEXIST;
	/*
	 * This eventually calls into get_option() which
	 * has a ton of callers and is not const.  It is
	 * easiest to cast it away here.
	 */
	profile_setup((char *)buf);
	ret = profile_init();
	if (ret)
		return ret;
	ret = create_proc_profile();
	if (ret)
		return ret;
	return count;
}
KERNEL_ATTR_RW(profiling);
#endif

#ifdef CONFIG_KEXEC
static ssize_t kexec_loaded_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", !!kexec_image);
}
KERNEL_ATTR_RO(kexec_loaded);

static ssize_t kexec_crash_loaded_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", !!kexec_crash_image);
}
KERNEL_ATTR_RO(kexec_crash_loaded);

static ssize_t kexec_crash_size_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%zu\n", crash_get_memory_size());
}
static ssize_t kexec_crash_size_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned long cnt;
	int ret;

	if (strict_strtoul(buf, 0, &cnt))
		return -EINVAL;

	ret = crash_shrink_memory(cnt);
	return ret < 0 ? ret : count;
}
KERNEL_ATTR_RW(kexec_crash_size);

static ssize_t vmcoreinfo_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lx %x\n",
		       paddr_vmcoreinfo_note(),
		       (unsigned int)vmcoreinfo_max_size);
}
KERNEL_ATTR_RO(vmcoreinfo);

#endif /* CONFIG_KEXEC */

/* whether file capabilities are enabled */
static ssize_t fscaps_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", file_caps_enabled);
}
KERNEL_ATTR_RO(fscaps);

/*
 * Make /sys/kernel/notes give the raw contents of our kernel .notes section.
 */
extern const void __start_notes __attribute__((weak));
extern const void __stop_notes __attribute__((weak));
#define	notes_size (&__stop_notes - &__start_notes)

static ssize_t notes_read(struct file *filp, struct kobject *kobj,
			  struct bin_attribute *bin_attr,
			  char *buf, loff_t off, size_t count)
{
	memcpy(buf, &__start_notes + off, count);
	return count;
}

static struct bin_attribute notes_attr = {
	.attr = {
		.name = "notes",
		.mode = S_IRUGO,
	},
	.read = &notes_read,
};

struct kobject *kernel_kobj;
EXPORT_SYMBOL_GPL(kernel_kobj);

static struct attribute * kernel_attrs[] = {
	&fscaps_attr.attr,
#if defined(CONFIG_HOTPLUG)
	&uevent_seqnum_attr.attr,
	&uevent_helper_attr.attr,
#endif
#ifdef CONFIG_PROFILING
	&profiling_attr.attr,
#endif
#ifdef CONFIG_KEXEC
	&kexec_loaded_attr.attr,
	&kexec_crash_loaded_attr.attr,
	&kexec_crash_size_attr.attr,
	&vmcoreinfo_attr.attr,
#endif
	NULL
};

static struct attribute * kernel_ve_attrs[] = {
	&fscaps_attr.attr,
#if defined(CONFIG_HOTPLUG)
	&uevent_seqnum_attr.attr,
#endif
#ifdef CONFIG_PROFILING
	&profiling_attr.attr,
#endif
	NULL
};

static struct attribute_group kernel_attr_group = {
	.attrs = kernel_attrs,
};

static struct attribute_group kernel_ve_attr_group = {
	.attrs = kernel_ve_attrs,
};

int ksysfs_init_ve(struct ve_struct *ve, struct kobject **kernel_obj)
{
	struct attribute_group *k_grp;
	int err;

	if (!ve || ve_is_super(ve))
		k_grp = &kernel_attr_group;
	else
		k_grp = &kernel_ve_attr_group;

	*kernel_obj = kobject_create_and_add("kernel", NULL);
	if (!*kernel_obj)
		return -ENOMEM;

	err = sysfs_create_group(*kernel_obj, k_grp);

	if (err) {
		kobject_put(*kernel_obj);
		*kernel_obj = NULL;
	}

	return err;
}
EXPORT_SYMBOL_GPL(ksysfs_init_ve);

void ksysfs_fini_ve(struct ve_struct *ve, struct kobject **kernel_obj)
{
	sysfs_remove_group(*kernel_obj, &kernel_ve_attr_group);
	kobject_put(*kernel_obj);
	*kernel_obj = NULL;
}
EXPORT_SYMBOL_GPL(ksysfs_fini_ve);

static int __init ksysfs_init(void)
{
	int error = ksysfs_init_ve(NULL, &kernel_kobj);

	if (error)
		return error;

	if (notes_size > 0) {
		notes_attr.size = notes_size;
		error = sysfs_create_bin_file(kernel_kobj, &notes_attr);
		if (error)
			goto group_exit;
	}

	/* create the /sys/kernel/uids/ directory */
	error = uids_sysfs_init();
	if (error)
		goto notes_exit;

	return 0;

notes_exit:
	if (notes_size > 0)
		sysfs_remove_bin_file(kernel_kobj, &notes_attr);
group_exit:
	ksysfs_fini_ve(NULL, &kernel_kobj);
	return error;
}

core_initcall(ksysfs_init);
