
#include <linux/wait.h>
#include <linux/backing-dev.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/writeback.h>
#include <linux/device.h>
#include <trace/events/writeback.h>

void default_unplug_io_fn(struct backing_dev_info *bdi, struct page *page)
{
}
EXPORT_SYMBOL(default_unplug_io_fn);

struct backing_dev_info default_backing_dev_info = {
	.name		= "default",
	.ra_pages	= VM_MAX_READAHEAD * 1024 / PAGE_CACHE_SIZE,
	.state		= 0,
	.capabilities	= BDI_CAP_MAP_COPY,
	.unplug_io_fn	= default_unplug_io_fn,
};
EXPORT_SYMBOL_GPL(default_backing_dev_info);

struct backing_dev_info noop_backing_dev_info = {
	.name		= "noop",
	.capabilities	= BDI_CAP_NO_ACCT_AND_WRITEBACK,
};
EXPORT_SYMBOL_GPL(noop_backing_dev_info);

static struct class *bdi_class;

/*
 * bdi_lock protects updates to bdi_list and bdi_pending_list, as well as
 * reader side protection for bdi_pending_list. bdi_list has RCU reader side
 * locking.
 */
DEFINE_SPINLOCK(bdi_lock);
LIST_HEAD(bdi_list);
LIST_HEAD(bdi_pending_list);

static struct task_struct *sync_supers_tsk;
static struct timer_list sync_supers_timer;

static int bdi_sync_supers(void *);
static void sync_supers_timer_fn(unsigned long);

static void bdi_add_default_flusher_task(struct backing_dev_info *bdi);

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>

static struct dentry *bdi_debug_root;

static void bdi_debug_init(void)
{
	bdi_debug_root = debugfs_create_dir("bdi", NULL);
}

static int bdi_debug_stats_show(struct seq_file *m, void *v)
{
	struct backing_dev_info *bdi = m->private;
	struct bdi_writeback *wb;
	unsigned long background_thresh;
	unsigned long dirty_thresh;
	unsigned long bdi_thresh;
	unsigned long nr_dirty, nr_io, nr_more_io, nr_wb, nr_dirty_time;
	struct inode *inode;

	/*
	 * inode lock is enough here, the bdi->wb_list is protected by
	 * RCU on the reader side
	 */
	nr_wb = nr_dirty = nr_io = nr_more_io = nr_dirty_time = 0;
	spin_lock(&inode_lock);
	list_for_each_entry(wb, &bdi->wb_list, list) {
		nr_wb++;
		list_for_each_entry(inode, &wb->b_dirty, i_list)
			nr_dirty++;
		list_for_each_entry(inode, &wb->b_io, i_list)
			nr_io++;
		list_for_each_entry(inode, &wb->b_more_io, i_list)
			nr_more_io++;
		list_for_each_entry(inode, &wb->b_dirty_time, i_list)
			if (inode->i_state & I_DIRTY_TIME)
				nr_dirty_time++;
	}
	spin_unlock(&inode_lock);

	get_dirty_limits(&background_thresh, &dirty_thresh, &bdi_thresh, bdi);

#define K(x) ((x) << (PAGE_SHIFT - 10))
	seq_printf(m,
		   "BdiWriteback:     %8lu kB\n"
		   "BdiReclaimable:   %8lu kB\n"
		   "BdiDirtyThresh:   %8lu kB\n"
		   "DirtyThresh:      %8lu kB\n"
		   "BackgroundThresh: %8lu kB\n"
		   "WritebackThreads: %8lu\n"
		   "b_dirty:          %8lu\n"
		   "b_io:             %8lu\n"
		   "b_more_io:        %8lu\n"
		   "b_dirty_time:     %8lu\n"
		   "bdi_list:         %8u\n"
		   "state:            %8lx\n"
		   "wb_list:          %8u\n",
		   (unsigned long) K(bdi_stat(bdi, BDI_WRITEBACK)),
		   (unsigned long) K(bdi_stat(bdi, BDI_RECLAIMABLE)),
		   K(bdi_thresh), K(dirty_thresh),
		   K(background_thresh), nr_wb, nr_dirty, nr_io, nr_more_io,
		   nr_dirty_time,
		   !list_empty(&bdi->bdi_list), bdi->state,
		   !list_empty(&bdi->wb_list));
#undef K

	return 0;
}

