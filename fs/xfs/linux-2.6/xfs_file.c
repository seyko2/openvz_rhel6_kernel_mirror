/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_bit.h"
#include "xfs_log.h"
#include "xfs_inum.h"
#include "xfs_sb.h"
#include "xfs_ag.h"
#include "xfs_trans.h"
#include "xfs_mount.h"
#include "xfs_bmap_btree.h"
#include "xfs_alloc.h"
#include "xfs_dinode.h"
#include "xfs_inode.h"
#include "xfs_inode_item.h"
#include "xfs_bmap.h"
#include "xfs_error.h"
#include "xfs_vnodeops.h"
#include "xfs_da_btree.h"
#include "xfs_ioctl.h"
#include "xfs_trace.h"

#include <linux/dcache.h>

static const struct vm_operations_struct xfs_file_vm_ops;

/*
 * Locking primitives for read and write IO paths to ensure we consistently use
 * and order the inode->i_mutex, ip->i_lock and ip->i_iolock.
 */
static inline void
xfs_rw_ilock(
	struct xfs_inode	*ip,
	int			type)
{
	if (type & XFS_IOLOCK_EXCL)
		mutex_lock(&VFS_I(ip)->i_mutex);
	xfs_ilock(ip, type);
}

static inline void
xfs_rw_iunlock(
	struct xfs_inode	*ip,
	int			type)
{
	xfs_iunlock(ip, type);
	if (type & XFS_IOLOCK_EXCL)
		mutex_unlock(&VFS_I(ip)->i_mutex);
}

static inline void
xfs_rw_ilock_demote(
	struct xfs_inode	*ip,
	int			type)
{
	xfs_ilock_demote(ip, type);
	if (type & XFS_IOLOCK_EXCL)
		mutex_unlock(&VFS_I(ip)->i_mutex);
}

/*
 *	xfs_iozero
 *
 *	xfs_iozero clears the specified range of buffer supplied,
 *	and marks all the affected blocks as valid and modified.  If
 *	an affected block is not allocated, it will be allocated.  If
 *	an affected block is not completely overwritten, and is not
 *	valid before the operation, it will be read from disk before
 *	being partially zeroed.
 */
STATIC int
xfs_iozero(
	struct xfs_inode	*ip,	/* inode			*/
	loff_t			pos,	/* offset in file		*/
	size_t			count)	/* size of data to zero		*/
{
	struct page		*page;
	struct address_space	*mapping;
	int			status;

	mapping = VFS_I(ip)->i_mapping;
	do {
		unsigned offset, bytes;
		void *fsdata;

		offset = (pos & (PAGE_CACHE_SIZE -1)); /* Within page */
		bytes = PAGE_CACHE_SIZE - offset;
		if (bytes > count)
			bytes = count;

		status = pagecache_write_begin(NULL, mapping, pos, bytes,
					AOP_FLAG_UNINTERRUPTIBLE,
					&page, &fsdata);
		if (status)
			break;

		zero_user(page, offset, bytes);

		status = pagecache_write_end(NULL, mapping, pos, bytes, bytes,
					page, fsdata);
		WARN_ON(status <= 0); /* can't return less than zero! */
		pos += bytes;
		count -= bytes;
		status = 0;
	} while (count);

	return (-status);
}

/*
 * Fsync operations on directories are much simpler than on regular files,
 * as there is no file data to flush, and thus also no need for explicit
 * cache flush operations, and there are no non-transaction metadata updates
 * on directories either.
 */
STATIC int
xfs_dir_fsync(
	struct file		*file,
	struct dentry		*dentry,
	int			datasync)
{
	struct xfs_inode	*ip = XFS_I(dentry->d_inode);
	struct xfs_mount	*mp = ip->i_mount;
	xfs_lsn_t		lsn = 0;

	trace_xfs_dir_fsync(ip);

	xfs_ilock(ip, XFS_ILOCK_SHARED);
	if (xfs_ipincount(ip))
		lsn = ip->i_itemp->ili_last_lsn;
	xfs_iunlock(ip, XFS_ILOCK_SHARED);

	if (!lsn)
		return 0;
	return _xfs_log_force_lsn(mp, lsn, XFS_LOG_SYNC, NULL);
}

