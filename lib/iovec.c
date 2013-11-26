#include <linux/uaccess.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/uio.h>

/*
 *	Copy iovec to kernel. Returns -EFAULT on error.
 *
 *	Note: this modifies the original iovec.
 */

int memcpy_fromiovec(unsigned char *kdata, struct iovec *iov, int len)
{
	while (len > 0) {
		if (iov->iov_len) {
			int copy = min_t(unsigned int, len, iov->iov_len);
			if (copy_from_user(kdata, iov->iov_base, copy))
				return -EFAULT;
			len -= copy;
			kdata += copy;
			iov->iov_base += copy;
			iov->iov_len -= copy;
		}
		iov++;
	}

	return 0;
}
EXPORT_SYMBOL(memcpy_fromiovec);

/*
 *	Copy kernel to iovec. Returns -EFAULT on error.
 */

int memcpy_toiovecend(const struct iovec *iov, unsigned char *kdata,
		      int offset, int len)
{
	int copy;
	for (; len > 0; ++iov) {
		/* Skip over the finished iovecs */
		if (unlikely(offset >= iov->iov_len)) {
			offset -= iov->iov_len;
			continue;
		}
		copy = min_t(unsigned int, iov->iov_len - offset, len);
		if (copy_to_user(iov->iov_base + offset, kdata, copy))
			return -EFAULT;
		offset = 0;
		kdata += copy;
		len -= copy;
	}

	return 0;
}
EXPORT_SYMBOL(memcpy_toiovecend);

/*
 *	Copy iovec to kernel. Returns -EFAULT on error.
 */

int memcpy_fromiovecend(unsigned char *kdata, const struct iovec *iov,
			int offset, int len)
{
	/* No data? Done! */
	if (len == 0)
		return 0;

	/* Skip over the finished iovecs */
	while (offset >= iov->iov_len) {
		offset -= iov->iov_len;
		iov++;
	}

	while (len > 0) {
		u8 __user *base = iov->iov_base + offset;
		int copy = min_t(unsigned int, len, iov->iov_len - offset);

		offset = 0;
		if (copy_from_user(kdata, base, copy))
			return -EFAULT;
		len -= copy;
		kdata += copy;
		iov++;
	}

	return 0;
}
EXPORT_SYMBOL(memcpy_fromiovecend);

int iov_count_pages(const struct iov_iter *iter, unsigned align)
{
	struct iov_iter i = *iter;
	int nr_pages = 0;

	while (iov_iter_count(&i)) {
		unsigned long uaddr = (unsigned long) i.iov->iov_base +
			i.iov_offset;
		unsigned long len = i.iov->iov_len - i.iov_offset;

		if ((uaddr & align) || (len & align))
			return -EINVAL;

		/*
		 * Overflow, abort
		 */
		if (uaddr + len < uaddr)
			return -EINVAL;

		nr_pages += DIV_ROUND_UP(len + offset_in_page(uaddr),
					 PAGE_SIZE);
		iov_iter_advance(&i, len);
	}

	return nr_pages;
}
EXPORT_SYMBOL(iov_count_pages);