static int bdi_debug_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, bdi_debug_stats_show, inode->i_private);
}

static const struct file_operations bdi_debug_stats_fops = {
	.open		= bdi_debug_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void bdi_debug_register(struct backing_dev_info *bdi, const char *name)
{
	bdi->debug_dir = debugfs_create_dir(name, bdi_debug_root);
	bdi->debug_stats = debugfs_create_file("stats", 0444, bdi->debug_dir,
					       bdi, &bdi_debug_stats_fops);
}

static void bdi_debug_unregister(struct backing_dev_info *bdi)
{
	debugfs_remove(bdi->debug_stats);
	debugfs_remove(bdi->debug_dir);
}
#else
static inline void bdi_debug_init(void)
{
}
static inline void bdi_debug_register(struct backing_dev_info *bdi,
				      const char *name)
{
}
static inline void bdi_debug_unregister(struct backing_dev_info *bdi)
{
}
#endif

static ssize_t read_ahead_kb_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct backing_dev_info *bdi = dev_get_drvdata(dev);
	char *end;
	unsigned long read_ahead_kb;
	ssize_t ret = -EINVAL;

	read_ahead_kb = simple_strtoul(buf, &end, 10);
	if (*buf && (end[0] == '\0' || (end[0] == '\n' && end[1] == '\0'))) {
		bdi->ra_pages = read_ahead_kb >> (PAGE_SHIFT - 10);
		ret = count;
	}
	return ret;
}

#define K(pages) ((pages) << (PAGE_SHIFT - 10))

#define BDI_SHOW(name, expr)						\
static ssize_t name##_show(struct device *dev,				\
			   struct device_attribute *attr, char *page)	\
{									\
	struct backing_dev_info *bdi = dev_get_drvdata(dev);		\
									\
	return snprintf(page, PAGE_SIZE-1, "%lld\n", (long long)expr);	\
}

BDI_SHOW(read_ahead_kb, K(bdi->ra_pages))

static inline ssize_t generic_uint_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count,
		int (*set_func) (struct backing_dev_info *, unsigned int))
{
	struct backing_dev_info *bdi = dev_get_drvdata(dev);
	char *end;
	unsigned int val;
	ssize_t ret = -EINVAL;

	val = simple_strtoul(buf, &end, 10);
	if (*buf && (end[0] == '\0' || (end[0] == '\n' && end[1] == '\0'))) {
		ret = set_func(bdi, val);
		if (!ret)
			ret = count;
	}
	return ret;
}

static ssize_t min_ratio_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return generic_uint_store(dev, attr, buf, count, bdi_set_min_ratio);
}
BDI_SHOW(min_ratio, bdi->min_ratio)

static ssize_t max_ratio_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return generic_uint_store(dev, attr, buf, count, bdi_set_max_ratio);
}
BDI_SHOW(max_ratio, bdi->max_ratio)

static ssize_t min_dirty_pages_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return generic_uint_store(dev, attr, buf, count, bdi_set_min_dirty);
}
BDI_SHOW(min_dirty_pages, bdi->min_dirty_pages)

static ssize_t max_dirty_pages_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return generic_uint_store(dev, attr, buf, count, bdi_set_max_dirty);
}
BDI_SHOW(max_dirty_pages, bdi->max_dirty_pages)

static ssize_t stable_pages_required_show(struct device *dev,
					  struct device_attribute *attr,
					  char *page)
{
	struct backing_dev_info *bdi = dev_get_drvdata(dev);

	return snprintf(page, PAGE_SIZE-1, "%d\n",
			bdi_cap_stable_pages_required(bdi) ? 1 : 0);
}