STATIC int
xfs_file_fsync(
	struct file		*file,
	struct dentry		*dentry,
	int			datasync)
{
	struct xfs_inode	*ip = XFS_I(dentry->d_inode);
	struct xfs_mount	*mp = ip->i_mount;
	int			error = 0;
	int			log_flushed = 0;
	xfs_lsn_t		lsn = 0;

	trace_xfs_file_fsync(ip);

	if (XFS_FORCED_SHUTDOWN(mp))
		return -XFS_ERROR(EIO);

	xfs_iflags_clear(ip, XFS_ITRUNCATED);

	xfs_ioend_wait(ip);

	if (mp->m_flags & XFS_MOUNT_BARRIER) {
		/*
		 * If we have an RT and/or log subvolume we need to make sure
		 * to flush the write cache the device used for file data
		 * first.  This is to ensure newly written file data make
		 * it to disk before logging the new inode size in case of
		 * an extending write.
		 */
		if (XFS_IS_REALTIME_INODE(ip))
			xfs_blkdev_issue_flush(mp->m_rtdev_targp);
		else if (mp->m_logdev_targp != mp->m_ddev_targp)
			xfs_blkdev_issue_flush(mp->m_ddev_targp);
	}

	/*
	 * All metadata updates are logged, which means that we just have
	 * to flush the log up to the latest LSN that touched the inode.
	 */
	xfs_ilock(ip, XFS_ILOCK_SHARED);
	if (xfs_ipincount(ip)) {
		if (!datasync ||
		    (ip->i_itemp->ili_fields & ~XFS_ILOG_TIMESTAMP))
			lsn = ip->i_itemp->ili_last_lsn;
	}
	xfs_iunlock(ip, XFS_ILOCK_SHARED);

	if (lsn)
		error = _xfs_log_force_lsn(mp, lsn, XFS_LOG_SYNC, &log_flushed);

	/*
	 * If we only have a single device, and the log force about was
	 * a no-op we might have to flush the data device cache here.
	 * This can only happen for fdatasync/O_DSYNC if we were overwriting
	 * an already allocated file and thus do not have any metadata to
	 * commit.
	 */
	if ((mp->m_flags & XFS_MOUNT_BARRIER) &&
	    mp->m_logdev_targp == mp->m_ddev_targp &&
	    !XFS_IS_REALTIME_INODE(ip) &&
	    !log_flushed)
		xfs_blkdev_issue_flush(mp->m_ddev_targp);

	return -error;
}

