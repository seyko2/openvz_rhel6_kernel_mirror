#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/mmgang.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/wait.h>
#include <linux/seqlock.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/ioport.h>
#include <linux/backing-dev.h>

static unsigned int kstaled_scan_secs;
static DECLARE_WAIT_QUEUE_HEAD(kstaled_wait);

struct kstaled_page_info {
	int referenced_ptes;
	int dirty_ptes;
	unsigned long vm_flags;
};

static int __kstaled_check_page(struct page *page,
		struct vm_area_struct *vma, unsigned long address, void *data)
{
	struct kstaled_page_info *info = data;
	struct mm_struct *mm = vma->vm_mm;

	if (unlikely(PageTransHuge(page))) {
		pmd_t *pmd;

		spin_lock(&mm->page_table_lock);
		pmd = page_check_address_pmd(page, mm, address,
					     PAGE_CHECK_ADDRESS_PMD_FLAG);
		if (!pmd) {
			spin_unlock(&mm->page_table_lock);
			goto out;
		}

		if (pmdp_test_and_clear_young(vma, address, pmd)) {
			info->referenced_ptes++;
			info->vm_flags |= vma->vm_flags;
			SetPageYoung(page);
		}
		info->dirty_ptes++;
		spin_unlock(&mm->page_table_lock);
	} else {
		pte_t *pte;
		spinlock_t *ptl;

		pte = page_check_address(page, mm, address, &ptl, 0);
		if (!pte)
			goto out;

		if (ptep_test_and_clear_young(vma, address, pte)) {
			info->referenced_ptes++;
			info->vm_flags |= vma->vm_flags;
			SetPageYoung(page);
		}
		if (pte_dirty(*pte))
			info->dirty_ptes++;
		pte_unmap_unlock(pte, ptl);
	}

out:
	if (vma->vm_flags & VM_LOCKED)
		info->vm_flags |= VM_LOCKED;

	return SWAP_AGAIN;
}

static void kstaled_check_page(struct page *page,
			       struct kstaled_page_info *info)
{
	info->referenced_ptes = 0;
	info->dirty_ptes = 0;
	info->vm_flags = 0;

	rmap_walk(page, __kstaled_check_page, info);

	if (page_test_and_clear_young(page))
		info->referenced_ptes++;

	if (info->referenced_ptes && !PageAnon(page)) {
		/*
		 * Promote shared file-mapped pages.
		 */
		if (info->referenced_ptes > 1)
			SetPageReferenced(page);

		/*
		 * Stimulate activation of file-backed executable pages at
		 * first reference.
		 */
		if (info->vm_flags & VM_EXEC)
			SetPageReferenced(page);
	}

	if (!PageIdle(page)) {
		SetPageIdle(page);
		info->referenced_ptes++;
	}
}

static int kstaled_scan_page(struct page *page)
{
	struct kstaled_page_info info;
	struct address_space *mapping;
	struct gang *gang;
	struct idle_page_stats *stats;
	int nr_pages = 1;
	int swap_backed = 1;

	if (!PageLRU(page))
		goto out;

	if (!PageCompound(page)) {
		if (PageMlocked(page))
			goto out;
		if (!page->mapping && !PageSwapCache(page))
			goto out;
	}

	if (!get_page_unless_zero(page))
		goto out;

	if (unlikely(!PageLRU(page)))
		goto out_put_page;

	nr_pages = 1 << compound_trans_order(page);

	if (PageMlocked(page))
		goto out_put_page;

	if (!trylock_page(page))
		goto out_put_page;

	if (!PageAnon(page) && !PageSwapCache(page)) {
		mapping = page->mapping;
		if (!mapping)
			goto out_unlock_page;

		if (mapping_unevictable(mapping))
			goto out_unlock_page;

		swap_backed = mapping_cap_swap_backed(mapping);
		if (!swap_backed &&
		    !mapping_cap_writeback_dirty(mapping))
			goto out_unlock_page;
	}

	kstaled_check_page(page, &info);

	unlock_page(page);

	if (info.referenced_ptes || (info.vm_flags & VM_LOCKED))
		goto out_put_page;

	rcu_read_lock();
	gang = page_gang(page);
	stats = &gang->idle_scan_stats;
	if (info.dirty_ptes || PageDirty(page) || PageWriteback(page)) {
		if (swap_backed)
			stats->idle_dirty_swap += nr_pages;
		else
			stats->idle_dirty_file += nr_pages;
	} else
		stats->idle_clean += nr_pages;
	rcu_read_unlock();

out_put_page:
	put_page(page);
out:
	return nr_pages;

out_unlock_page:
	unlock_page(page);
	goto out_put_page;
}

