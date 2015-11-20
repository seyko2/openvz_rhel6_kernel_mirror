#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>

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

static const struct file_operations cmdline_proc_fops = {
	.open		= cmdline_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_cmdline_init(void)
{
	proc_create("cmdline", 0, &glob_proc_root, &cmdline_proc_fops);
	return 0;
}
module_init(proc_cmdline_init);