STATIC ssize_t
xfs_file_aio_read(
	struct kiocb		*iocb,
	const struct iovec	*iovp,
	unsigned long		nr_segs,
	loff_t			pos)
{
	struct file		*file = iocb->ki_filp;
	struct inode		*inode = file->f_mapping->host;
	struct xfs_inode	*ip = XFS_I(inode);
	struct xfs_mount	*mp = ip->i_mount;
	size_t			size = 0;
	ssize_t			ret = 0;
	int			ioflags = 0;
	xfs_fsize_t		n;

	XFS_STATS_INC(xs_read_calls);

	BUG_ON(iocb->ki_pos != pos);

	if (unlikely(file->f_flags & O_DIRECT))
		ioflags |= IO_ISDIRECT;
	if (file->f_mode & FMODE_NOCMTIME)
		ioflags |= IO_INVIS;

	ret = generic_segment_checks(iovp, &nr_segs, &size, VERIFY_WRITE);
	if (ret < 0)
		return ret;

	if (unlikely(ioflags & IO_ISDIRECT)) {
		xfs_buftarg_t	*target =
			XFS_IS_REALTIME_INODE(ip) ?
				mp->m_rtdev_targp : mp->m_ddev_targp;
		/* DIO must be aligned to device logical sector size */
		if ((iocb->ki_pos | size) & target->bt_logical_sectormask) {
			if (iocb->ki_pos == i_size_read(inode))
				return 0;
			return -XFS_ERROR(EINVAL);
		}
	}

	n = XFS_MAXIOFFSET(mp) - iocb->ki_pos;
	if (n <= 0 || size == 0)
		return 0;

	if (n < size)
		size = n;

	if (XFS_FORCED_SHUTDOWN(mp))
		return -EIO;

	/*
	 * Locking is a bit tricky here. If we take an exclusive lock
	 * for direct IO, we effectively serialise all new concurrent
	 * read IO to this file and block it behind IO that is currently in
	 * progress because IO in progress holds the IO lock shared. We only
	 * need to hold the lock exclusive to blow away the page cache, so
	 * only take lock exclusively if the page cache needs invalidation.
	 * This allows the normal direct IO case of no page cache pages to
	 * proceeed concurrently without serialisation.
	 */
	xfs_rw_ilock(ip, XFS_IOLOCK_SHARED);
	if ((ioflags & IO_ISDIRECT) && inode->i_mapping->nrpages) {
		xfs_rw_iunlock(ip, XFS_IOLOCK_SHARED);
		xfs_rw_ilock(ip, XFS_IOLOCK_EXCL);

		if (inode->i_mapping->nrpages) {
			ret = -xfs_flushinval_pages(ip,
					(iocb->ki_pos & PAGE_CACHE_MASK),
					-1, FI_REMAPF_LOCKED);
			if (ret) {
				xfs_rw_iunlock(ip, XFS_IOLOCK_EXCL);
				return ret;
			}
		}
		xfs_rw_ilock_demote(ip, XFS_IOLOCK_EXCL);
	}

	trace_xfs_file_read(ip, size, iocb->ki_pos, ioflags);

	ret = generic_file_aio_read(iocb, iovp, nr_segs, iocb->ki_pos);
	if (ret > 0)
		XFS_STATS_ADD(xs_read_bytes, ret);

	xfs_rw_iunlock(ip, XFS_IOLOCK_SHARED);
	return ret;
}

STATIC ssize_t
xfs_file_splice_read(
	struct file		*infilp,
	loff_t			*ppos,
	struct pipe_inode_info	*pipe,
	size_t			count,
	unsigned int		flags)
{
	struct xfs_inode	*ip = XFS_I(infilp->f_mapping->host);
	int			ioflags = 0;
	ssize_t			ret;

	XFS_STATS_INC(xs_read_calls);

	if (infilp->f_mode & FMODE_NOCMTIME)
		ioflags |= IO_INVIS;

	if (XFS_FORCED_SHUTDOWN(ip->i_mount))
		return -EIO;

	xfs_rw_ilock(ip, XFS_IOLOCK_SHARED);

	trace_xfs_file_splice_read(ip, count, *ppos, ioflags);

	ret = generic_file_splice_read(infilp, ppos, pipe, count, flags);
	if (ret > 0)
		XFS_STATS_ADD(xs_read_bytes, ret);

	xfs_rw_iunlock(ip, XFS_IOLOCK_SHARED);
	return ret;
}

/*
 * xfs_file_splice_write() does not use xfs_rw_ilock() because
 * generic_file_splice_write() takes the i_mutex itself. This, in theory,
 * couuld cause lock inversions between the aio_write path and the splice path
 * if someone is doing concurrent splice(2) based writes and write(2) based
 * writes to the same inode. The only real way to fix this is to re-implement
 * the generic code here with correct locking orders.
 */