static struct device_attribute bdi_dev_attrs[] = {
	__ATTR_RW(read_ahead_kb),
	__ATTR_RW(min_ratio),
	__ATTR_RW(max_ratio),
	__ATTR_RW(min_dirty_pages),
	__ATTR_RW(max_dirty_pages),
	__ATTR_RO(stable_pages_required),
	__ATTR_NULL,
};

static __init int bdi_class_init(void)
{
	bdi_class = class_create(THIS_MODULE, "bdi");
	bdi_class->dev_attrs = bdi_dev_attrs;
	bdi_debug_init();
	return 0;
}
postcore_initcall(bdi_class_init);

static int __init default_bdi_init(void)
{
	int err;

	sync_supers_tsk = kthread_run(bdi_sync_supers, NULL, "sync_supers");
	BUG_ON(IS_ERR(sync_supers_tsk));

	init_timer(&sync_supers_timer);
	setup_timer(&sync_supers_timer, sync_supers_timer_fn, 0);
	bdi_arm_supers_timer();

	err = bdi_init(&default_backing_dev_info);
	if (!err)
		bdi_register(&default_backing_dev_info, NULL, "default");
	err = bdi_init(&noop_backing_dev_info);

	return err;
}
subsys_initcall(default_bdi_init);

static void bdi_wb_init(struct bdi_writeback *wb, struct backing_dev_info *bdi)
{
	memset(wb, 0, sizeof(*wb));

	wb->bdi = bdi;
	wb->last_old_flush = jiffies;
	INIT_LIST_HEAD(&wb->b_dirty);
	INIT_LIST_HEAD(&wb->b_io);
	INIT_LIST_HEAD(&wb->b_more_io);
	INIT_LIST_HEAD(&wb->b_dirty_time);
}

static void bdi_task_init(struct backing_dev_info *bdi,
			  struct bdi_writeback *wb)
{
	struct task_struct *tsk = current;

	spin_lock(&bdi->wb_lock);
	list_add_tail_rcu(&wb->list, &bdi->wb_list);
	spin_unlock(&bdi->wb_lock);

	tsk->flags |= PF_FLUSHER | PF_SWAPWRITE;
	set_freezable();

	/*
	 * Our parent may run at a different priority, just set us to normal
	 */
	set_user_nice(tsk, 0);
}

extern struct io_context *current_io_context(gfp_t gfp_flags, int node);

static int bdi_start_fn(void *ptr)
{
	struct bdi_writeback *wb = ptr;
	struct backing_dev_info *bdi = wb->bdi;
	int ret;

	/*
	 * Add us to the active bdi_list
	 */
	spin_lock_bh(&bdi_lock);
	list_add_rcu(&bdi->bdi_list, &bdi_list);
	spin_unlock_bh(&bdi_lock);

	bdi_task_init(bdi, wb);

	(void)current_io_context(GFP_KERNEL, -1);

	/*
	 * Clear pending bit and wakeup anybody waiting to tear us down
	 */
	clear_bit(BDI_pending, &bdi->state);
	smp_mb__after_clear_bit();
	wake_up_bit(&bdi->state, BDI_pending);

	ret = bdi_writeback_task(wb);

	/*
	 * Remove us from the list
	 */
	spin_lock(&bdi->wb_lock);
	list_del_rcu(&wb->list);
	spin_unlock(&bdi->wb_lock);

	/*
	 * Flush any work that raced with us exiting. No new work
	 * will be added, since this bdi isn't discoverable anymore.
	 */
	if (!list_empty(&bdi->work_list))
		wb_do_writeback(wb, 1);

	wb->task = NULL;
	return ret;
}

int bdi_has_dirty_io(struct backing_dev_info *bdi)
{
	return wb_has_dirty_io(&bdi->wb);
}

