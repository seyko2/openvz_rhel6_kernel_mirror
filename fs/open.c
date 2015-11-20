/*
 *  linux/fs/open.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/string.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/quotaops.h>
#include <linux/fsnotify.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/namei.h>
#include <linux/backing-dev.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <linux/security.h>
#include <linux/mount.h>
#include <linux/vfs.h>
#include <linux/fcntl.h>
#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/personality.h>
#include <linux/pagemap.h>
#include <linux/syscalls.h>
#include <linux/rcupdate.h>
#include <linux/audit.h>
#include <linux/falloc.h>
#include <linux/fs_struct.h>
#include <linux/ima.h>

#include "internal.h"

int do_truncate(struct dentry *dentry, loff_t length, unsigned int time_attrs,
	struct file *filp)
{
	int ret;
	struct iattr newattrs;

	/* Not pretty: "inode->i_size" shouldn't really be signed. But it is. */
	if (length < 0)
		return -EINVAL;

	newattrs.ia_size = length;
	newattrs.ia_valid = ATTR_SIZE | time_attrs;
	if (filp) {
		newattrs.ia_file = filp;
		newattrs.ia_valid |= ATTR_FILE;
	}

	/* Remove suid/sgid on truncate too */
	ret = should_remove_suid(dentry);
	if (ret)
		newattrs.ia_valid |= ret | ATTR_FORCE;

	mutex_lock(&dentry->d_inode->i_mutex);
	ret = notify_change(dentry, &newattrs);
	mutex_unlock(&dentry->d_inode->i_mutex);
	return ret;
}

long vfs_truncate(struct path *path, loff_t length)
{
	struct inode *inode;
	long error;

	inode = path->dentry->d_inode;

	/* For directories it's -EISDIR, for other non-regulars - -EINVAL */
	if (S_ISDIR(inode->i_mode))
		return -EISDIR;
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	error = mnt_want_write(path->mnt);
	if (error)
		goto out;

	error = inode_permission(inode, MAY_WRITE);
	if (error)
		goto mnt_drop_write_and_out;

	error = -EPERM;
	if (IS_APPEND(inode))
		goto mnt_drop_write_and_out;

	error = get_write_access(inode);
	if (error)
		goto mnt_drop_write_and_out;

	/*
	 * Make sure that there are no leases.  get_write_access() protects
	 * against the truncate racing with a lease-granting setlease().
	 */
	error = break_lease(inode, O_WRONLY);
	if (error)
		goto put_write_and_out;

	error = locks_verify_truncate(inode, NULL, length);
	if (!error)
		error = security_path_truncate(path, length, 0);
	if (!error) {
		vfs_dq_init(inode);
		error = do_truncate(path->dentry, length, 0, NULL);
	}

put_write_and_out:
	put_write_access(inode);
mnt_drop_write_and_out:
	mnt_drop_write(path->mnt);
out:
	return error;
}
EXPORT_SYMBOL_GPL(vfs_truncate);