STATIC ssize_t
xfs_file_splice_write(
	struct pipe_inode_info	*pipe,
	struct file		*outfilp,
	loff_t			*ppos,
	size_t			count,
	unsigned int		flags)
{
	struct inode		*inode = outfilp->f_mapping->host;
	struct xfs_inode	*ip = XFS_I(inode);
	int			ioflags = 0;
	ssize_t			ret;

	XFS_STATS_INC(xs_write_calls);

	if (outfilp->f_mode & FMODE_NOCMTIME)
		ioflags |= IO_INVIS;

	if (XFS_FORCED_SHUTDOWN(ip->i_mount))
		return -EIO;

	xfs_ilock(ip, XFS_IOLOCK_EXCL);

	trace_xfs_file_splice_write(ip, count, *ppos, ioflags);

	ret = generic_file_splice_write(pipe, outfilp, ppos, count, flags);
	if (ret > 0)
		XFS_STATS_ADD(xs_write_bytes, ret);

	xfs_iunlock(ip, XFS_IOLOCK_EXCL);
	return ret;
}

/*
 * This routine is called to handle zeroing any space in the last block of the
 * file that is beyond the EOF.  We do this since the size is being increased
 * without writing anything to that block and we don't want to read the
 * garbage on the disk.
 */
STATIC int				/* error (positive) */
xfs_zero_last_block(
	struct xfs_inode	*ip,
	xfs_fsize_t		offset,
	xfs_fsize_t		isize)
{
	struct xfs_mount	*mp = ip->i_mount;
	xfs_fileoff_t		last_fsb = XFS_B_TO_FSBT(mp, isize);
	int			zero_offset = XFS_B_FSB_OFFSET(mp, isize);
	int			zero_len;
	int			nimaps = 1;
	int			error = 0;
	struct xfs_bmbt_irec	imap;

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	last_fsb = XFS_B_TO_FSBT(mp, isize);
	nimaps = 1;
	error = xfs_bmapi(NULL, ip, last_fsb, 1, 0, NULL, 0, &imap,
			  &nimaps, NULL);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	if (error)
		return error;

	ASSERT(nimaps > 0);

	/*
	 * If the block underlying isize is just a hole, then there
	 * is nothing to zero.
	 */
	if (imap.br_startblock == HOLESTARTBLOCK)
		return 0;

	zero_len = mp->m_sb.sb_blocksize - zero_offset;
	if (isize + zero_len > offset)
		zero_len = offset - isize;
	return xfs_iozero(ip, isize, zero_len);
}

/*
 * Zero any on disk space between the current EOF and the new, larger EOF.
 *
 * This handles the normal case of zeroing the remainder of the last block in
 * the file and the unusual case of zeroing blocks out beyond the size of the
 * file.  This second case only happens with fixed size extents and when the
 * system crashes before the inode size was updated but after blocks were
 * allocated.
 *
 * Expects the iolock to be held exclusive, and will take the ilock internally.
 */
