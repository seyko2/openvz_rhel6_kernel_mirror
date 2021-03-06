/*
	kmod, the new module loader (replaces kerneld)
	Kirk Petersen

	Reorganized not to be a daemon by Adam Richter, with guidance
	from Greg Zornetzer.

	Modified to avoid chroot and file sharing problems.
	Mikael Pettersson

	Limit the concurrent number of kmod modprobes to catch loops from
	"modprobe needs a service that is in a module".
	Keith Owens <kaos@ocs.com.au> December 1999

	Unblock all signals when we exec a usermode process.
	Shuu Yamaguchi <shuu@wondernetworkresources.com> December 2000

	call_usermodehelper wait flag, and remove exec_usermodehelper.
	Rusty Russell <rusty@rustcorp.com.au>  Jan 2003
*/
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <linux/unistd.h>
#include <linux/kmod.h>
#include <linux/slab.h>
#include <linux/completion.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/workqueue.h>
#include <linux/security.h>
#include <linux/mount.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/resource.h>
#include <linux/notifier.h>
#include <linux/suspend.h>
#include <linux/netfilter.h>
#include <net/net_namespace.h>
#include <asm/uaccess.h>

#include <trace/events/module.h>

extern int max_threads;

struct workqueue_struct *khelper_wq;

#define CAP_BSET	(void *)1
#define CAP_PI		(void *)2

static kernel_cap_t usermodehelper_bset = CAP_FULL_SET;
static kernel_cap_t usermodehelper_inheritable = CAP_FULL_SET;
static DEFINE_SPINLOCK(umh_sysctl_lock);

#ifdef CONFIG_MODULES

/*
	modprobe_path is set via /proc/sys.
*/
char modprobe_path[KMOD_PATH_LEN] = "/sbin/modprobe";

static void free_modprobe_argv(struct subprocess_info *info)
{
	kfree(info->argv[4]); /* check call_modprobe() */
	kfree(info->argv);
}

static int call_modprobe(char *module_name, int wait, int blacklist)
{
	static char *envp[] = {
		"HOME=/",
		"TERM=linux",
		"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
		NULL
	};

	char **argv = kmalloc(sizeof(char *[6]), GFP_KERNEL);
	if (!argv)
		goto out;

	module_name = kstrdup(module_name, GFP_KERNEL);
	if (!module_name)
		goto free_argv;

	argv[0] = modprobe_path;
	argv[1] = "-q";
	if (blacklist)
		argv[2] = "-b";
	else
		argv[2] = "-q"; /* just repeat argv[1] */
	argv[3] = "--";
	argv[4] = module_name;	/* check free_modprobe_argv() */
	argv[5] = NULL;

	return call_usermodehelper_fns(modprobe_path, argv, envp,
		wait | UMH_KILLABLE, NULL, free_modprobe_argv, NULL);
free_argv:
	kfree(argv);
out:
	return -ENOMEM;
}

/**
 * ___request_module - try to load a kernel module
 * @wait: wait (or not) for the operation to complete
 * @blacklist: say usermodehelper to ignore blacklisted modules
 * @module_name: name of requested module
 *
 * Load a module using the user mode module loader. The function returns
 * zero on success or a negative errno code on failure. Note that a
 * successful module load does not mean the module did not then unload
 * and exit on an error of its own. Callers must check that the service
 * they requested is now available not blindly invoke it.
 *
 * If module auto-loading support is disabled then this function
 * becomes a no-operation.
 */