static void bdi_flush_io(struct backing_dev_info *bdi)
{
	struct writeback_control wbc = {
		.sync_mode		= WB_SYNC_NONE,
		.older_than_this	= NULL,
		.range_cyclic		= 1,
		.nr_to_write		= 1024,
	};

	writeback_inodes_wb(&bdi->wb, &wbc);
}

/*
 * kupdated() used to do this. We cannot do it from the bdi_forker_task()
 * or we risk deadlocking on ->s_umount. The longer term solution would be
 * to implement sync_supers_bdi() or similar and simply do it from the
 * bdi writeback tasks individually.
 */
static int bdi_sync_supers(void *unused)
{
	set_user_nice(current, 0);

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();

		/*
		 * Do this periodically, like kupdated() did before.
		 */
		sync_supers();
	}

	return 0;
}

void bdi_arm_supers_timer(void)
{
	unsigned long next;

	if (!dirty_writeback_interval)
		return;

	next = msecs_to_jiffies(dirty_writeback_interval * 10) + jiffies;
	mod_timer(&sync_supers_timer, round_jiffies_up(next));
}

static void sync_supers_timer_fn(unsigned long unused)
{
	wake_up_process(sync_supers_tsk);
	bdi_arm_supers_timer();
}

static int bdi_forker_task(void *ptr)
{
	struct bdi_writeback *me = ptr;

	bdi_task_init(me->bdi, me);

	for (;;) {
		struct task_struct *task;
		struct backing_dev_info *bdi, *tmp;

		/*
		 * Temporary measure, we want to make sure we don't see
		 * dirty data on the default backing_dev_info
		 */
		if (wb_has_dirty_io(me) || !list_empty(&me->bdi->work_list))
			wb_do_writeback(me, 0);

		spin_lock_bh(&bdi_lock);

		/*
		 * Check if any existing bdi's have dirty data without
		 * a thread registered. If so, set that up.
		 */
		list_for_each_entry_safe(bdi, tmp, &bdi_list, bdi_list) {
			if (bdi->wb.task)
				continue;
			if (list_empty(&bdi->work_list) &&
			    !bdi_has_dirty_io(bdi))
				continue;

			bdi_add_default_flusher_task(bdi);
		}

		set_current_state(TASK_INTERRUPTIBLE);

		if (list_empty(&bdi_pending_list)) {
			unsigned long wait;

			spin_unlock_bh(&bdi_lock);
			wait = msecs_to_jiffies(dirty_writeback_interval * 10);
			if (wait)
				schedule_timeout(wait);
			else
				schedule();
			try_to_freeze();
			continue;
		}

		__set_current_state(TASK_RUNNING);

		/*
		 * This is our real job - check for pending entries in
		 * bdi_pending_list, and create the tasks that got added
		 */
		bdi = list_entry(bdi_pending_list.next, struct backing_dev_info,
				 bdi_list);
		list_del_init(&bdi->bdi_list);
		spin_unlock_bh(&bdi_lock);

		task = kthread_create(bdi_start_fn, &bdi->wb, "flush-%s",
				dev_name(bdi->dev));
		if (IS_ERR(task)) {
			/*
			 * If thread creation fails, then readd the bdi back to
			 * the list and force writeout of the bdi from this
			 * forker thread. That will free some memory and we can
			 * try again. Add it to the tail so we get a chance to
			 * flush other bdi's to free memory.
			 */
			spin_lock_bh(&bdi_lock);
			list_add_tail(&bdi->bdi_list, &bdi_pending_list);
			spin_unlock_bh(&bdi_lock);

			bdi_flush_io(bdi);
		} else {
			bdi->wb.task = task;
			/*
			 * And as soon as the bdi thread is visible,
			 * we can start it.
			 */
			wake_up_process(task);
		}
	}

	return 0;
}