int					/* error (positive) */
xfs_zero_eof(
	struct xfs_inode	*ip,
	xfs_off_t		offset,		/* starting I/O offset */
	xfs_fsize_t		isize)		/* current inode size */
{
	struct xfs_mount	*mp = ip->i_mount;
	xfs_fileoff_t		start_zero_fsb;
	xfs_fileoff_t		end_zero_fsb;
	xfs_fileoff_t		zero_count_fsb;
	xfs_fileoff_t		last_fsb;
	xfs_fileoff_t		zero_off;
	xfs_fsize_t		zero_len;
	int			nimaps;
	int			error = 0;
	struct xfs_bmbt_irec	imap;

	ASSERT(xfs_isilocked(ip, XFS_IOLOCK_EXCL));
	ASSERT(offset > isize);

	/*
	 * First handle zeroing the block on which isize resides.
	 *
	 * We only zero a part of that block so it is handled specially.
	 */
	if (XFS_B_FSB_OFFSET(mp, isize) != 0) {
		error = xfs_zero_last_block(ip, offset, isize);
		if (error)
			return error;
	}

	/*
	 * Calculate the range between the new size and the old where blocks
	 * needing to be zeroed may exist.
	 *
	 * To get the block where the last byte in the file currently resides,
	 * we need to subtract one from the size and truncate back to a block
	 * boundary.  We subtract 1 in case the size is exactly on a block
	 * boundary.
	 */
	last_fsb = isize ? XFS_B_TO_FSBT(mp, isize - 1) : (xfs_fileoff_t)-1;
	start_zero_fsb = XFS_B_TO_FSB(mp, (xfs_ufsize_t)isize);
	end_zero_fsb = XFS_B_TO_FSBT(mp, offset - 1);
	ASSERT((xfs_sfiloff_t)last_fsb < (xfs_sfiloff_t)start_zero_fsb);
	if (last_fsb == end_zero_fsb) {
		/*
		 * The size was only incremented on its last block.
		 * We took care of that above, so just return.
		 */
		return 0;
	}

	ASSERT(start_zero_fsb <= end_zero_fsb);
	while (start_zero_fsb <= end_zero_fsb) {
		nimaps = 1;
		zero_count_fsb = end_zero_fsb - start_zero_fsb + 1;

		xfs_ilock(ip, XFS_ILOCK_EXCL);
		error = xfs_bmapi(NULL, ip, start_zero_fsb, zero_count_fsb,
				  0, NULL, 0, &imap, &nimaps, NULL);
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
		if (error)
			return error;

		ASSERT(nimaps > 0);

		if (imap.br_state == XFS_EXT_UNWRITTEN ||
		    imap.br_startblock == HOLESTARTBLOCK) {
			start_zero_fsb = imap.br_startoff + imap.br_blockcount;
			ASSERT(start_zero_fsb <= (end_zero_fsb + 1));
			continue;
		}

		/*
		 * There are blocks we need to zero.
		 */
		zero_off = XFS_FSB_TO_B(mp, start_zero_fsb);
		zero_len = XFS_FSB_TO_B(mp, imap.br_blockcount);

		if ((zero_off + zero_len) > offset)
			zero_len = offset - zero_off;

		error = xfs_iozero(ip, zero_off, zero_len);
		if (error)
			return error;

		start_zero_fsb = imap.br_startoff + imap.br_blockcount;
		ASSERT(start_zero_fsb <= (end_zero_fsb + 1));
	}

	return 0;
}

/*
 * Common pre-write limit and setup checks.
 *
 * Called with the iolocked held either shared and exclusive according to
 * @iolock, and returns with it held.  Might upgrade the iolock to exclusive
 * if called for a direct write beyond i_size.
 */
STATIC ssize_t
xfs_file_aio_write_checks(
	struct file		*file,
	loff_t			*pos,
	size_t			*count,
	int			*iolock)
{
	struct inode		*inode = file->f_mapping->host;
	struct xfs_inode	*ip = XFS_I(inode);
	int			error = 0;
	unsigned long		flags;

restart:
	error = generic_write_checks(file, pos, count, S_ISBLK(inode->i_mode));
	if (error)
		return error;

	/*
	 * If the offset is beyond the size of the file, we need to zero any
	 * blocks that fall between the existing EOF and the start of this
	 * write.  If zeroing is needed and we are currently holding the
	 * iolock shared, we need to update it to exclusive which implies
	 * having to redo all checks before.
	 *
	 * We need to serialise against EOF updates that occur in IO
	 * completions here. We want to make sure that nobody is changing the
	 * size while we do this check until we have placed an IO barrier (i.e.
	 * hold the XFS_IOLOCK_EXCL) that prevents new IO from being dispatched.
	 * The spinlock effectively forms a memory barrier once we have the
	 * XFS_IOLOCK_EXCL so we are guaranteed to see the latest EOF value
	 * and hence be able to correctly determine if we need to run zeroing.
	 */
	spin_lock_irqsave(&ip->i_size_lock, flags);
	if (*pos > i_size_read(inode)) {
		spin_unlock_irqrestore(&ip->i_size_lock, flags);
		if (*iolock == XFS_IOLOCK_SHARED) {
			xfs_rw_iunlock(ip, *iolock);
			*iolock = XFS_IOLOCK_EXCL;
			xfs_rw_ilock(ip, *iolock);
			goto restart;
		}
		error = -xfs_zero_eof(ip, *pos, i_size_read(inode));
		if (error)
			return error;
	} else
		spin_unlock_irqrestore(&ip->i_size_lock, flags);

	/*
	 * Updating the timestamps will grab the ilock again from
	 * xfs_fs_dirty_inode, so we have to call it after dropping the
	 * lock above.  Eventually we should look into a way to avoid
	 * the pointless lock roundtrip.
	 */
	if (likely(!(file->f_mode & FMODE_NOCMTIME)))
		file_update_time(file);

	/*
	 * If we're writing the file then make sure to clear the setuid and
	 * setgid bits if the process is not being run by root.  This keeps
	 * people from modifying setuid and setgid binaries.
	 */
	return file_remove_suid(file);
}

