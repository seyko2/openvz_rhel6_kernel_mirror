#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <asm/uaccess.h>

static int cmdline_proc_show(struct seq_file *m, void *v)
{
	struct ve_struct *ve = get_exec_env();
	char *cmdline = ve->proc_cmdline;

	if (!cmdline)
		cmdline = "quiet";

	if (ve_is_super(ve))
		cmdline = saved_command_line;

	seq_printf(m, "%s\n", cmdline);
	return 0;
}

static int cmdline_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, cmdline_proc_show, NULL);
}

static ssize_t cmdline_proc_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct ve_struct *ve = get_exec_env();
	char *cmdline;

	if (count > PAGE_SIZE - 1)
 		count = PAGE_SIZE - 1;

	cmdline = kzalloc(count + 1, GFP_KERNEL);
	if (unlikely(!cmdline))
		return -ENOMEM;

	if (copy_from_user(cmdline, buf, count)) {
		kfree(cmdline);
		return -EFAULT;
	}

	if (!ve_is_super(ve))	{
		if (ve->proc_cmdline) {
			kfree(ve->proc_cmdline);
			ve->proc_cmdline = 0;
		}
		ve->proc_cmdline = cmdline;
	}
	else {
		static was_here = 0;
		if (was_here)
		    kfree(saved_command_line);

		was_here = 1;
		saved_command_line = cmdline;
	}

	return count;
}

static const struct file_operations cmdline_proc_fops = {
	.open		= cmdline_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= cmdline_proc_write,
};

static int __init proc_cmdline_init(void)
{
	proc_create("cmdline", S_IRUGO|S_IWUSR, &glob_proc_root, &cmdline_proc_fops);
	return 0;
}
module_init(proc_cmdline_init);