static void bdi_add_to_pending(struct rcu_head *head)
{
	struct backing_dev_info *bdi;

	bdi = container_of(head, struct backing_dev_info, rcu_head);
	INIT_LIST_HEAD(&bdi->bdi_list);

	spin_lock(&bdi_lock);
	list_add_tail(&bdi->bdi_list, &bdi_pending_list);
	spin_unlock(&bdi_lock);

	/*
	 * We are now on the pending list, wake up bdi_forker_task()
	 * to finish the job and add us back to the active bdi_list
	 */
	wake_up_process(default_backing_dev_info.wb.task);
}

/*
 * Add the default flusher task that gets created for any bdi
 * that has dirty data pending writeout
 */
void static bdi_add_default_flusher_task(struct backing_dev_info *bdi)
{
	if (!bdi_cap_writeback_dirty(bdi))
		return;

	if (WARN_ON(!test_bit(BDI_registered, &bdi->state))) {
		printk(KERN_ERR "bdi %p/%s is not registered!\n",
							bdi, bdi->name);
		return;
	}

	/*
	 * Check with the helper whether to proceed adding a task. Will only
	 * abort if we two or more simultanous calls to
	 * bdi_add_default_flusher_task() occured, further additions will block
	 * waiting for previous additions to finish.
	 */
	if (!test_and_set_bit(BDI_pending, &bdi->state)) {
		list_del_rcu(&bdi->bdi_list);

		/*
		 * We must wait for the current RCU period to end before
		 * moving to the pending list. So schedule that operation
		 * from an RCU callback.
		 */
		call_rcu(&bdi->rcu_head, bdi_add_to_pending);
	}
}

/*
 * Remove bdi from bdi_list, and ensure that it is no longer visible
 */
static void bdi_remove_from_list(struct backing_dev_info *bdi)
{
	spin_lock_bh(&bdi_lock);
	list_del_rcu(&bdi->bdi_list);
	spin_unlock_bh(&bdi_lock);

	synchronize_rcu_expedited();
}

int bdi_register(struct backing_dev_info *bdi, struct device *parent,
		const char *fmt, ...)
{
	va_list args;
	int ret = 0;
	struct device *dev;
	struct ve_struct *ve;

	if (bdi->dev)	/* The driver needs to use separate queues per device */
		goto exit;

	ve = set_exec_env(&ve0);
	va_start(args, fmt);
	dev = device_create_vargs(bdi_class, parent, MKDEV(0, 0), bdi, fmt, args);
	va_end(args);
	set_exec_env(ve);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto exit;
	}

	spin_lock_bh(&bdi_lock);
	list_add_tail_rcu(&bdi->bdi_list, &bdi_list);
	spin_unlock_bh(&bdi_lock);

	bdi->dev = dev;

	/*
	 * Just start the forker thread for our default backing_dev_info,
	 * and add other bdi's to the list. They will get a thread created
	 * on-demand when they need it.
	 */
	if (bdi_cap_flush_forker(bdi)) {
		struct bdi_writeback *wb = &bdi->wb;

		wb->task = kthread_run(bdi_forker_task, wb, "bdi-%s",
						dev_name(dev));
		if (IS_ERR(wb->task)) {
			wb->task = NULL;
			ret = -ENOMEM;

			bdi_remove_from_list(bdi);
			goto exit;
		}
	}

	bdi_debug_register(bdi, dev_name(dev));
	set_bit(BDI_registered, &bdi->state);
	trace_writeback_bdi_register(bdi);
exit:
	return ret;
}
EXPORT_SYMBOL(bdi_register);

int bdi_register_dev(struct backing_dev_info *bdi, dev_t dev)
{
	return bdi_register(bdi, NULL, "%u:%u", MAJOR(dev), MINOR(dev));
}
EXPORT_SYMBOL(bdi_register_dev);

/*
 * Remove bdi from the global list and shutdown any threads we have running
 */