/*
 * xfs_file_dio_aio_write - handle direct IO writes
 *
 * Lock the inode appropriately to prepare for and issue a direct IO write.
 * By separating it from the buffered write path we remove all the tricky to
 * follow locking changes and looping.
 *
 * If there are cached pages or we're extending the file, we need IOLOCK_EXCL
 * until we're sure the bytes at the new EOF have been zeroed and/or the cached
 * pages are flushed out.
 *
 * In most cases the direct IO writes will be done holding IOLOCK_SHARED
 * allowing them to be done in parallel with reads and other direct IO writes.
 * However, if the IO is not aligned to filesystem blocks, the direct IO layer
 * needs to do sub-block zeroing and that requires serialisation against other
 * direct IOs to the same block. In this case we need to serialise the
 * submission of the unaligned IOs so that we don't get racing block zeroing in
 * the dio layer.  To avoid the problem with aio, we also need to wait for
 * outstanding IOs to complete so that unwritten extent conversion is completed
 * before we try to map the overlapping block. This is currently implemented by
 * hitting it with a big hammer (i.e. xfs_ioend_wait()).
 *
 * Returns with locks held indicated by @iolock and errors indicated by
 * negative return values.
 */
STATIC ssize_t
xfs_file_dio_aio_write(
	struct kiocb		*iocb,
	const struct iovec	*iovp,
	unsigned long		nr_segs,
	loff_t			pos,
	size_t			ocount)
{
	struct file		*file = iocb->ki_filp;
	struct address_space	*mapping = file->f_mapping;
	struct inode		*inode = mapping->host;
	struct xfs_inode	*ip = XFS_I(inode);
	struct xfs_mount	*mp = ip->i_mount;
	ssize_t			ret = 0;
	size_t			count = ocount;
	int			unaligned_io = 0;
	int			iolock;
	struct xfs_buftarg	*target = XFS_IS_REALTIME_INODE(ip) ?
					mp->m_rtdev_targp : mp->m_ddev_targp;

	/* DIO must be aligned to device logical sector size */
	if ((pos | count) & target->bt_logical_sectormask)
		return -XFS_ERROR(EINVAL);

	/* "unaligned" here means not aligned to a filesystem block */
	if ((pos & mp->m_blockmask) || ((pos + count) & mp->m_blockmask))
		unaligned_io = 1;

	/*
	 * We don't need to take an exclusive lock unless there page cache needs
	 * to be invalidated or unaligned IO is being executed. We don't need to
	 * consider the EOF extension case here because
	 * xfs_file_aio_write_checks() will relock the inode as necessary for
	 * EOF zeroing cases and fill out the new inode size as appropriate.
	 */
	if (unaligned_io || mapping->nrpages)
		iolock = XFS_IOLOCK_EXCL;
	else
		iolock = XFS_IOLOCK_SHARED;
	xfs_rw_ilock(ip, iolock);

	/*
	 * Recheck if there are cached pages that need invalidate after we got
	 * the iolock to protect against other threads adding new pages while
	 * we were waiting for the iolock.
	 */
	if (mapping->nrpages && iolock == XFS_IOLOCK_SHARED) {
		xfs_rw_iunlock(ip, iolock);
		iolock = XFS_IOLOCK_EXCL;
		xfs_rw_ilock(ip, iolock);
	}

	ret = xfs_file_aio_write_checks(file, &pos, &count, &iolock);
	if (ret)
		goto out;

	if (mapping->nrpages) {
		ret = -xfs_flushinval_pages(ip, (pos & PAGE_CACHE_MASK), -1,
							FI_REMAPF_LOCKED);
		if (ret)
			goto out;
	}

	/*
	 * If we are doing unaligned IO, wait for all other IO to drain,
	 * otherwise demote the lock if we had to flush cached pages
	 */
	if (unaligned_io)
		xfs_ioend_wait(ip);
	else if (iolock == XFS_IOLOCK_EXCL) {
		xfs_rw_ilock_demote(ip, XFS_IOLOCK_EXCL);
		iolock = XFS_IOLOCK_SHARED;
	}

	trace_xfs_file_direct_write(ip, count, iocb->ki_pos, 0);
	ret = generic_file_direct_write(iocb, iovp,
			&nr_segs, pos, &iocb->ki_pos, count, ocount);

out:
	xfs_rw_iunlock(ip, iolock);

	/* No fallback to buffered IO on errors for XFS. */
	ASSERT(ret < 0 || ret == count);
	return ret;
}