static int kstaled_scan_pages_range(unsigned long start_pfn,
				    unsigned long nr_pages, void *arg)
{
	unsigned long pfn = start_pfn;
	unsigned long end_pfn = start_pfn + nr_pages;

	while (pfn < end_pfn) {
		if (!pfn_valid(pfn)) {
			pfn++;
			continue;
		}
		pfn += kstaled_scan_page(pfn_to_page(pfn));
		cond_resched();
	}
	return 0;
}

static void kstaled_do_scan(void)
{
	walk_system_ram_range(0, 1UL << MAX_PHYSMEM_BITS, NULL,
			      kstaled_scan_pages_range);
}

static void kstaled_update_stats(void)
{
	struct zone *zone;
	struct gang *gang;

	rcu_read_lock();
	for_each_zone(zone) {
		for_each_gang(gang, zone) {
			write_seqcount_begin(&gang->idle_page_stats_lock);
			gang->idle_page_stats = gang->idle_scan_stats;
			write_seqcount_end(&gang->idle_page_stats_lock);
			memset(&gang->idle_scan_stats, 0,
			       sizeof(gang->idle_scan_stats));
		}
	}
	rcu_read_unlock();

}

static inline int kstaled_should_run(void)
{
	return kstaled_scan_secs > 0;
}

static int kstaled_scan_thread(void *arg)
{
	set_freezable();
	set_user_nice(current, 5);

	while (!kthread_should_stop()) {
		if (kstaled_should_run()) {
			kstaled_do_scan();
			kstaled_update_stats();
		}

		try_to_freeze();

		if (kstaled_should_run()) {
			schedule_timeout_interruptible(kstaled_scan_secs * HZ);
		} else {
			/* zero idle page stats */
			kstaled_update_stats();
			wait_event_freezable(kstaled_wait,
				kstaled_should_run() || kthread_should_stop());
		}
	}

	return 0;
}

static ssize_t kstaled_scan_secs_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", kstaled_scan_secs);
}

static ssize_t kstaled_scan_secs_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned long val;

	if (strict_strtoul(buf, 10, &val))
		return -EINVAL;
	kstaled_scan_secs = val;
	wake_up_interruptible(&kstaled_wait);
	return count;
}

static struct kobj_attribute kstaled_scan_secs_attr = __ATTR(scan_secs, 0644,
		kstaled_scan_secs_show, kstaled_scan_secs_store);

static struct attribute *kstaled_attrs[] = {
	&kstaled_scan_secs_attr.attr,
	NULL,
};

static struct attribute_group kstaled_attr_group = {
	.attrs = kstaled_attrs,
	.name = "kstaled",
};

static __init int kstaled_init(void)
{
	int err;
	struct task_struct *kstaled_thread;

	kstaled_thread = kthread_run(kstaled_scan_thread, NULL, "kstaled");
	if (IS_ERR(kstaled_thread)) {
		printk(KERN_ERR "Failed to start kstaled\n");
		return PTR_ERR(kstaled_thread);
	}

	err = sysfs_create_group(mm_kobj, &kstaled_attr_group);
	if (err) {
		printk(KERN_ERR "kstaled: register sysfs failed\n");
		kthread_stop(kstaled_thread);
		return err;
	}

	return 0;
}
module_init(kstaled_init);