static void bdi_wb_shutdown(struct backing_dev_info *bdi)
{
	struct bdi_writeback *wb;

	if (!bdi_cap_writeback_dirty(bdi))
		return;

	/*
	 * If setup is pending, wait for that to complete first
	 */
	wait_on_bit(&bdi->state, BDI_pending, bdi_sched_wait,
			TASK_UNINTERRUPTIBLE);

	/*
	 * Make sure nobody finds us on the bdi_list anymore
	 */
	bdi_remove_from_list(bdi);

	/*
	 * Finally, kill the kernel threads. We don't need to be RCU
	 * safe anymore, since the bdi is gone from visibility. Force
	 * unfreeze of the thread before calling kthread_stop(), otherwise
	 * it would never exet if it is currently stuck in the refrigerator.
	 */
	list_for_each_entry(wb, &bdi->wb_list, list) {
		wb->task->flags &= ~PF_FROZEN;
		kthread_stop(wb->task);
	}
}

/*
 * This bdi is going away now, make sure that no super_blocks point to it
 */
static void bdi_prune_sb(struct backing_dev_info *bdi)
{
	struct super_block *sb;

	spin_lock(&sb_lock);
	list_for_each_entry(sb, &super_blocks, s_list) {
		if (sb->s_bdi == bdi)
			sb->s_bdi = &default_backing_dev_info;
	}
	spin_unlock(&sb_lock);
}

void bdi_unregister(struct backing_dev_info *bdi)
{
	struct ve_struct *ve;

	if (bdi->dev) {
		trace_writeback_bdi_unregister(bdi);
		bdi_prune_sb(bdi);

		if (!bdi_cap_flush_forker(bdi))
			bdi_wb_shutdown(bdi);
		bdi_debug_unregister(bdi);
		ve = set_exec_env(&ve0);
		device_unregister(bdi->dev);
		set_exec_env(ve);
		bdi->dev = NULL;
	}
}
EXPORT_SYMBOL(bdi_unregister);

int bdi_init(struct backing_dev_info *bdi)
{
	int i, err;

	bdi->dev = NULL;

	bdi->min_ratio = 0;
	bdi->max_ratio = 100;
	bdi->max_prop_frac = PROP_FRAC_BASE;
	bdi->min_dirty_pages = 0;
	bdi->max_dirty_pages = 0;
	spin_lock_init(&bdi->wb_lock);
	INIT_RCU_HEAD(&bdi->rcu_head);
	INIT_LIST_HEAD(&bdi->bdi_list);
	INIT_LIST_HEAD(&bdi->wb_list);
	INIT_LIST_HEAD(&bdi->work_list);
	init_waitqueue_head(&bdi->cong_waitq);

	bdi_wb_init(&bdi->wb, bdi);

	for (i = 0; i < NR_BDI_STAT_ITEMS; i++) {
		err = percpu_counter_init(&bdi->bdi_stat[i], 0);
		if (err)
			goto err;
	}

	bdi->dirty_exceeded = 0;
	err = prop_local_init_percpu(&bdi->completions);

	if (err) {
err:
		while (i--)
			percpu_counter_destroy(&bdi->bdi_stat[i]);
	}

	return err;
}
EXPORT_SYMBOL(bdi_init);

void bdi_destroy(struct backing_dev_info *bdi)
{
	int i;

	/*
	 * Splice our entries to the default_backing_dev_info, if this
	 * bdi disappears
	 */
	if (bdi_has_dirty_io(bdi)) {
		struct bdi_writeback *dst = &default_backing_dev_info.wb;

		spin_lock(&inode_lock);
		list_splice(&bdi->wb.b_dirty, &dst->b_dirty);
		list_splice(&bdi->wb.b_io, &dst->b_io);
		list_splice(&bdi->wb.b_more_io, &dst->b_more_io);
		spin_unlock(&inode_lock);
	}

	bdi_unregister(bdi);

	for (i = 0; i < NR_BDI_STAT_ITEMS; i++)
		percpu_counter_destroy(&bdi->bdi_stat[i]);

	prop_local_destroy_percpu(&bdi->completions);
}
EXPORT_SYMBOL(bdi_destroy);