static long do_sys_truncate(const char __user *pathname, loff_t length)
{
	unsigned int lookup_flags = LOOKUP_FOLLOW;
	struct path path;
	int error;

	if (length < 0)	/* sorry, but loff_t says... */
		return -EINVAL;

retry:
	error = user_path_at(AT_FDCWD, pathname, lookup_flags, &path);
	if (!error) {
		error = vfs_truncate(&path, length);
		path_put(&path);
	}
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

SYSCALL_DEFINE2(truncate, const char __user *, path, long, length)
{
	return do_sys_truncate(path, length);
}

static long do_sys_ftruncate(unsigned int fd, loff_t length, int small)
{
	struct inode * inode;
	struct dentry *dentry;
	struct file * file;
	int error;

	error = -EINVAL;
	if (length < 0)
		goto out;
	error = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;

	/* explicitly opened as large or we are on 64-bit box */
	if (file->f_flags & O_LARGEFILE)
		small = 0;

	dentry = file->f_path.dentry;
	inode = dentry->d_inode;
	error = -EINVAL;
	if (!S_ISREG(inode->i_mode) || !(file->f_mode & FMODE_WRITE))
		goto out_putf;

	error = -EINVAL;
	/* Cannot ftruncate over 2^31 bytes without large file support */
	if (small && length > MAX_NON_LFS)
		goto out_putf;

	error = -EPERM;
	if (IS_APPEND(inode))
		goto out_putf;

	sb_start_write(inode->i_sb);
	error = locks_verify_truncate(inode, file, length);
	if (!error)
		error = security_path_truncate(&file->f_path, length,
					       ATTR_MTIME|ATTR_CTIME);
	if (!error)
		error = do_truncate(dentry, length, ATTR_MTIME|ATTR_CTIME, file);
	sb_end_write(inode->i_sb);
out_putf:
	fput(file);
out:
	return error;
}

SYSCALL_DEFINE2(ftruncate, unsigned int, fd, unsigned long, length)
{
	long ret = do_sys_ftruncate(fd, length, 1);
	/* avoid REGPARM breakage on x86: */
	asmlinkage_protect(2, ret, fd, length);
	return ret;
}
EXPORT_SYMBOL(do_truncate);

/* LFS versions of truncate are only needed on 32 bit machines */
#if BITS_PER_LONG == 32
SYSCALL_DEFINE(truncate64)(const char __user * path, loff_t length)
{
	return do_sys_truncate(path, length);
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_truncate64(long path, loff_t length)
{
	return SYSC_truncate64((const char __user *) path, length);
}
SYSCALL_ALIAS(sys_truncate64, SyS_truncate64);
#endif

SYSCALL_DEFINE(ftruncate64)(unsigned int fd, loff_t length)
{
	long ret = do_sys_ftruncate(fd, length, 0);
	/* avoid REGPARM breakage on x86: */
	asmlinkage_protect(2, ret, fd, length);
	return ret;
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_ftruncate64(long fd, loff_t length)
{
	return SYSC_ftruncate64((unsigned int) fd, length);
}
SYSCALL_ALIAS(sys_ftruncate64, SyS_ftruncate64);
#endif
#endif /* BITS_PER_LONG == 32 */


int do_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	long ret;

	if (offset < 0 || len <= 0)
		return -EINVAL;

	/* Return error if mode is not supported */
	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
		return -EOPNOTSUPP;

	/* Punch hole must have keep size set */
	if ((mode & FALLOC_FL_PUNCH_HOLE) &&
	    !(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;

	/* It's not possible punch hole on append only file */
	if (mode & FALLOC_FL_PUNCH_HOLE && IS_APPEND(inode))
		return -EPERM;

	if (IS_IMMUTABLE(inode))
		return -EPERM;

	/*
	 * Revalidate the write permissions, in case security policy has
	 * changed since the files were opened.
	 */
	ret = security_file_permission(file, MAY_WRITE);
	if (ret)
		return ret;

	if (S_ISFIFO(inode->i_mode))
		return -ESPIPE;

	/*
	 * Let individual file system decide if it supports preallocation
	 * for directories or not.
	 */
	if (!S_ISREG(inode->i_mode) && !S_ISDIR(inode->i_mode))
		return -ENODEV;

	/* Check for wrap through zero too */
	if (((offset + len) > inode->i_sb->s_maxbytes) || ((offset + len) < 0))
		return -EFBIG;

	if (!inode->i_op->fallocate)
		return -EOPNOTSUPP;

	sb_start_write(inode->i_sb);
	ret = inode->i_op->fallocate(inode, mode, offset, len);
	sb_end_write(inode->i_sb);
	return ret;
}

SYSCALL_DEFINE(fallocate)(int fd, int mode, loff_t offset, loff_t len)
{
	struct file *file;
	int error = -EBADF;

	file = fget(fd);
	if (file) {
		error = do_fallocate(file, mode, offset, len);
		fput(file);
	}

	return error;
}

#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_fallocate(long fd, long mode, loff_t offset, loff_t len)
{
	return SYSC_fallocate((int)fd, (int)mode, offset, len);
}
SYSCALL_ALIAS(sys_fallocate, SyS_fallocate);
#endif

/*
 * access() needs to use the real uid/gid, not the effective uid/gid.
 * We do this by temporarily clearing all FS-related capabilities and
 * switching the fsuid/fsgid around to the real ones.
 */
SYSCALL_DEFINE3(faccessat, int, dfd, const char __user *, filename, int, mode)
{
	const struct cred *old_cred;
	struct cred *override_cred;
	struct path path;
	struct inode *inode;
	int res;
	unsigned int lookup_flags = LOOKUP_FOLLOW;

	if (mode & ~S_IRWXO)	/* where's F_OK, X_OK, W_OK, R_OK? */
		return -EINVAL;

	override_cred = prepare_creds();
	if (!override_cred)
		return -ENOMEM;

	override_cred->fsuid = override_cred->uid;
	override_cred->fsgid = override_cred->gid;

	if (!issecure(SECURE_NO_SETUID_FIXUP)) {
		/* Clear the capabilities if we switch to a non-root user */
		if (override_cred->uid)
			cap_clear(override_cred->cap_effective);
		else
			override_cred->cap_effective =
				override_cred->cap_permitted;
	}

	old_cred = override_creds(override_cred);
retry:
	res = user_path_at(dfd, filename, lookup_flags, &path);
	if (res)
		goto out;

	inode = path.dentry->d_inode;

	if ((mode & MAY_EXEC) && S_ISREG(inode->i_mode)) {
		/*
		 * MAY_EXEC on regular files is denied if the fs is mounted
		 * with the "noexec" flag.
		 */
		res = -EACCES;
		if (path.mnt->mnt_flags & MNT_NOEXEC)
			goto out_path_release;
	}

	res = inode_permission(inode, mode | MAY_ACCESS);
	/* SuS v2 requires we report a read only fs too */
	if (res || !(mode & S_IWOTH) || special_file(inode->i_mode))
		goto out_path_release;
	/*
	 * This is a rare case where using __mnt_is_readonly()
	 * is OK without a mnt_want/drop_write() pair.  Since
	 * no actual write to the fs is performed here, we do
	 * not need to telegraph to that to anyone.
	 *
	 * By doing this, we accept that this access is
	 * inherently racy and know that the fs may change
	 * state before we even see this result.
	 */
	if (__mnt_is_readonly(path.mnt))
		res = -EROFS;

out_path_release:
	path_put(&path);
	if (retry_estale(res, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out:
	revert_creds(old_cred);
	put_cred(override_cred);
	return res;
}

SYSCALL_DEFINE2(access, const char __user *, filename, int, mode)
{
	return sys_faccessat(AT_FDCWD, filename, mode);
}

SYSCALL_DEFINE1(chdir, const char __user *, filename)
{
	struct path path;
	int error;
	unsigned int lookup_flags = LOOKUP_FOLLOW | LOOKUP_DIRECTORY;
retry:
	error = user_path_at(AT_FDCWD, filename, lookup_flags, &path);
	if (error)
		goto out;

	error = inode_permission(path.dentry->d_inode, MAY_EXEC | MAY_ACCESS);
	if (error)
		goto dput_and_out;

	set_fs_pwd(current->fs, &path);

dput_and_out:
	path_put(&path);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out:
	return error;
}

SYSCALL_DEFINE1(fchdir, unsigned int, fd)
{
	struct file *file;
	struct inode *inode;
	int error;

	error = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;

	inode = file->f_path.dentry->d_inode;

	error = -ENOTDIR;
	if (!S_ISDIR(inode->i_mode))
		goto out_putf;

	error = inode_permission(inode, MAY_EXEC | MAY_ACCESS);
	if (!error)
		set_fs_pwd(current->fs, &file->f_path);
out_putf:
	fput(file);
out:
	return error;
}

SYSCALL_DEFINE1(chroot, const char __user *, filename)
{
	struct path path;
	int error;
	unsigned int lookup_flags = LOOKUP_FOLLOW | LOOKUP_DIRECTORY;
retry:
	error = user_path_at(AT_FDCWD, filename, lookup_flags, &path);
	if (error)
		goto out;

	error = inode_permission(path.dentry->d_inode, MAY_EXEC | MAY_ACCESS);
	if (error)
		goto dput_and_out;

	error = -EPERM;
	if (!capable(CAP_SYS_CHROOT))
		goto dput_and_out;

	set_fs_root(current->fs, &path);
	error = 0;
dput_and_out:
	path_put(&path);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out:
	return error;
}

SYSCALL_DEFINE2(fchmod, unsigned int, fd, mode_t, mode)
{
	struct inode * inode;
	struct dentry * dentry;
	struct file * file;
	int err = -EBADF;
	struct iattr newattrs;

	file = fget(fd);
	if (!file)
		goto out;

	dentry = file->f_path.dentry;
	inode = dentry->d_inode;

	audit_inode(NULL, dentry, 0);

	err = mnt_want_write_file(file);
	if (err)
		goto out_putf;
	mutex_lock(&inode->i_mutex);
	if (mode == (mode_t) -1)
		mode = inode->i_mode;
	newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
	newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;
	err = notify_change(dentry, &newattrs);
	mutex_unlock(&inode->i_mutex);
	mnt_drop_write(file->f_path.mnt);
out_putf:
	fput(file);
out:
	return err;
}

static int do_fchmodat(int dfd, const char __user *filename, mode_t mode, int flag)
{
	struct path path;
	struct inode *inode;
	int error;
	struct iattr newattrs;
	unsigned int lookup_flags = LOOKUP_FOLLOW;
	int follow;
retry:
	error = -EINVAL;
	if ((flag & ~AT_SYMLINK_NOFOLLOW) != 0)
		goto out;

	follow = (flag & AT_SYMLINK_NOFOLLOW) ? 0 : lookup_flags;
	error = user_path_at(dfd, filename, follow, &path);
	if (error)
		goto out;
	inode = path.dentry->d_inode;

	error = mnt_want_write(path.mnt);
	if (error)
		goto dput_and_out;
	mutex_lock(&inode->i_mutex);
	if (mode == (mode_t) -1)
		mode = inode->i_mode;
	newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
	newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;
	error = notify_change(path.dentry, &newattrs);
	mutex_unlock(&inode->i_mutex);
	mnt_drop_write(path.mnt);
dput_and_out:
	path_put(&path);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out:
	return error;
}

SYSCALL_DEFINE3(fchmodat, int, dfd, const char __user *, filename, mode_t, mode)
{
	return do_fchmodat(dfd, filename, mode, 0);
}

SYSCALL_DEFINE2(chmod, const char __user *, filename, mode_t, mode)
{
	return do_fchmodat(AT_FDCWD, filename, mode, 0);
}
EXPORT_SYMBOL_GPL(sys_chmod);

SYSCALL_DEFINE2(lchmod, const char __user *, filename, mode_t, mode)
{
	return do_fchmodat(AT_FDCWD, filename, mode, AT_SYMLINK_NOFOLLOW);
}

static int chown_common(struct dentry * dentry, uid_t user, gid_t group)
{
	struct inode *inode = dentry->d_inode;
	int error;
	struct iattr newattrs;

	newattrs.ia_valid =  ATTR_CTIME;
	if (user != (uid_t) -1) {
		newattrs.ia_valid |= ATTR_UID;
		newattrs.ia_uid = user;
	}
	if (group != (gid_t) -1) {
		newattrs.ia_valid |= ATTR_GID;
		newattrs.ia_gid = group;
	}
	if (!S_ISDIR(inode->i_mode))
		newattrs.ia_valid |=
			ATTR_KILL_SUID | ATTR_KILL_SGID | ATTR_KILL_PRIV;
	mutex_lock(&inode->i_mutex);
	error = notify_change(dentry, &newattrs);
	mutex_unlock(&inode->i_mutex);

	return error;
}

SYSCALL_DEFINE5(fchownat, int, dfd, const char __user *, filename, uid_t, user,
		gid_t, group, int, flag)
{
	struct path path;
	int error = -EINVAL;
	unsigned int lookup_flags;

	if ((flag & ~AT_SYMLINK_NOFOLLOW) != 0)
		goto out;

	lookup_flags = (flag & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
retry:
	error = user_path_at(dfd, filename, lookup_flags, &path);
	if (error)
		goto out;
	error = mnt_want_write(path.mnt);
	if (error)
		goto out_release;
	error = chown_common(path.dentry, user, group);
	mnt_drop_write(path.mnt);
out_release:
	path_put(&path);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out:
	return error;
}

SYSCALL_DEFINE3(chown, const char __user *, filename, uid_t, user, gid_t, group)
{
	return sys_fchownat(AT_FDCWD, filename, user, group, 0);
}
EXPORT_SYMBOL(sys_chown);

SYSCALL_DEFINE3(lchown, const char __user *, filename, uid_t, user, gid_t, group)
{
	return sys_fchownat(AT_FDCWD, filename, user, group,
			    AT_SYMLINK_NOFOLLOW);
}

SYSCALL_DEFINE3(fchown, unsigned int, fd, uid_t, user, gid_t, group)
{
	struct file * file;
	int error = -EBADF;
	struct dentry * dentry;

	file = fget(fd);
	if (!file)
		goto out;

	error = mnt_want_write_file(file);
	if (error)
		goto out_fput;
	dentry = file->f_path.dentry;
	audit_inode(NULL, dentry, 0);
	error = chown_common(dentry, user, group);
	mnt_drop_write(file->f_path.mnt);
out_fput:
	fput(file);
out:
	return error;
}

/*
 * You have to be very careful that these write
 * counts get cleaned up in error cases and
 * upon __fput().  This should probably never
 * be called outside of __dentry_open().
 */
static inline int __get_file_write_access(struct inode *inode,
					  struct vfsmount *mnt)
{
	int error;
	error = get_write_access(inode);
	if (error)
		return error;
	/*
	 * Do not take mount writer counts on
	 * special files since no writes to
	 * the mount itself will occur.
	 */
	if (!special_file(inode->i_mode)) {
		/*
		 * Balanced in __fput()
		 */
		error = __mnt_want_write(mnt);
		if (error)
			put_write_access(inode);
	}
	return error;
}

static struct file *__dentry_open(struct dentry *dentry, struct vfsmount *mnt,
					struct file *f,
					int (*open)(struct inode *, struct file *),
					const struct cred *cred)
{
	struct inode *inode;
	int error;

	if (!may_use_odirect())
		f->f_flags &= ~O_DIRECT;
	if (ve_fsync_behavior() == FSYNC_NEVER)
		f->f_flags &= ~O_SYNC;

	f->f_mode = (__force fmode_t)((f->f_flags+1) & O_ACCMODE) | FMODE_LSEEK |
				FMODE_PREAD | FMODE_PWRITE;
	inode = dentry->d_inode;
	if (f->f_mode & FMODE_WRITE) {
		error = __get_file_write_access(inode, mnt);
		if (error)
			goto cleanup_file;
		if (!special_file(inode->i_mode))
			file_take_write(f);
	}

	f->f_mapping = inode->i_mapping;
	f->f_path.dentry = dentry;
	f->f_path.mnt = mnt;
	f->f_pos = 0;
	f->f_op = fops_get(inode->i_fop);
	file_sb_list_add(f, inode->i_sb);

	error = security_dentry_open(f, cred);
	if (error)
		goto cleanup_all;

	if (!open && f->f_op)
		open = f->f_op->open;
	if (open) {
		error = open(inode, f);
		if (error)
			goto cleanup_all;
	}
	ima_counts_get(f);

	f->f_flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);

	file_ra_state_init(&f->f_ra, f->f_mapping->host->i_mapping);

	/* NB: we're sure to have correct a_ops only after f_op->open */
	if (f->f_flags & O_DIRECT) {
		if (!f->f_mapping->a_ops ||
		    ((!f->f_mapping->a_ops->direct_IO) &&
		    (!f->f_mapping->a_ops->get_xip_mem))) {
			fput(f);
			f = ERR_PTR(-EINVAL);
		}
	}

	return f;

cleanup_all:
	fops_put(f->f_op);
	if (f->f_mode & FMODE_WRITE) {
		put_write_access(inode);
		if (!special_file(inode->i_mode)) {
			/*
			 * We don't consider this a real
			 * mnt_want/drop_write() pair
			 * because it all happenend right
			 * here, so just reset the state.
			 */
			file_reset_write(f);
			__mnt_drop_write(mnt);
		}
	}
	file_sb_list_del(f);
	f->f_path.dentry = NULL;
	f->f_path.mnt = NULL;
cleanup_file:
	put_filp(f);
	dput(dentry);
	mntput(mnt);
	return ERR_PTR(error);
}

/**
 * lookup_instantiate_filp - instantiates the open intent filp
 * @nd: pointer to nameidata
 * @dentry: pointer to dentry
 * @open: open callback
 *
 * Helper for filesystems that want to use lookup open intents and pass back
 * a fully instantiated struct file to the caller.
 * This function is meant to be called from within a filesystem's
 * lookup method.
 * Beware of calling it for non-regular files! Those ->open methods might block
 * (e.g. in fifo_open), leaving you with parent locked (and in case of fifo,
 * leading to a deadlock, as nobody can open that fifo anymore, because
 * another process to open fifo will block on locked parent when doing lookup).
 * Note that in case of error, nd->intent.open.file is destroyed, but the
 * path information remains valid.
 * If the open callback is set to NULL, then the standard f_op->open()
 * filesystem callback is substituted.
 */
struct file *lookup_instantiate_filp(struct nameidata *nd, struct dentry *dentry,
		int (*open)(struct inode *, struct file *))
{
	const struct cred *cred = current_cred();

	if (IS_ERR(nd->intent.open.file))
		goto out;
	if (IS_ERR(dentry))
		goto out_err;
	nd->intent.open.file = __dentry_open(dget(dentry), mntget(nd->path.mnt),
					     nd->intent.open.file,
					     open, cred);
out:
	return nd->intent.open.file;
out_err:
	release_open_intent(nd);
	nd->intent.open.file = (struct file *)dentry;
	goto out;
}
EXPORT_SYMBOL_GPL(lookup_instantiate_filp);

/**
 * nameidata_to_filp - convert a nameidata to an open filp.
 * @nd: pointer to nameidata
 * @flags: open flags
 *
 * Note that this function destroys the original nameidata
 */
struct file *nameidata_to_filp(struct nameidata *nd)
{
	const struct cred *cred = current_cred();
	struct file *filp;

	/* Pick up the filp from the open intent */
	filp = nd->intent.open.file;
	/* Has the filesystem initialised the file for us? */
	if (filp->f_path.dentry == NULL)
		filp = __dentry_open(nd->path.dentry, nd->path.mnt, filp,
				     NULL, cred);
	else
		path_put(&nd->path);
	return filp;
}
EXPORT_SYMBOL_GPL(nameidata_to_filp);

/*
 * dentry_open() will have done dput(dentry) and mntput(mnt) if it returns an
 * error.
 */
struct file *dentry_open(struct dentry *dentry, struct vfsmount *mnt, int flags,
			 const struct cred *cred)
{
	int error;
	struct file *f;

	validate_creds(cred);

	/*
	 * We must always pass in a valid mount pointer.   Historically
	 * callers got away with not passing it, but we must enforce this at
	 * the earliest possible point now to avoid strange problems deep in the
	 * filesystem stack.
	 */
	if (!mnt) {
		printk(KERN_WARNING "%s called with NULL vfsmount\n", __func__);
		dump_stack();
		return ERR_PTR(-EINVAL);
	}

	error = -ENFILE;
	f = get_empty_filp();
	if (f == NULL) {
		dput(dentry);
		mntput(mnt);
		return ERR_PTR(error);
	}

	f->f_flags = flags;
	return __dentry_open(dentry, mnt, f, NULL, cred);
}
EXPORT_SYMBOL(dentry_open);

static void __put_unused_fd(struct files_struct *files, unsigned int fd)
{
	struct fdtable *fdt = files_fdtable(files);
	__FD_CLR(fd, fdt->open_fds);
	if (fd < files->next_fd)
		files->next_fd = fd;
}

void put_unused_fd(unsigned int fd)
{
	struct files_struct *files = current->files;
	spin_lock(&files->file_lock);
	__put_unused_fd(files, fd);
	spin_unlock(&files->file_lock);
}

EXPORT_SYMBOL(put_unused_fd);

/*
 * Install a file pointer in the fd array.
 *
 * The VFS is full of places where we drop the files lock between
 * setting the open_fds bitmap and installing the file in the file
 * array.  At any such point, we are vulnerable to a dup2() race
 * installing a file in the array before us.  We need to detect this and
 * fput() the struct file we are about to overwrite in this case.
 *
 * It should never happen - if we allow dup2() do it, _really_ bad things
 * will follow.
 */

void fd_install(unsigned int fd, struct file *file)
{
	struct files_struct *files = current->files;
	struct fdtable *fdt;
	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	BUG_ON(fdt->fd[fd] != NULL);
	rcu_assign_pointer(fdt->fd[fd], file);
	spin_unlock(&files->file_lock);
}

EXPORT_SYMBOL(fd_install);

long do_sys_open(int dfd, const char __user *filename, int flags, int mode)
{
	struct filename *tmp = getname(filename);
	int fd = PTR_ERR(tmp);

	if (!IS_ERR(tmp)) {
		fd = get_unused_fd_flags(flags);
		if (fd >= 0) {
			struct file *f = do_filp_open(dfd, tmp, flags, mode, 0);
			if (IS_ERR(f)) {
				put_unused_fd(fd);
				fd = PTR_ERR(f);
			} else {
				fsnotify_open(f->f_path.dentry);
				fd_install(fd, f);
			}
		}
		putname(tmp);
	}
	return fd;
}

SYSCALL_DEFINE3(open, const char __user *, filename, int, flags, int, mode)
{
	long ret;

	if (force_o_largefile())
		flags |= O_LARGEFILE;

	ret = do_sys_open(AT_FDCWD, filename, flags, mode);
	/* avoid REGPARM breakage on x86: */
	asmlinkage_protect(3, ret, filename, flags, mode);
	return ret;
}
EXPORT_SYMBOL(sys_open);

SYSCALL_DEFINE4(openat, int, dfd, const char __user *, filename, int, flags,
		int, mode)
{
	long ret;

	if (force_o_largefile())
		flags |= O_LARGEFILE;

	ret = do_sys_open(dfd, filename, flags, mode);
	/* avoid REGPARM breakage on x86: */
	asmlinkage_protect(4, ret, dfd, filename, flags, mode);
	return ret;
}

#ifndef __alpha__

/*
 * For backward compatibility?  Maybe this should be moved
 * into arch/i386 instead?
 */
SYSCALL_DEFINE2(creat, const char __user *, pathname, int, mode)
{
	return sys_open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

#endif

/*
 * "id" is the POSIX thread ID. We use the
 * files pointer for this..
 */
int filp_close(struct file *filp, fl_owner_t id)
{
	int retval = 0;

	if (!file_count(filp)) {
		printk(KERN_ERR "VFS: Close: file count is 0\n");
		return 0;
	}

	if (filp->f_op && filp->f_op->flush)
		retval = filp->f_op->flush(filp, id);

	dnotify_flush(filp, id);
	locks_remove_posix(filp, id);
	fput(filp);
	return retval;
}

EXPORT_SYMBOL(filp_close);

/*
 * Careful here! We test whether the file pointer is NULL before
 * releasing the fd. This ensures that one clone task can't release
 * an fd while another clone is opening it.
 */
SYSCALL_DEFINE1(close, unsigned int, fd)
{
	struct file * filp;
	struct files_struct *files = current->files;
	struct fdtable *fdt;
	int retval;

	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	if (fd >= fdt->max_fds)
		goto out_unlock;
	filp = fdt->fd[fd];
	if (!filp)
		goto out_unlock;
	rcu_assign_pointer(fdt->fd[fd], NULL);
	FD_CLR(fd, fdt->close_on_exec);
	__put_unused_fd(files, fd);
	spin_unlock(&files->file_lock);
	retval = filp_close(filp, files);

	/* can't restart close syscall because file table entry was cleared */
	if (unlikely(retval == -ERESTARTSYS ||
		     retval == -ERESTARTNOINTR ||
		     retval == -ERESTARTNOHAND ||
		     retval == -ERESTART_RESTARTBLOCK))
		retval = -EINTR;

	return retval;

out_unlock:
	spin_unlock(&files->file_lock);
	return -EBADF;
}
EXPORT_SYMBOL(sys_close);

/*
 * This routine simulates a hangup on the tty, to arrange that users
 * are given clean terminals at login time.
 */
SYSCALL_DEFINE0(vhangup)
{
	if (capable(CAP_SYS_TTY_CONFIG)) {
		tty_vhangup_self();
		return 0;
	}
	return -EPERM;
}

/*
 * Called when an inode is about to be open.
 * We use this to disallow opening large files on 32bit systems if
 * the caller didn't specify O_LARGEFILE.  On 64bit systems we force
 * on this flag in sys_open.
 */
int generic_file_open(struct inode * inode, struct file * filp)
{
	if (!(filp->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EOVERFLOW;
	return 0;
}

EXPORT_SYMBOL(generic_file_open);

/*
 * This is used by subsystems that don't want seekable
 * file descriptors
 */
int nonseekable_open(struct inode *inode, struct file *filp)
{
	filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);
	return 0;
}

EXPORT_SYMBOL(nonseekable_open);

/*
 * require inode->i_mutex or unreachable inode
 */
int open_inode_peer(struct inode *inode, struct path *path,
		    const struct cred *cred)
{
	struct inode *peer = path->dentry->d_inode;
	struct address_space *mapping;
	struct file *file;
	struct user_beancounter *cur_ub;
	const struct cred *cur_cred;
	int err;

	/*
	 * We cannot open peers in the middle of transaction,
	 * this shouldn't happens at all.
	 */
	err = -EDEADLK;
	if (WARN_ON_ONCE(current->journal_info))
		goto out_err;

	err = -EBUSY;
	if (inode->i_peer_file)
		goto out_err;

	err = -EINVAL;
	if (!S_ISREG(inode->i_mode) || !S_ISREG(peer->i_mode) ||
	    peer == inode || i_size_read(peer) != i_size_read(inode))
		goto out_err;

restart:
	rcu_read_lock();
	file = rcu_dereference(peer->i_peer_file);
	if (file && file->f_mapping != peer->i_mapping) {
		rcu_read_unlock();
		err = -EMLINK;
		goto out_err;
	}
	if (file && atomic_long_inc_not_zero(&file->f_count)) {
		rcu_read_unlock();
		path_put(path);
		goto install;
	}
	rcu_read_unlock();

	cur_ub = set_exec_ub(&ub0);
	cur_cred = override_creds(cred);
	file = dentry_open(path->dentry, path->mnt,
			   O_RDONLY|O_LARGEFILE, cred);
	revert_creds(cur_cred);
	set_exec_ub(cur_ub);
	if (IS_ERR(file))
		return PTR_ERR(file);

	spin_lock(&peer->i_lock);
	if (atomic_read(&peer->i_writecount) > 0) {
		spin_unlock(&peer->i_lock);
		fput(file);
		return -ETXTBSY;
	}
	if (peer->i_size != inode->i_size) {
		spin_unlock(&peer->i_lock);
		fput(file);
		return -EINVAL;
	}
	if (peer->i_peer_file && file_count(peer->i_peer_file)) {
		spin_unlock(&peer->i_lock);
		*path = file->f_path;
		path_get(path);
		fput(file);
		goto restart;
	}
	atomic_dec(&peer->i_writecount);
	rcu_assign_pointer(peer->i_peer_file, file);
	spin_unlock(&peer->i_lock);

install:
	spin_lock(&inode->i_lock);
	if (inode->i_peer_file)
		goto undo;
	rcu_assign_pointer(inode->i_peer_file, file);
	spin_unlock(&inode->i_lock);

	mapping = file->f_mapping;
	spin_lock(&mapping->i_mmap_lock);
	list_add(&inode->i_mapping->i_peer_list, &mapping->i_peer_list);
	spin_unlock(&mapping->i_mmap_lock);

	/* update peer atime */
	file_accessed(file);

	/* prune unused pages */
	invalidate_mapping_pages(inode->i_mapping, 0, -1);

	return 0;

undo:
	spin_unlock(&inode->i_lock);
	peer_fput(file);

	return -EBUSY;

out_err:
	path_put(path);
	return err;
}
EXPORT_SYMBOL(open_inode_peer);

static void __peer_fput(struct work_struct *work)
{
	struct file *file = container_of(work, struct file, f_work);

	/* update peer atime */
	file_accessed(file);

	__fput(file);
}

void peer_fput(struct file *file)
{
	if (atomic_long_dec_and_test(&file->f_count)) {
		struct inode *peer = file->f_mapping->host;

		spin_lock(&peer->i_lock);
		if (peer->i_peer_file == file)
			rcu_assign_pointer(peer->i_peer_file, NULL);
		atomic_inc(&peer->i_writecount);
		spin_unlock(&peer->i_lock);

		/*
		 * We cannot fput file if we are in the middle of
		 * fs-transaction, so schedule this fput to keventd.
		 */
		if (current->journal_info) {
			INIT_WORK(&file->f_work, __peer_fput);
			schedule_work(&file->f_work);
		} else
			__peer_fput(&file->f_work);
	}
}

/*
 * require inode->i_mutex or unreachable inode
 */
void close_inode_peer(struct inode *inode)
{
	struct address_space *mapping = inode->i_mapping;
	struct file *file = inode->i_peer_file;

	if (!file)
		return;

	spin_lock(&inode->i_lock);
	BUG_ON(inode->i_peer_file != file);
	BUG_ON(file->f_mapping == mapping);
	rcu_assign_pointer(inode->i_peer_file, NULL);
	spin_unlock(&inode->i_lock);

	if (mapping_mapped(mapping)) {
		struct zap_details details = {
			.check_mapping = file->f_mapping,
			.first_index = 0,
			.last_index = ULONG_MAX,
		};

		synchronize_mapping_faults(mapping);
		zap_mapping_range(mapping, &details);
	}

	mapping = file->f_mapping;
	spin_lock(&mapping->i_mmap_lock);
	list_del_init(&inode->i_mapping->i_peer_list);
	spin_unlock(&mapping->i_mmap_lock);

	peer_fput(file);
}
EXPORT_SYMBOL(close_inode_peer);
