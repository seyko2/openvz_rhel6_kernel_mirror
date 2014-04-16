/*
 *  Copyright (C) 2006 IBM Corporation
 *
 *  Author: Serge Hallyn <serue@us.ibm.com>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation, version 2 of the
 *  License.
 *
 *  Jun 2006 - namespaces support
 *             OpenVZ, SWsoft Inc.
 *             Pavel Emelianov <xemul@openvz.org>
 */

#include <linux/module.h>
#include <linux/nsproxy.h>
#include <linux/init_task.h>
#include <linux/mnt_namespace.h>
#include <linux/utsname.h>
#include <linux/pid_namespace.h>
#include <net/net_namespace.h>
#include <linux/ipc_namespace.h>
#include <linux/proc_fs.h>
#include <linux/file.h>
#include <linux/syscalls.h>

static struct kmem_cache *nsproxy_cachep;

struct nsproxy init_nsproxy = INIT_NSPROXY(init_nsproxy);

void get_task_namespaces(struct task_struct *tsk)
{
	struct nsproxy *ns = tsk->nsproxy;
	if (ns) {
		get_nsproxy(ns);
	}
}

static inline struct nsproxy *create_nsproxy(void)
{
	struct nsproxy *nsproxy;

	nsproxy = kmem_cache_alloc(nsproxy_cachep, GFP_KERNEL);
	if (nsproxy)
		atomic_set(&nsproxy->count, 1);
	return nsproxy;
}

struct nsproxy *duplicate_nsproxy(struct nsproxy *nsproxy)
{
	struct nsproxy *ns = create_nsproxy();
	if (ns) {
		*ns = *nsproxy;
		atomic_set(&ns->count, 1);
		get_uts_ns(ns->uts_ns);
		get_ipc_ns(ns->ipc_ns);
		get_mnt_ns(ns->mnt_ns);
		get_pid_ns(ns->pid_ns);
		get_net(ns->net_ns);
	}
	return ns;
}
EXPORT_SYMBOL_GPL(duplicate_nsproxy);

/*
 * Create new nsproxy and all of its the associated namespaces.
 * Return the newly created nsproxy.  Do not attach this to the task,
 * leave it to the caller to do proper locking and attach it to task.
 */
static struct nsproxy *create_new_namespaces(unsigned long flags,
			struct task_struct *tsk, struct fs_struct *new_fs)
{
	struct nsproxy *new_nsp;
	int err;

	new_nsp = create_nsproxy();
	if (!new_nsp)
		return ERR_PTR(-ENOMEM);

	new_nsp->mnt_ns = copy_mnt_ns(flags, tsk->nsproxy->mnt_ns, new_fs);
	if (IS_ERR(new_nsp->mnt_ns)) {
		err = PTR_ERR(new_nsp->mnt_ns);
		goto out_ns;
	}

	new_nsp->uts_ns = copy_utsname(flags, tsk->nsproxy->uts_ns);
	if (IS_ERR(new_nsp->uts_ns)) {
		err = PTR_ERR(new_nsp->uts_ns);
		goto out_uts;
	}

	new_nsp->ipc_ns = copy_ipcs(flags, tsk->nsproxy->ipc_ns);
	if (IS_ERR(new_nsp->ipc_ns)) {
		err = PTR_ERR(new_nsp->ipc_ns);
		goto out_ipc;
	}

	new_nsp->pid_ns = copy_pid_ns(flags, tsk->nsproxy->pid_ns);
	if (IS_ERR(new_nsp->pid_ns)) {
		err = PTR_ERR(new_nsp->pid_ns);
		goto out_pid;
	}

	new_nsp->net_ns = copy_net_ns(flags, tsk->nsproxy->net_ns);
	if (IS_ERR(new_nsp->net_ns)) {
		err = PTR_ERR(new_nsp->net_ns);
		goto out_net;
	}

	return new_nsp;

out_net:
	if (new_nsp->pid_ns)
		put_pid_ns(new_nsp->pid_ns);
out_pid:
	if (new_nsp->ipc_ns)
		put_ipc_ns(new_nsp->ipc_ns);
out_ipc:
	if (new_nsp->uts_ns)
		put_uts_ns(new_nsp->uts_ns);
out_uts:
	if (new_nsp->mnt_ns)
		put_mnt_ns(new_nsp->mnt_ns);
out_ns:
	kmem_cache_free(nsproxy_cachep, new_nsp);
	return ERR_PTR(err);
}

/*
 * called from clone.  This now handles copy for nsproxy and all
 * namespaces therein.
 */