static int ___request_module(bool wait, bool blacklist, char *module_name)
{
	unsigned int max_modprobes;
	int ret;
	static atomic_t kmod_concurrent = ATOMIC_INIT(0);
#define MAX_KMOD_CONCURRENT 50	/* Completely arbitrary value - KAO */
	static int kmod_loop_msg;

	/* Don't allow request_module() inside VE. */
	if (!ve_is_super(get_exec_env()))
		return -EPERM;

	ret = security_kernel_module_request(module_name);
	if (ret)
		return ret;

	/* If modprobe needs a service that is in a module, we get a recursive
	 * loop.  Limit the number of running kmod threads to max_threads/2 or
	 * MAX_KMOD_CONCURRENT, whichever is the smaller.  A cleaner method
	 * would be to run the parents of this process, counting how many times
	 * kmod was invoked.  That would mean accessing the internals of the
	 * process tables to get the command line, proc_pid_cmdline is static
	 * and it is not worth changing the proc code just to handle this case. 
	 * KAO.
	 *
	 * "trace the ppid" is simple, but will fail if someone's
	 * parent exits.  I think this is as good as it gets. --RR
	 */
	max_modprobes = min(max_threads/2, MAX_KMOD_CONCURRENT);
	atomic_inc(&kmod_concurrent);
	if (atomic_read(&kmod_concurrent) > max_modprobes) {
		/* We may be blaming an innocent here, but unlikely */
		if (kmod_loop_msg++ < 5)
			printk(KERN_ERR
			       "request_module: runaway loop modprobe %s\n",
			       module_name);
		atomic_dec(&kmod_concurrent);
		return -ENOMEM;
	}

	trace_module_request(module_name, wait, _RET_IP_);

	ret = call_modprobe(module_name, wait ? UMH_WAIT_PROC : UMH_WAIT_EXEC, blacklist);

	atomic_dec(&kmod_concurrent);
	return ret;
}

int __request_module(bool wait, const char *fmt, ...)
{
	char module_name[MODULE_NAME_LEN];
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vsnprintf(module_name, MODULE_NAME_LEN, fmt, args);
	va_end(args);

	if (ret >= MODULE_NAME_LEN)
		return -ENAMETOOLONG;

	return ___request_module(wait, false, module_name);
}
EXPORT_SYMBOL(__request_module);

#ifdef CONFIG_VE_IPTABLES