STATIC ssize_t
xfs_file_buffered_aio_write(
	struct kiocb		*iocb,
	const struct iovec	*iovp,
	unsigned long		nr_segs,
	loff_t			pos,
	size_t			ocount)
{
	struct file		*file = iocb->ki_filp;
	struct address_space	*mapping = file->f_mapping;
	struct inode		*inode = mapping->host;
	struct xfs_inode	*ip = XFS_I(inode);
	ssize_t			ret;
	int			enospc = 0;
	int			iolock = XFS_IOLOCK_EXCL;
	size_t			count = ocount;

	xfs_rw_ilock(ip, iolock);

	ret = xfs_file_aio_write_checks(file, &pos, &count, &iolock);
	if (ret)
		goto out;

	/* We can write back this queue in page reclaim */
	current->backing_dev_info = mapping->backing_dev_info;

write_retry:
	trace_xfs_file_buffered_write(ip, count, iocb->ki_pos, 0);
	ret = generic_file_buffered_write(iocb, iovp, nr_segs,
			pos, &iocb->ki_pos, count, ret);
	/*
	 * if we just got an ENOSPC, flush the inode now we aren't holding any
	 * page locks and retry *once*
	 */
	if (ret == -ENOSPC && !enospc) {
		enospc = 1;
		ret = -xfs_flush_pages(ip, 0, -1, 0, FI_NONE);
		if (!ret)
			goto write_retry;
	}

	current->backing_dev_info = NULL;
out:
	xfs_rw_iunlock(ip, iolock);
	return ret;
}

STATIC ssize_t
xfs_file_aio_write(
	struct kiocb		*iocb,
	const struct iovec	*iovp,
	unsigned long		nr_segs,
	loff_t			pos)
{
	struct file		*file = iocb->ki_filp;
	struct address_space	*mapping = file->f_mapping;
	struct inode		*inode = mapping->host;
	struct xfs_inode	*ip = XFS_I(inode);
	ssize_t			ret;
	size_t			ocount = 0;

	XFS_STATS_INC(xs_write_calls);

	BUG_ON(iocb->ki_pos != pos);

	ret = generic_segment_checks(iovp, &nr_segs, &ocount, VERIFY_READ);
	if (ret)
		return ret;

	if (ocount == 0)
		return 0;

	sb_start_write(inode->i_sb);

	if (XFS_FORCED_SHUTDOWN(ip->i_mount)) {
		ret = -EIO;
		goto out;
	}

	if (unlikely(file->f_flags & O_DIRECT))
		ret = xfs_file_dio_aio_write(iocb, iovp, nr_segs, pos, ocount);
	else
		ret = xfs_file_buffered_aio_write(iocb, iovp, nr_segs, pos,
						  ocount);

	if (ret > 0) {
		ssize_t err;

		XFS_STATS_ADD(xs_write_bytes, ret);

		/* Handle various SYNC-type writes */
		err = generic_write_sync(file, pos, ret);
		if (err < 0)
			ret = err;
	}

out:
	sb_end_write(inode->i_sb);
	return ret;
}