int copy_namespaces(unsigned long flags, struct task_struct *tsk,
		int force_admin)
{
	struct nsproxy *old_ns = tsk->nsproxy;
	struct nsproxy *new_ns;
	int err = 0;

	if (!old_ns)
		return 0;

	get_nsproxy(old_ns);

	if (!(flags & (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
				CLONE_NEWPID | CLONE_NEWNET)))
		return 0;

	if (!force_admin) {
		if (!capable(CAP_SYS_ADMIN) && !capable(CAP_VE_SYS_ADMIN)) {
			err = -EPERM;
			goto out;
		}

		if (!capable(CAP_SYS_ADMIN) &&
		    (flags & (CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWPID))) {
			err = -EPERM;
			goto out;
		}

		/*
		 * netns-vs-sysfs is deadly broken, thus new namespace
		 * (even in ve0) can bring the node down
		 */
		if (flags & CLONE_NEWNET) {
			err = -EINVAL;
			goto out;
		}
	}

	/*
	 * CLONE_NEWIPC must detach from the undolist: after switching
	 * to a new ipc namespace, the semaphore arrays from the old
	 * namespace are unreachable.  In clone parlance, CLONE_SYSVSEM
	 * means share undolist with parent, so we must forbid using
	 * it along with CLONE_NEWIPC.
	 */
	if ((flags & CLONE_NEWIPC) && (flags & CLONE_SYSVSEM)) {
		err = -EINVAL;
		goto out;
	}

	new_ns = create_new_namespaces(flags, tsk, tsk->fs);
	if (IS_ERR(new_ns)) {
		err = PTR_ERR(new_ns);
		goto out;
	}

	tsk->nsproxy = new_ns;

out:
	put_nsproxy(old_ns);
	return err;
}
EXPORT_SYMBOL(copy_namespaces);

void free_nsproxy(struct nsproxy *ns)
{
	if (ns->mnt_ns)
		put_mnt_ns(ns->mnt_ns);
	if (ns->uts_ns)
		put_uts_ns(ns->uts_ns);
	if (ns->ipc_ns)
		put_ipc_ns(ns->ipc_ns);
	if (ns->pid_ns)
		put_pid_ns(ns->pid_ns);
	put_net(ns->net_ns);
	kmem_cache_free(nsproxy_cachep, ns);
}
EXPORT_SYMBOL(free_nsproxy);

struct mnt_namespace * get_task_mnt_ns(struct task_struct *tsk)
{
	struct mnt_namespace *mnt_ns = NULL;

	task_lock(tsk);
	if (tsk->nsproxy)
		mnt_ns = tsk->nsproxy->mnt_ns;
	if (mnt_ns)
		get_mnt_ns(mnt_ns);
	task_unlock(tsk);

	return mnt_ns;
}
EXPORT_SYMBOL(get_task_mnt_ns);

/*
 * Called from unshare. Unshare all the namespaces part of nsproxy.
 * On success, returns the new nsproxy.
 */
int unshare_nsproxy_namespaces(unsigned long unshare_flags,
		struct nsproxy **new_nsp, struct fs_struct *new_fs)
{
	int err = 0;

	if (!(unshare_flags & (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
			       CLONE_NEWNET | CLONE_NEWPID)))
		return 0;

	if (!capable(CAP_SYS_ADMIN) && !capable(CAP_VE_SYS_ADMIN))
		return -EPERM;

	if (!capable(CAP_SYS_ADMIN) &&
	    (unshare_flags & (CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET |
			      CLONE_NEWPID)))
		return -EPERM;

	*new_nsp = create_new_namespaces(unshare_flags, current,
				new_fs ? new_fs : current->fs);
	if (IS_ERR(*new_nsp)) {
		err = PTR_ERR(*new_nsp);
		goto out;
	}

	err = ns_cgroup_clone(current, task_pid(current));
	if (err)
		put_nsproxy(*new_nsp);

out:
	return err;
}

void switch_task_namespaces(struct task_struct *p, struct nsproxy *new)
{
	struct nsproxy *ns;

	might_sleep();

	ns = p->nsproxy;

	rcu_assign_pointer(p->nsproxy, new);

	if (ns && atomic_dec_and_test(&ns->count)) {
		/*
		 * wait for others to get what they want from this nsproxy.
		 *
		 * cannot release this nsproxy via the call_rcu() since
		 * put_mnt_ns() will want to sleep
		 */
		synchronize_rcu();
		free_nsproxy(ns);
	}
}
EXPORT_SYMBOL_GPL(switch_task_namespaces);

void exit_task_namespaces(struct task_struct *p)
{
	switch_task_namespaces(p, NULL);
}

SYSCALL_DEFINE2(setns, int, fd, int, nstype)
{
	const struct proc_ns_operations *ops;
	struct task_struct *tsk = current;
	struct nsproxy *new_nsproxy;
	struct proc_inode *ei;
	struct file *file;
	int err;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	file = proc_ns_fget(fd);
	if (IS_ERR(file))
		return PTR_ERR(file);

	err = -EINVAL;
	ei = PROC_I(file->f_dentry->d_inode);
	ops = ei->ns_ops;
	if (nstype && (ops->type != nstype))
		goto out;

	new_nsproxy = create_new_namespaces(0, tsk, tsk->fs);
	if (IS_ERR(new_nsproxy)) {
		err = PTR_ERR(new_nsproxy);
		goto out;
	}

	err = ops->install(new_nsproxy, ei->ns);
	if (err) {
		free_nsproxy(new_nsproxy);
		goto out;
	}
	switch_task_namespaces(tsk, new_nsproxy);
out:
	fput(file);
	return err;
}

int __init nsproxy_cache_init(void)
{
	nsproxy_cachep = KMEM_CACHE(nsproxy, SLAB_PANIC);
	return 0;
}