/* ve0 allowed modules */
static struct {
	const char *name;
	u64 perm;
} ve0_am[] = {
	{ "ip_tables",		VE_IP_IPTABLES	},
	{ "ip6_tables",		VE_IP_IPTABLES6	},
	{ "iptable_filter",	VE_IP_FILTER	},
	{ "iptable_raw",	VE_IP_IPTABLES	},
	{ "iptable_nat",	VE_IP_NAT	},
	{ "iptable_mangle",	VE_IP_MANGLE	},
	{ "ip6table_filter",	VE_IP_FILTER6	},
	{ "ip6table_mangle",	VE_IP_MANGLE6	},

	{ "xt_CONNMARK",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "xt_CONNSECMARK",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "xt_NOTRACK",		VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "xt_cluster",		VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "xt_connbytes",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "xt_connlimit",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "xt_connmark",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "xt_conntrack",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "xt_helper",		VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "xt_state",		VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "xt_socket",		VE_NF_CONNTRACK|VE_IP_CONNTRACK|
				VE_IP_IPTABLES6			},

	{ "ipt_CLUSTERIP",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ipt_CONNMARK",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ipt_CONNSECMARK",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ipt_NOTRACK",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ipt_cluster",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ipt_connbytes",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ipt_connlimit",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ipt_connmark",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ipt_conntrack",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ipt_helper",		VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ipt_state",		VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ipt_socket",		VE_NF_CONNTRACK|VE_IP_CONNTRACK|
				VE_IP_IPTABLES6			},
	{ "ipt_MASQUERADE",	VE_NF_CONNTRACK|VE_IP_CONNTRACK|
				VE_IP_NAT			},
	{ "ipt_NETMAP",		VE_NF_CONNTRACK|VE_IP_CONNTRACK|
				VE_IP_NAT			},
	{ "ipt_REDIRECT",	VE_NF_CONNTRACK|VE_IP_CONNTRACK|
				VE_IP_NAT			},

	{ "ip6t_CONNMARK",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ip6t_CONNSECMARK",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ip6t_NOTRACK",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ip6t_cluster",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ip6t_connbytes",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ip6t_connlimit",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ip6t_connmark",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ip6t_conntrack",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ip6t_helper",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ip6t_state",		VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ip6t_socket",	VE_NF_CONNTRACK|VE_IP_CONNTRACK|
				VE_IP_IPTABLES6			},
	{ "nf-nat-ipv4",	VE_NF_CONNTRACK|VE_IP_CONNTRACK|
				VE_IP_NAT			},
	{ "nf-nat",		VE_NF_CONNTRACK|VE_IP_CONNTRACK|
				VE_IP_NAT			},
	{ "nf_conntrack-2",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "nf_conntrack_ipv4",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "ip_conntrack",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "nf_conntrack-10",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
	{ "nf_conntrack_ipv6",	VE_NF_CONNTRACK|VE_IP_CONNTRACK },
};

/*
 * module_payload_allowed - check if module functionality is allowed
 * 			    to be used inside current virtual enviroment.
 *
 * Returns true if it is allowed or we're in ve0, false otherwise.
 */
bool module_payload_allowed(const char *module)
{
	u64 permitted = get_exec_env()->ipt_mask;
	int i;

	if (ve_is_super(get_exec_env()))
		return true;

	/* Look for full module name in ve0_am table */
	for (i = 0; i < ARRAY_SIZE(ve0_am); i++) {
		if (!strcmp(ve0_am[i].name, module))
			return mask_ipt_allow(permitted, ve0_am[i].perm);
	}

	/* ts_* algorithms are required for xt_string module */
	if (!strcmp("ts_bm", module) || !strcmp("ts_fsm", module) ||
	    !strcmp("ts_kmp", module))
		return mask_ipt_allow(permitted, VE_IP_IPTABLES) ||
		       mask_ipt_allow(permitted, VE_IP_IPTABLES6);

	/* The rest of xt_* modules is allowed in both ipv4 and ipv6 modes */
	if (!strncmp("xt_", module, 3))
		return mask_ipt_allow(permitted, VE_IP_IPTABLES) ||
		       mask_ipt_allow(permitted, VE_IP_IPTABLES6);

	/* The rest of ipt_* modules */
	if (!strncmp("ipt_", module, 4))
		return mask_ipt_allow(permitted, VE_IP_IPTABLES);

	/* The rest of ip6t_* modules */
	if (!strncmp("ip6t_", module, 5))
		return mask_ipt_allow(permitted, VE_IP_IPTABLES6);

	/* The rest of arpt_* modules */
	if (!strncmp("arpt_", module, 5))
		return true;

	/* The rest of ebt_* modules */
	if (!strncmp("ebt_", module, 4))
		return true;

	return false;
}
#endif /* CONFIG_VE_IPTABLES */

int ve0_request_module(const char *name,...)
{
	char module_name[MODULE_NAME_LEN];
	struct ve_struct *old;
	int blacklist, ret;
	va_list args;

	va_start(args, name);
	ret = vsnprintf(module_name, MODULE_NAME_LEN, name, args);
	va_end(args);

	if (ret >= MODULE_NAME_LEN)
		return -ENAMETOOLONG;

	/* Check that autoload is not prohobited using /proc interface */
	if (!ve_is_super(get_exec_env()) &&
	    !ve_allow_module_load)
		return -EPERM;

	/* Check that module functionality is permitted */
	if (!module_payload_allowed(module_name))
		return -EPERM;

	old = set_exec_env(get_ve0());

	/*
	 * This function may be called from ve0, where standard behaviour
	 * is not to use blacklist. So, we request blacklist reading only
	 * if we're inside CT.
	 */
	blacklist = (old != get_ve0());

	ret = ___request_module(true, blacklist, module_name);

	set_exec_env(old);

	return ret;
}
EXPORT_SYMBOL(ve0_request_module);

#endif /* CONFIG_MODULES */

/*
 * This is the task which runs the usermode application
 */
static int ____call_usermodehelper(void *data)
{
	struct subprocess_info *sub_info = data;
	struct cred *new;
	int retval;

	/* Unblock all signals */
	spin_lock_irq(&current->sighand->siglock);
	flush_signal_handlers(current, 1);
	sigemptyset(&current->blocked);
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	/* We can run anywhere, unlike our parent keventd(). */
	set_cpus_allowed_ptr(current, cpu_all_mask);

	/*
	 * Our parent is keventd, which runs with elevated scheduling priority.
	 * Avoid propagating that into the userspace child.
	 */
	set_user_nice(current, 0);

	retval = -ENOMEM;
	new = prepare_kernel_cred(current);
	if (!new)
		goto fail;

	spin_lock(&umh_sysctl_lock);
	new->cap_bset = cap_intersect(usermodehelper_bset, new->cap_bset);
	new->cap_inheritable = cap_intersect(usermodehelper_inheritable,
					     new->cap_inheritable);
	spin_unlock(&umh_sysctl_lock);

	if (sub_info->init) {
		retval = sub_info->init(sub_info, new);
		if (retval) {
			abort_creds(new);
			goto fail;
		}
	}

	commit_creds(new);

	retval = kernel_execve(sub_info->path, sub_info->argv, sub_info->envp);

	/* Exec failed? */
fail:
	sub_info->retval = retval;
	do_exit(0);
}

void call_usermodehelper_freeinfo(struct subprocess_info *info)
{
	if (info->cleanup)
		(*info->cleanup)(info);
	kfree(info);
}
EXPORT_SYMBOL(call_usermodehelper_freeinfo);

static void umh_complete(struct subprocess_info *sub_info)
{
	struct completion *comp = xchg(&sub_info->complete, NULL);
	/*
	 * See call_usermodehelper_exec(). If xchg() returns NULL
	 * we own sub_info, the UMH_KILLABLE caller has gone away.
	 */
	if (comp)
		complete(comp);
	else
		call_usermodehelper_freeinfo(sub_info);
}

/* Keventd can't block, but this (a child) can. */
static int wait_for_helper(void *data)
{
	struct subprocess_info *sub_info = data;
	pid_t pid;

	/* If SIGCLD is ignored sys_wait4 won't populate the status. */
	spin_lock_irq(&current->sighand->siglock);
	current->sighand->action[SIGCHLD-1].sa.sa_handler = SIG_DFL;
	spin_unlock_irq(&current->sighand->siglock);

	pid = kernel_thread(____call_usermodehelper, sub_info, SIGCHLD);
	if (pid < 0) {
		sub_info->retval = pid;
	} else {
		int ret = -ECHILD;
		/*
		 * Normally it is bogus to call wait4() from in-kernel because
		 * wait4() wants to write the exit code to a userspace address.
		 * But wait_for_helper() always runs as keventd, and put_user()
		 * to a kernel address works OK for kernel threads, due to their
		 * having an mm_segment_t which spans the entire address space.
		 *
		 * Thus the __user pointer cast is valid here.
		 */
		sys_wait4(pid, (int __user *)&ret, 0, NULL);

		/*
		 * If ret is 0, either ____call_usermodehelper failed and the
		 * real error code is already in sub_info->retval or
		 * sub_info->retval is 0 anyway, so don't mess with it then.
		 */
		if (ret)
			sub_info->retval = ret;
	}

	umh_complete(sub_info);
	return 0;
}

/* This is run by khelper thread  */
static void __call_usermodehelper(struct work_struct *work)
{
	struct subprocess_info *sub_info =
		container_of(work, struct subprocess_info, work);
	enum umh_wait wait = sub_info->wait;
	pid_t pid;

	if (wait != UMH_NO_WAIT)
		wait &= ~UMH_KILLABLE;

	/* CLONE_VFORK: wait until the usermode helper has execve'd
	 * successfully We need the data structures to stay around
	 * until that is done.  */
	if (wait == UMH_WAIT_PROC)
		pid = kernel_thread(wait_for_helper, sub_info,
				    CLONE_FS | CLONE_FILES | SIGCHLD);
	else
		pid = kernel_thread(____call_usermodehelper, sub_info,
				    CLONE_VFORK | SIGCHLD);

	switch (wait) {
	case UMH_NO_WAIT:
		call_usermodehelper_freeinfo(sub_info);
		break;

	case UMH_WAIT_PROC:
		if (pid > 0)
			break;
		/* FALLTHROUGH */
	case UMH_WAIT_EXEC:
		if (pid < 0)
			sub_info->retval = pid;
		umh_complete(sub_info);
	}
}

#ifdef CONFIG_PM_SLEEP
/*
 * If set, call_usermodehelper_exec() will exit immediately returning -EBUSY
 * (used for preventing user land processes from being created after the user
 * land has been frozen during a system-wide hibernation or suspend operation).
 */
static int usermodehelper_disabled;

/* Number of helpers running */
static atomic_t running_helpers = ATOMIC_INIT(0);

/*
 * Wait queue head used by usermodehelper_pm_callback() to wait for all running
 * helpers to finish.
 */
static DECLARE_WAIT_QUEUE_HEAD(running_helpers_waitq);

/*
 * Time to wait for running_helpers to become zero before the setting of
 * usermodehelper_disabled in usermodehelper_pm_callback() fails
 */
#define RUNNING_HELPERS_TIMEOUT	(5 * HZ)

/**
 * usermodehelper_disable - prevent new helpers from being started
 */
int usermodehelper_disable(void)
{
	long retval;

	usermodehelper_disabled = 1;
	smp_mb();
	/*
	 * From now on call_usermodehelper_exec() won't start any new
	 * helpers, so it is sufficient if running_helpers turns out to
	 * be zero at one point (it may be increased later, but that
	 * doesn't matter).
	 */
	retval = wait_event_timeout(running_helpers_waitq,
					atomic_read(&running_helpers) == 0,
					RUNNING_HELPERS_TIMEOUT);
	if (retval)
		return 0;

	usermodehelper_disabled = 0;
	return -EAGAIN;
}

/**
 * usermodehelper_enable - allow new helpers to be started again
 */
void usermodehelper_enable(void)
{
	usermodehelper_disabled = 0;
}

static void helper_lock(void)
{
	atomic_inc(&running_helpers);
	smp_mb__after_atomic_inc();
}

static void helper_unlock(void)
{
	if (atomic_dec_and_test(&running_helpers))
		wake_up(&running_helpers_waitq);
}
#else /* CONFIG_PM_SLEEP */
#define usermodehelper_disabled	0

static inline void helper_lock(void) {}
static inline void helper_unlock(void) {}
#endif /* CONFIG_PM_SLEEP */

/**
 * call_usermodehelper_setup - prepare to call a usermode helper
 * @path: path to usermode executable
 * @argv: arg vector for process
 * @envp: environment for process
 * @gfp_mask: gfp mask for memory allocation
 *
 * Returns either %NULL on allocation failure, or a subprocess_info
 * structure.  This should be passed to call_usermodehelper_exec to
 * exec the process and free the structure.
 */
struct subprocess_info *call_usermodehelper_setup(char *path, char **argv,
						  char **envp, gfp_t gfp_mask)
{
	struct subprocess_info *sub_info;
	sub_info = kzalloc(sizeof(struct subprocess_info), gfp_mask);
	if (!sub_info)
		goto out;

	INIT_WORK(&sub_info->work, __call_usermodehelper);
	sub_info->path = path;
	sub_info->argv = argv;
	sub_info->envp = envp;
  out:
	return sub_info;
}
EXPORT_SYMBOL(call_usermodehelper_setup);

/**
 * call_usermodehelper_setfns - set a cleanup/init function
 * @info: a subprocess_info returned by call_usermodehelper_setup
 * @cleanup: a cleanup function
 * @init: an init function
 * @data: arbitrary context sensitive data
 *
 * The init function is used to customize the helper process prior to
 * exec.  A non-zero return code causes the process to error out, exit,
 * and return the failure to the calling process
 *
 * The cleanup function is just before ethe subprocess_info is about to
 * be freed.  This can be used for freeing the argv and envp.  The
 * Function must be runnable in either a process context or the
 * context in which call_usermodehelper_exec is called.
 */
void call_usermodehelper_setfns(struct subprocess_info *info,
		    int (*init)(struct subprocess_info *info, struct cred *new),
		    void (*cleanup)(struct subprocess_info *info),
		    void *data)
{
	info->cleanup = cleanup;
	info->init = init;
	info->data = data;
}
EXPORT_SYMBOL(call_usermodehelper_setfns);

/**
 * call_usermodehelper_exec - start a usermode application
 * @sub_info: information about the subprocessa
 * @wait: wait for the application to finish and return status.
 *        when -1 don't wait at all, but you get no useful error back when
 *        the program couldn't be exec'ed. This makes it safe to call
 *        from interrupt context.
 *
 * Runs a user-space application.  The application is started
 * asynchronously if wait is not set, and runs as a child of keventd.
 * (ie. it runs with full root capabilities).
 */
int call_usermodehelper_exec_wq(struct subprocess_info *sub_info,
				enum umh_wait wait,
				struct workqueue_struct *khelper_wq)
{
	DECLARE_COMPLETION_ONSTACK(done);
	int retval = 0;

	if (!ve_is_super(get_exec_env()) &&
	    khelper_wq != get_exec_env()->khelper_wq)
		return -EPERM;

	helper_lock();
	if (sub_info->path[0] == '\0')
		goto out;

	if (!khelper_wq || usermodehelper_disabled) {
		retval = -EBUSY;
		goto out;
	}

	sub_info->complete = &done;
	sub_info->wait = wait;

	queue_work(khelper_wq, &sub_info->work);
	if (wait == UMH_NO_WAIT)	/* task has freed sub_info */
		goto unlock;

	if (wait & UMH_KILLABLE) {
		retval = wait_for_completion_killable(&done);
		if (!retval)
			goto wait_done;

		/* umh_complete() will see NULL and free sub_info */
		if (xchg(&sub_info->complete, NULL))
			goto unlock;
		/* fallthrough, umh_complete() was already called */
	}

	wait_for_completion(&done);
wait_done:
	retval = sub_info->retval;
out:
	call_usermodehelper_freeinfo(sub_info);
unlock:
	helper_unlock();
	return retval;
}
EXPORT_SYMBOL(call_usermodehelper_exec_wq);

int call_usermodehelper_exec(struct subprocess_info *sub_info,
			     enum umh_wait wait)
{
	return call_usermodehelper_exec_wq(sub_info, wait, khelper_wq);
}
EXPORT_SYMBOL(call_usermodehelper_exec);

int
call_usermodehelper_fns_wq(char *path, char **argv, char **envp,
			enum umh_wait wait,
			int (*init)(struct subprocess_info *info, struct cred *),
			void (*cleanup)(struct subprocess_info *), void *data,
			struct workqueue_struct *khelper_wq)
{
	struct subprocess_info *info;
	gfp_t gfp_mask = (wait == UMH_NO_WAIT) ? GFP_ATOMIC : GFP_KERNEL;

	info = call_usermodehelper_setup(path, argv, envp, gfp_mask);
	if (info == NULL)
		return -ENOMEM;
	call_usermodehelper_setfns(info, init, cleanup, data);
	return call_usermodehelper_exec_wq(info, wait, khelper_wq);
}
EXPORT_SYMBOL(call_usermodehelper_fns_wq);

int
call_usermodehelper_fns(char *path, char **argv, char **envp,
			enum umh_wait wait,
			int (*init)(struct subprocess_info *info, struct cred *),
			void (*cleanup)(struct subprocess_info *), void *data)
{
	return call_usermodehelper_fns_wq(path, argv, envp, wait, init,
					cleanup, data, khelper_wq);
}
EXPORT_SYMBOL(call_usermodehelper_fns);

static int proc_cap_handler(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	unsigned long cap_array[_KERNEL_CAPABILITY_U32S];
	kernel_cap_t new_cap;
	int err, i;

	if (write && (!capable(CAP_SETPCAP) ||
		      !capable(CAP_SYS_MODULE)))
		return -EPERM;

	/*
	 * convert from the global kernel_cap_t to the ulong array to print to
	 * userspace if this is a read.
	 */
	spin_lock(&umh_sysctl_lock);
	for (i = 0; i < _KERNEL_CAPABILITY_U32S; i++)  {
		if (table->data == CAP_BSET)
			cap_array[i] = usermodehelper_bset.cap[i];
		else if (table->data == CAP_PI)
			cap_array[i] = usermodehelper_inheritable.cap[i];
		else
			BUG();
	}
	spin_unlock(&umh_sysctl_lock);

	t = *table;
	t.data = &cap_array;

	/*
	 * actually read or write and array of ulongs from userspace.  Remember
	 * these are least significant 32 bits first
	 */
	err = proc_doulongvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;

	/*
	 * convert from the sysctl array of ulongs to the kernel_cap_t
	 * internal representation
	 */
	for (i = 0; i < _KERNEL_CAPABILITY_U32S; i++)
		new_cap.cap[i] = cap_array[i];

	/*
	 * Drop everything not in the new_cap (but don't add things)
	 */
	spin_lock(&umh_sysctl_lock);
	if (write) {
		if (table->data == CAP_BSET)
			usermodehelper_bset = cap_intersect(usermodehelper_bset, new_cap);
		if (table->data == CAP_PI)
			usermodehelper_inheritable = cap_intersect(usermodehelper_inheritable, new_cap);
	}
	spin_unlock(&umh_sysctl_lock);

	return 0;
}

struct ctl_table usermodehelper_table[] = {
	{
		.procname	= "bset",
		.data		= CAP_BSET,
		.maxlen		= _KERNEL_CAPABILITY_U32S * sizeof(unsigned long),
		.mode		= 0600,
		.proc_handler	= proc_cap_handler,
	},
	{
		.procname	= "inheritable",
		.data		= CAP_PI,
		.maxlen		= _KERNEL_CAPABILITY_U32S * sizeof(unsigned long),
		.mode		= 0600,
		.proc_handler	= proc_cap_handler,
	},
	{ }
};

void __init usermodehelper_init(void)
{
	khelper_wq = create_singlethread_workqueue("khelper");
	ve0.khelper_wq = khelper_wq;
	BUG_ON(!khelper_wq);
}