STATIC int
xfs_file_open(
	struct inode	*inode,
	struct file	*file)
{
	if (!(file->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EFBIG;
	if (XFS_FORCED_SHUTDOWN(XFS_M(inode->i_sb)))
		return -EIO;
	return 0;
}

STATIC int
xfs_dir_open(
	struct inode	*inode,
	struct file	*file)
{
	struct xfs_inode *ip = XFS_I(inode);
	int		mode;
	int		error;

	error = xfs_file_open(inode, file);
	if (error)
		return error;

	/*
	 * If there are any blocks, read-ahead block 0 as we're almost
	 * certain to have the next operation be a read there.
	 */
	mode = xfs_ilock_map_shared(ip);
	if (ip->i_d.di_nextents > 0)
		xfs_da_reada_buf(NULL, ip, 0, XFS_DATA_FORK);
	xfs_iunlock(ip, mode);
	return 0;
}

STATIC int
xfs_file_release(
	struct inode	*inode,
	struct file	*filp)
{
	return -xfs_release(XFS_I(inode));
}

STATIC int
xfs_file_readdir(
	struct file	*filp,
	void		*dirent,
	filldir_t	filldir)
{
	struct inode	*inode = filp->f_path.dentry->d_inode;
	xfs_inode_t	*ip = XFS_I(inode);
	int		error;
	size_t		bufsize;

	/*
	 * The Linux API doesn't pass down the total size of the buffer
	 * we read into down to the filesystem.  With the filldir concept
	 * it's not needed for correct information, but the XFS dir2 leaf
	 * code wants an estimate of the buffer size to calculate it's
	 * readahead window and size the buffers used for mapping to
	 * physical blocks.
	 *
	 * Try to give it an estimate that's good enough, maybe at some
	 * point we can change the ->readdir prototype to include the
	 * buffer size.  For now we use the current glibc buffer size.
	 */
	bufsize = (size_t)min_t(loff_t, 32768, ip->i_d.di_size);

	error = xfs_readdir(ip, dirent, bufsize,
				(xfs_off_t *)&filp->f_pos, filldir);
	if (error)
		return -error;
	return 0;
}

STATIC int
xfs_file_mmap(
	struct file	*filp,
	struct vm_area_struct *vma)
{
	vma->vm_ops = &xfs_file_vm_ops;
	vma->vm_flags |= VM_CAN_NONLINEAR;

	file_accessed(filp);
	return 0;
}

/*
 * mmap()d file has taken write protection fault and is being made
 * writable. We can set the page state up correctly for a writable
 * page, which means we can do correct delalloc accounting (ENOSPC
 * checking!) and unwritten extent mapping.
 */
STATIC int
xfs_vm_page_mkwrite(
	struct vm_area_struct	*vma,
	struct vm_fault		*vmf)
{
	return block_page_mkwrite(vma, vmf, xfs_get_blocks);
}

const struct file_operations xfs_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.aio_read	= xfs_file_aio_read,
	.aio_write	= xfs_file_aio_write,
	.splice_read	= xfs_file_splice_read,
	.splice_write	= xfs_file_splice_write,
	.unlocked_ioctl	= xfs_file_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= xfs_file_compat_ioctl,
#endif
	.mmap		= xfs_file_mmap,
	.open		= xfs_file_open,
	.release	= xfs_file_release,
	.fsync		= xfs_file_fsync,
};

const struct file_operations xfs_dir_file_operations = {
	.open		= xfs_dir_open,
	.read		= generic_read_dir,
	.readdir	= xfs_file_readdir,
	.llseek		= generic_file_llseek,
	.unlocked_ioctl	= xfs_file_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= xfs_file_compat_ioctl,
#endif
	.fsync		= xfs_dir_fsync,
};

static const struct vm_operations_struct xfs_file_vm_ops = {
	.fault		= filemap_fault,
	.page_mkwrite	= xfs_vm_page_mkwrite,
};
