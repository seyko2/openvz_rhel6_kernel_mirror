/*
 *  Copyright (C) 2004 IBM Corporation
 *
 *  Author: Serge Hallyn <serue@us.ibm.com>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation, version 2 of the
 *  License.
 */

#include <linux/module.h>
#include <linux/uts.h>
#include <linux/utsname.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>

static struct uts_namespace *create_uts_ns(void)
{
	struct uts_namespace *uts_ns;

	uts_ns = kmalloc(sizeof(struct uts_namespace), GFP_KERNEL);
	if (uts_ns) {
#ifdef CONFIG_X86
#ifdef CONFIG_X86_64
		memset(&uts_ns->vdso, 0, sizeof(uts_ns->vdso));
#endif
#if defined(CONFIG_X86_32) || defined(CONFIG_IA32_EMULATION)
		memset(&uts_ns->vdso32, 0, sizeof(uts_ns->vdso32));
#endif
#endif
		kref_init(&uts_ns->kref);
	}
	return uts_ns;
}

/*
 * Clone a new ns copying an original utsname, setting refcount to 1
 * @old_ns: namespace to clone
 * Return NULL on error (failure to kmalloc), new ns otherwise
 */
static struct uts_namespace *clone_uts_ns(struct uts_namespace *old_ns)
{
	struct uts_namespace *ns;
	int err;

	ns = create_uts_ns();
	if (!ns)
		return ERR_PTR(-ENOMEM);

	err = proc_alloc_inum(&ns->proc_inum);
	if (err) {
		kfree(ns);
		return ERR_PTR(err);
	}

	down_read(&uts_sem);
	memcpy(&ns->name, &old_ns->name, sizeof(ns->name));
	up_read(&uts_sem);
	return ns;
}

/*
 * Copy task tsk's utsname namespace, or clone it if flags
 * specifies CLONE_NEWUTS.  In latter case, changes to the
 * utsname of this process won't be seen by parent, and vice
 * versa.
 */
struct uts_namespace *copy_utsname(unsigned long flags, struct uts_namespace *old_ns)
{
	struct uts_namespace *new_ns;

	BUG_ON(!old_ns);
	get_uts_ns(old_ns);

	if (!(flags & CLONE_NEWUTS))
		return old_ns;

	new_ns = clone_uts_ns(old_ns);

	put_uts_ns(old_ns);
	return new_ns;
}

void free_uts_ns(struct kref *kref)
{
	struct uts_namespace *ns;

	ns = container_of(kref, struct uts_namespace, kref);
	proc_free_inum(ns->proc_inum);
#ifdef CONFIG_X86
#ifdef CONFIG_X86_64
	if (ns->vdso.pages) {
		int i;

		vunmap(ns->vdso.addr);
		for (i = 0; i < ns->vdso.nr_pages; i++)
			put_page(ns->vdso.pages[i]);
		kfree(ns->vdso.pages);
	}
#endif
#if defined(CONFIG_X86_32) || defined(CONFIG_IA32_EMULATION)
	if (ns->vdso32.pages) {
		int i;

		for (i = 0; i < ns->vdso32.nr_pages; i++)
			put_page(ns->vdso32.pages[i]);
		kfree(ns->vdso32.pages);
	}
#endif
#endif
	kfree(ns);
}

static void *utsns_get(struct task_struct *task)
{
	struct uts_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	rcu_read_lock();
	nsproxy = task_nsproxy(task);
	if (nsproxy) {
		ns = nsproxy->uts_ns;
		get_uts_ns(ns);
	}
	rcu_read_unlock();

	return ns;
}

static void utsns_put(void *ns)
{
	put_uts_ns(ns);
}

static int utsns_install(struct nsproxy *nsproxy, void *ns)
{
	get_uts_ns(ns);
	put_uts_ns(nsproxy->uts_ns);
	nsproxy->uts_ns = ns;
	return 0;
}

static unsigned int utsns_inum(void *vp)
{
	struct uts_namespace *ns = vp;

	return ns->proc_inum;
}

const struct proc_ns_operations utsns_operations = {
	.name		= "uts",
	.type		= CLONE_NEWUTS,
	.get		= utsns_get,
	.put		= utsns_put,
	.install	= utsns_install,
	.inum		= utsns_inum,
};