static wait_queue_head_t congestion_wqh[2] = {
		__WAIT_QUEUE_HEAD_INITIALIZER(congestion_wqh[0]),
		__WAIT_QUEUE_HEAD_INITIALIZER(congestion_wqh[1])
	};
static atomic_t nr_bdi_congested[2];

void clear_bdi_congested(struct backing_dev_info *bdi, int sync)
{
	enum bdi_state bit;
	wait_queue_head_t *wqh = &congestion_wqh[sync];

	bit = sync ? BDI_sync_congested : BDI_async_congested;
	if (test_and_clear_bit(bit, &bdi->state))
		atomic_dec(&nr_bdi_congested[sync]);
	smp_mb__after_clear_bit();
	if (waitqueue_active(wqh))
		wake_up(wqh);
}
EXPORT_SYMBOL(clear_bdi_congested);

void set_bdi_congested(struct backing_dev_info *bdi, int sync)
{
	enum bdi_state bit;

	bit = sync ? BDI_sync_congested : BDI_async_congested;
	if (!test_and_set_bit(bit, &bdi->state))
		atomic_inc(&nr_bdi_congested[sync]);
}
EXPORT_SYMBOL(set_bdi_congested);

/**
 * congestion_wait - wait for a backing_dev to become uncongested
 * @sync: SYNC or ASYNC IO
 * @timeout: timeout in jiffies
 *
 * Waits for up to @timeout jiffies for a backing_dev (any backing_dev) to exit
 * write congestion.  If no backing_devs are congested then just wait for the
 * next write to be completed.
 */
long congestion_wait(int sync, long timeout)
{
	long ret;
	DEFINE_WAIT(wait);
	wait_queue_head_t *wqh = &congestion_wqh[sync];

	prepare_to_wait(wqh, &wait, TASK_UNINTERRUPTIBLE);
	ret = io_schedule_timeout(timeout);
	finish_wait(wqh, &wait);
	return ret;
}
EXPORT_SYMBOL(congestion_wait);

/**
 * wait_iff_congested - Conditionally wait for a backing_dev to become uncongested or a zone to complete writes
 * @zone: A zone to check if it is heavily congested
 * @sync: SYNC or ASYNC IO
 * @timeout: timeout in jiffies
 *
 * In the event of a congested backing_dev (any backing_dev) and the given
 * @zone has experienced recent congestion, this waits for up to @timeout
 * jiffies for either a BDI to exit congestion of the given @sync queue
 * or a write to complete.
 *
 * In the absense of zone congestion, cond_resched() is called to yield
 * the processor if necessary but otherwise does not sleep.
 *
 * The return value is 0 if the sleep is for the full timeout. Otherwise,
 * it is the number of jiffies that were still remaining when the function
 * returned. return_value == timeout implies the function did not sleep.
 */
long wait_iff_congested(struct zone *zone, int sync, long timeout)
{
	long ret;
	unsigned long start = jiffies;
	DEFINE_WAIT(wait);
	wait_queue_head_t *wqh = &congestion_wqh[sync];

	/*
	 * If there is no congestion, or heavy congestion is not being
	 * encountered in the current zone, yield if necessary instead
	 * of sleeping on the congestion queue
	 */
	if (atomic_read(&nr_bdi_congested[sync]) == 0 ||
			!zone_is_reclaim_congested(zone)) {
		cond_resched();

		/* In case we scheduled, work out time remaining */
		ret = timeout - (jiffies - start);
		if (ret < 0)
			ret = 0;

		goto out;
	}

	/* Sleep until uncongested or a write happens */
	prepare_to_wait(wqh, &wait, TASK_UNINTERRUPTIBLE);
	ret = io_schedule_timeout(timeout);
	finish_wait(wqh, &wait);

out:

	return ret;
}
EXPORT_SYMBOL(wait_iff_congested);
