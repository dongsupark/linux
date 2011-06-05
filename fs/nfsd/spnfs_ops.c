/*
 * fs/nfsd/spnfs_ops.c
 *
 * Communcation layer between spNFS kernel and userspace
 *
 */
/******************************************************************************

(c) 2007 Network Appliance, Inc.  All Rights Reserved.

Network Appliance provides this source code under the GPL v2 License.
The GPL v2 license is available at
http://opensource.org/licenses/gpl-license.php.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

******************************************************************************/

#include <linux/sched.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/nfs_fs.h>
#include <linux/nfsd4_spnfs.h>
#include <linux/nfsd/debug.h>
#include <linux/nfsd/nfsd4_pnfs.h>
#include <linux/nfsd/nfs4layoutxdr.h>

#include "pnfsd.h"

/* comment out CONFIG_SPNFS_TEST for non-test behaviour */
/* #define CONFIG_SPNFS_TEST 1 */

#define	NFSDDBG_FACILITY		NFSDDBG_PNFS

/*
 * The functions that are called from elsewhere in the kernel
 * to perform tasks in userspace
 *
 */

#ifdef CONFIG_SPNFS_LAYOUTSEGMENTS
extern int spnfs_use_layoutsegments;
extern uint64_t layoutsegment_size;
#endif /* CONFIG_SPNFS_LAYOUTSEGMENTS */
extern struct spnfs *global_spnfs;

int
spnfs_layout_type(struct super_block *sb)
{
	return LAYOUT_NFSV4_1_FILES;
}

enum nfsstat4
spnfs_layoutget(struct inode *inode, struct exp_xdr_stream *xdr,
		const struct nfsd4_pnfs_layoutget_arg *lg_arg,
		struct nfsd4_pnfs_layoutget_res *lg_res)
{
	struct spnfs *spnfs = global_spnfs; /* keep up the pretence */
	struct spnfs_msg *im = NULL;
	union spnfs_msg_res *res = NULL;
	struct pnfs_filelayout_layout *flp = NULL;
	int status, i;
	enum nfsstat4 nfserr;

	im = kmalloc(sizeof(struct spnfs_msg), GFP_KERNEL);
	if (im == NULL) {
		nfserr = NFS4ERR_LAYOUTTRYLATER;
		goto layoutget_cleanup;
	}

	res = kmalloc(sizeof(union spnfs_msg_res), GFP_KERNEL);
	if (res == NULL) {
		nfserr = NFS4ERR_LAYOUTTRYLATER;
		goto layoutget_cleanup;
	}

	im->im_type = SPNFS_TYPE_LAYOUTGET;
	im->im_args.layoutget_args.inode = inode->i_ino;
	im->im_args.layoutget_args.generation = inode->i_generation;

	/* call function to queue the msg for upcall */
	if (spnfs_upcall(spnfs, im, res) != 0) {
		dprintk("failed spnfs upcall: layoutget\n");
		nfserr = NFS4ERR_LAYOUTUNAVAILABLE;
		goto layoutget_cleanup;
	}
	status = res->layoutget_res.status;
	if (status != 0) {
		/* FIXME? until user mode is fixed, translate system error */
		switch (status) {
		case -E2BIG:
		case -ETOOSMALL:
			nfserr = NFS4ERR_TOOSMALL;
			break;
		case -ENOMEM:
		case -EAGAIN:
		case -EINTR:
			nfserr = NFS4ERR_LAYOUTTRYLATER;
			break;
		case -ENOENT:
			nfserr = NFS4ERR_BADLAYOUT;
			break;
 		default:
			nfserr = NFS4ERR_LAYOUTUNAVAILABLE;
		}
		dprintk("spnfs layout_get upcall: status=%d nfserr=%u\n",
			status, nfserr);
		goto layoutget_cleanup;
	}

	lg_res->lg_return_on_close = 0;
#if defined(CONFIG_SPNFS_LAYOUTSEGMENTS)
	/* if spnfs_use_layoutsegments & layoutsegment_size == 0, use */
	/* the amount requested by the client.			      */
	if (spnfs_use_layoutsegments) {
		if (layoutsegment_size != 0)
			lg_res->lg_seg.length = layoutsegment_size;
	} else
		lg_res->lg_seg.length = NFS4_MAX_UINT64;
#else
	lg_res->lg_seg.length = NFS4_MAX_UINT64;
#endif /* CONFIG_SPNFS_LAYOUTSEGMENTS */

	flp = kmalloc(sizeof(struct pnfs_filelayout_layout), GFP_KERNEL);
	if (flp == NULL) {
		nfserr = NFS4ERR_LAYOUTTRYLATER;
		goto layoutget_cleanup;
	}
	flp->device_id.sbid = lg_arg->lg_sbid;
	flp->device_id.devid = res->layoutget_res.devid;
	flp->lg_layout_type = 1; /* XXX */
	flp->lg_stripe_type = res->layoutget_res.stripe_type;
	flp->lg_commit_through_mds = 0;
	flp->lg_stripe_unit =  res->layoutget_res.stripe_size;
	flp->lg_first_stripe_index = 0;
	flp->lg_pattern_offset = 0;
	flp->lg_fh_length = res->layoutget_res.stripe_count;

	flp->lg_fh_list = kmalloc(flp->lg_fh_length * sizeof(struct knfsd_fh),
				  GFP_KERNEL);
	if (flp->lg_fh_list == NULL) {
		nfserr = NFS4ERR_LAYOUTTRYLATER;
		goto layoutget_cleanup;
	}
	/*
	 * FIX: Doing an extra copy here.  Should group res.flist's fh_len
	 * and fh_val into a knfsd_fh structure.
	 */
	for (i = 0; i < flp->lg_fh_length; i++) {
		flp->lg_fh_list[i].fh_size = res->layoutget_res.flist[i].fh_len;
		memcpy(&flp->lg_fh_list[i].fh_base,
		       res->layoutget_res.flist[i].fh_val,
		       res->layoutget_res.flist[i].fh_len);
	}

	/* encode the layoutget body */
	nfserr = filelayout_encode_layout(xdr, flp);

layoutget_cleanup:
	if (flp) {
		if (flp->lg_fh_list)
			kfree(flp->lg_fh_list);
		kfree(flp);
	}
	kfree(im);
	kfree(res);

	return nfserr;
}

int
spnfs_layoutcommit(void)
{
	return 0;
}

int
spnfs_layoutreturn(struct inode *inode,
		   const struct nfsd4_pnfs_layoutreturn_arg *args)
{
	return 0;
}

int
spnfs_layoutrecall(struct inode *inode, int type, u64 offset, u64 len)
{
	struct super_block *sb;
	struct nfsd4_pnfs_cb_layout lr;

	switch (type) {
	case RETURN_FILE:
		sb = inode->i_sb;
		dprintk("%s: recalling layout for ino = %lu\n",
			__func__, inode->i_ino);
		break;
	case RETURN_FSID:
		sb = inode->i_sb;
		dprintk("%s: recalling layout for fsid x (unimplemented)\n",
			__func__);
		return 0;
	case RETURN_ALL:
		/* XXX figure out how to get a sb since there's no inode ptr */
		dprintk("%s: recalling all layouts (unimplemented)\n",
			__func__);
		return 0;
	default:
		return -EINVAL;
	}

	lr.cbl_recall_type = type;
	lr.cbl_seg.layout_type = LAYOUT_NFSV4_1_FILES;
	lr.cbl_seg.clientid = 0;
	lr.cbl_seg.offset = offset;
	lr.cbl_seg.length = len;
	lr.cbl_seg.iomode = IOMODE_ANY;
	lr.cbl_layoutchanged = 0;

	nfsd_layout_recall_cb(sb, inode, &lr);

	return 0;
}


int
spnfs_test_layoutrecall(char *path, u64 offset, u64 len)
{
	struct path p;
	struct inode *inode;
	int type, rc;

	dprintk("%s: path=%s, offset=%llu, len=%llu\n",
		__func__, path, offset, len);

	if (strcmp(path, "all") == 0) {
		inode = NULL;
		type = RETURN_ALL;
	} else {
		rc = kern_path(path, 0, &p);
		if (rc != 0)
			return rc;

		/*
		 * XXX todo: add a RETURN_FSID scenario here...maybe if
		 * inode is a dir...
		 */

		inode = p.dentry->d_inode;
		type = RETURN_FILE;
	}

	if (len == 0)
		len = NFS4_MAX_UINT64;

	rc = spnfs_layoutrecall(inode, type, offset, len);

	if (type != RETURN_ALL)
		path_put(&p);
	return rc;
}

int
spnfs_getdeviceiter(struct super_block *sb,
		    u32 layout_type,
		    struct nfsd4_pnfs_dev_iter_res *gd_res)
{
	struct spnfs *spnfs = global_spnfs;   /* XXX keep up the pretence */
	struct spnfs_msg *im = NULL;
	union spnfs_msg_res *res = NULL;
	int status = 0;

	im = kmalloc(sizeof(struct spnfs_msg), GFP_KERNEL);
	if (im == NULL) {
		status = -ENOMEM;
		goto getdeviceiter_out;
	}

	res = kmalloc(sizeof(union spnfs_msg_res), GFP_KERNEL);
	if (res == NULL) {
		status = -ENOMEM;
		goto getdeviceiter_out;
	}

	im->im_type = SPNFS_TYPE_GETDEVICEITER;
	im->im_args.getdeviceiter_args.cookie = gd_res->gd_cookie;
	im->im_args.getdeviceiter_args.verf = gd_res->gd_verf;

	/* call function to queue the msg for upcall */
	status = spnfs_upcall(spnfs, im, res);
	if (status != 0) {
		dprintk("%s spnfs upcall failure: %d\n", __func__, status);
		status = -EIO;
		goto getdeviceiter_out;
	}
	status = res->getdeviceiter_res.status;

	if (res->getdeviceiter_res.eof)
		gd_res->gd_eof = 1;
	else {
		gd_res->gd_devid = res->getdeviceiter_res.devid;
		gd_res->gd_cookie = res->getdeviceiter_res.cookie;
		gd_res->gd_verf = res->getdeviceiter_res.verf;
		gd_res->gd_eof = 0;
	}

getdeviceiter_out:
	kfree(im);
	kfree(res);

	return status;
}

#ifdef CONFIG_SPNFS_TEST
/*
 * Setup the rq_res xdr_buf.  The svc_rqst rq_respages[1] page contains the
 * 1024 encoded stripe indices.
 *
 * Skip the devaddr4 length and encode the indicies count (1024) in the
 * rq_res.head and set the rq_res.head length.
 *
 * Set the rq_res page_len to 4096 (for the 1024 stripe indices).
 * Set the rq_res xdr_buf tail base to rq_respages[0] just after the
 * rq_res head to hold the rest of the getdeviceinfo return.
 *
 * So rq_respages[rq_resused - 1] contains the rq_res.head and rq_res.tail and
 * rq_respages[rq_resused] contains the rq_res.pages.
 */
static int spnfs_test_indices_xdr(struct pnfs_xdr_info *info,
				  const struct pnfs_filelayout_device *fdev)
{
	struct nfsd4_compoundres *resp = info->resp;
	struct svc_rqst *rqstp = resp->rqstp;
	struct xdr_buf *xb = &resp->rqstp->rq_res;
	__be32 *p;

	p = nfsd4_xdr_reserve_space(resp, 8);
	p++; /* Fill in length later */
	*p++ = cpu_to_be32(fdev->fl_stripeindices_length); /* 1024 */
	resp->p = p;

	xb->head[0].iov_len = (char *)resp->p - (char *)xb->head[0].iov_base;
	xb->pages = &rqstp->rq_respages[rqstp->rq_resused];
	xb->page_base = 0;
	xb->page_len = PAGE_SIZE; /* page of 1024 encoded indices */
	xb->tail[0].iov_base = resp->p;
	resp->end = xb->head[0].iov_base + PAGE_SIZE;
	xb->tail[0].iov_len = (char *)resp->end - (char *)resp->p;
	return 0;
}
/*
 * Return a stripeindices of length 1024 to test
 * the pNFS client multipage getdeviceinfo implementation.
 *
 * Encode a page of stripe indices.
 */
static void spnfs_set_test_indices(struct pnfs_filelayout_device *fldev,
				  struct spnfs_device *dev,
				  struct pnfs_devinfo_arg *info)
{
	struct svc_rqst *rqstp = info->xdr.resp->rqstp;
	__be32 *p;
	int i, j = 0;

	p = (__be32 *)page_address(rqstp->rq_respages[rqstp->rq_resused]);
	fldev->fl_stripeindices_length = 1024;
	/* round-robin the data servers device index into the stripe indicie */
	for (i = 0; i < 1024; i++) {
		*p++ = cpu_to_be32(j);
		if (j < dev->dscount - 1)
			j++;
		else
			j = 0;
	}
	fldev->fl_stripeindices_list = NULL;
}
#endif /* CONFIG_SPNFS_TEST */

int
spnfs_getdeviceinfo(struct super_block *sb, struct exp_xdr_stream *xdr,
		    u32 layout_type,
		    const struct nfsd4_pnfs_deviceid *devid)
{
	struct spnfs *spnfs = global_spnfs;
	struct spnfs_msg *im = NULL;
	union spnfs_msg_res *res = NULL;
	struct spnfs_device *dev;
	struct pnfs_filelayout_device *fldev = NULL;
	struct pnfs_filelayout_multipath *mp = NULL;
	struct pnfs_filelayout_devaddr *fldap = NULL;
	int status = 0, i, len;

	im = kmalloc(sizeof(struct spnfs_msg), GFP_KERNEL);
	if (im == NULL) {
		status = -ENOMEM;
		goto getdeviceinfo_out;
	}

	res = kmalloc(sizeof(union spnfs_msg_res), GFP_KERNEL);
	if (res == NULL) {
		status = -ENOMEM;
		goto getdeviceinfo_out;
	}

	im->im_type = SPNFS_TYPE_GETDEVICEINFO;
	/* XXX FIX: figure out what to do about fsid */
	im->im_args.getdeviceinfo_args.devid = devid->devid;

	/* call function to queue the msg for upcall */
	status = spnfs_upcall(spnfs, im, res);
	if (status != 0) {
		dprintk("%s spnfs upcall failure: %d\n", __func__, status);
		status = -EIO;
		goto getdeviceinfo_out;
	}
	status = res->getdeviceinfo_res.status;
	if (status != 0)
		goto getdeviceinfo_out;

	dev = &res->getdeviceinfo_res.devinfo;

	/* Fill in the device data, i.e., nfs4_1_file_layout_ds_addr4 */
	fldev = kzalloc(sizeof(struct pnfs_filelayout_device), GFP_KERNEL);
	if (fldev == NULL) {
		status = -ENOMEM;
		goto getdeviceinfo_out;
	}

	/*
	 * Stripe count is the same as data server count for our purposes
	 */
	fldev->fl_stripeindices_length = dev->dscount;
	fldev->fl_device_length = dev->dscount;

	/* Set stripe indices */
#ifdef CONFIG_SPNFS_TEST
	spnfs_set_test_indices(fldev, dev, info);
	fldev->fl_enc_stripe_indices = spnfs_test_indices_xdr;
#else /* CONFIG_SPNFS_TEST */
	fldev->fl_stripeindices_list =
		kmalloc(fldev->fl_stripeindices_length * sizeof(u32),
			GFP_KERNEL);
	if (fldev->fl_stripeindices_list == NULL) {
		status = -ENOMEM;
		goto getdeviceinfo_out;
	}
	for (i = 0; i < fldev->fl_stripeindices_length; i++)
		fldev->fl_stripeindices_list[i] = i;
#endif /* CONFIG_SPNFS_TEST */

	/*
	 * Set the device's data server addresses  No multipath for spnfs,
	 * so mp length is always 1.
	 *
	 */
	fldev->fl_device_list =
		kmalloc(fldev->fl_device_length *
			sizeof(struct pnfs_filelayout_multipath),
			GFP_KERNEL);
	if (fldev->fl_device_list == NULL) {
		status = -ENOMEM;
		goto getdeviceinfo_out;
	}
	for (i = 0; i < fldev->fl_device_length; i++) {
		mp = &fldev->fl_device_list[i];
		mp->fl_multipath_length = 1;
		mp->fl_multipath_list =
			kmalloc(sizeof(struct pnfs_filelayout_devaddr),
				GFP_KERNEL);
		if (mp->fl_multipath_list == NULL) {
			status = -ENOMEM;
			goto getdeviceinfo_out;
		}
		fldap = mp->fl_multipath_list;

		/*
		 * Copy the netid into the device address, for example: "tcp"
		 */
		len = strlen(dev->dslist[i].netid);
		fldap->r_netid.data = kmalloc(len, GFP_KERNEL);
		if (fldap->r_netid.data == NULL) {
			status = -ENOMEM;
			goto getdeviceinfo_out;
		}
		memcpy(fldap->r_netid.data, dev->dslist[i].netid, len);
		fldap->r_netid.len = len;

		/*
		 * Copy the network address into the device address,
		 * for example: "10.35.9.16.08.01"
		 */
		len = strlen(dev->dslist[i].addr);
		fldap->r_addr.data = kmalloc(len, GFP_KERNEL);
		if (fldap->r_addr.data == NULL) {
			status = -ENOMEM;
			goto getdeviceinfo_out;
		}
		memcpy(fldap->r_addr.data, dev->dslist[i].addr, len);
		fldap->r_addr.len = len;
	}

	/* encode the device data */
	status = filelayout_encode_devinfo(xdr, fldev);

getdeviceinfo_out:
	if (fldev) {
		kfree(fldev->fl_stripeindices_list);
		if (fldev->fl_device_list) {
			for (i = 0; i < fldev->fl_device_length; i++) {
				fldap =
				    fldev->fl_device_list[i].fl_multipath_list;
				kfree(fldap->r_netid.data);
				kfree(fldap->r_addr.data);
				kfree(fldap);
			}
			kfree(fldev->fl_device_list);
		}
		kfree(fldev);
	}

	kfree(im);
	kfree(res);

	return status;
}

int
spnfs_setattr(void)
{
	return 0;
}

int
spnfs_open(struct inode *inode, struct nfsd4_open *open)
{
	struct spnfs *spnfs = global_spnfs; /* keep up the pretence */
	struct spnfs_msg *im = NULL;
	union spnfs_msg_res *res = NULL;
	int status = 0;

	im = kmalloc(sizeof(struct spnfs_msg), GFP_KERNEL);
	if (im == NULL) {
		status = -ENOMEM;
		goto open_out;
	}

	res = kmalloc(sizeof(union spnfs_msg_res), GFP_KERNEL);
	if (res == NULL) {
		status = -ENOMEM;
		goto open_out;
	}

	im->im_type = SPNFS_TYPE_OPEN;
	im->im_args.open_args.inode = inode->i_ino;
	im->im_args.open_args.generation = inode->i_generation;
	im->im_args.open_args.create = open->op_create;
	im->im_args.open_args.createmode = open->op_createmode;
	im->im_args.open_args.truncate = open->op_truncate;

	/* call function to queue the msg for upcall */
	status = spnfs_upcall(spnfs, im, res);
	if (status != 0) {
		dprintk("%s spnfs upcall failure: %d\n", __func__, status);
		status = -EIO;
		goto open_out;
	}
	status = res->open_res.status;

open_out:
	kfree(im);
	kfree(res);

	return status;
}

int
spnfs_create(void)
{
	return 0;
}

/*
 * Invokes the spnfsd with the inode number of the object to remove.
 * The file has already been removed on the MDS, so all the spnsfd
 * daemon does is remove the stripes.
 * Returns 0 on success otherwise error code
 */
int
spnfs_remove(unsigned long ino, unsigned long generation)
{
	struct spnfs *spnfs = global_spnfs; /* keep up the pretence */
	struct spnfs_msg *im = NULL;
	union spnfs_msg_res *res = NULL;
	int status = 0;

	im = kmalloc(sizeof(struct spnfs_msg), GFP_KERNEL);
	if (im == NULL) {
		status = -ENOMEM;
		goto remove_out;
	}

	res = kmalloc(sizeof(union spnfs_msg_res), GFP_KERNEL);
	if (res == NULL) {
		status = -ENOMEM;
		goto remove_out;
	}

	im->im_type = SPNFS_TYPE_REMOVE;
	im->im_args.remove_args.inode = ino;
	im->im_args.remove_args.generation = generation;

	/* call function to queue the msg for upcall */
	status = spnfs_upcall(spnfs, im, res);
	if (status != 0) {
		dprintk("%s spnfs upcall failure: %d\n", __func__, status);
		status = -EIO;
		goto remove_out;
	}
	status = res->remove_res.status;

remove_out:
	kfree(im);
	kfree(res);

	return status;
}

static int
read_one(struct inode *inode, loff_t offset, size_t len, char *buf,
	 struct file **filp)
{
	loff_t bufoffset = 0, soffset, pos, snum, soff, tmp;
	size_t iolen;
	int completed = 0, ds, err;

	while (len > 0) {
		tmp = offset;
		soff = do_div(tmp, spnfs_config->stripe_size);
		snum = tmp;
		ds = do_div(tmp, spnfs_config->num_ds);
		if (spnfs_config->dense_striping == 0)
			soffset = offset;
		else {
			tmp = snum;
			do_div(tmp, spnfs_config->num_ds);
			soffset = tmp * spnfs_config->stripe_size + soff;
		}
		if (len < spnfs_config->stripe_size - soff)
			iolen = len;
		else
			iolen = spnfs_config->stripe_size - soff;

		pos = soffset;
		err = vfs_read(filp[ds], buf + bufoffset, iolen, &pos);
		if (err < 0)
			return -EIO;
		if (err == 0)
			break;
		filp[ds]->f_pos = pos;
		iolen = err;
		completed += iolen;
		len -= iolen;
		offset += iolen;
		bufoffset += iolen;
	}

	return completed;
}

static __be32
read(struct inode *inode, loff_t offset, unsigned long *lenp, int vlen,
     struct svc_rqst *rqstp)
{
	int i, vnum, err, bytecount = 0;
	char path[128];
	struct file *filp[SPNFS_MAX_DATA_SERVERS];
	size_t iolen;
	__be32 status = nfs_ok;

	/*
	 * XXX We should just be doing this at open time, but it gets
	 * kind of messy storing this info in nfsd's state structures
	 * and piggybacking its path through the various state handling
	 * functions.  Revisit this.
	 */
	memset(filp, 0, SPNFS_MAX_DATA_SERVERS * sizeof(struct file *));
	for (i = 0; i < spnfs_config->num_ds; i++) {
		sprintf(path, "%s/%ld.%u", spnfs_config->ds_dir[i],
			inode->i_ino, inode->i_generation);
		filp[i] = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
		if (filp[i] == NULL) {
			status = nfserr_io;
			goto read_out;
		}
		get_file(filp[i]);
	}

	for (vnum = 0 ; vnum < vlen ; vnum++) {
		iolen = rqstp->rq_vec[vnum].iov_len;
		err = read_one(inode, offset + bytecount, iolen,
			       (char *)rqstp->rq_vec[vnum].iov_base, filp);
		if (err < 0) {
			status = nfserr_io;
			goto read_out;
		}
		if (err < iolen) {
			bytecount += err;
			goto read_out;
		}
		bytecount += rqstp->rq_vec[vnum].iov_len;
	}

read_out:
	*lenp = bytecount;
	for (i = 0; i < spnfs_config->num_ds; i++) {
		if (filp[i]) {
			filp_close(filp[i], current->files);
			fput(filp[i]);
		}
	}
	return status;
}

__be32
spnfs_read(struct inode *inode, loff_t offset, unsigned long *lenp, int vlen,
	   struct svc_rqst *rqstp)
{
	if (spnfs_config)
		return read(inode, offset, lenp, vlen, rqstp);
	else {
		printk(KERN_ERR "Please upgrade to latest spnfsd\n");
		return nfserr_notsupp;
	}
}

static int
write_one(struct inode *inode, loff_t offset, size_t len, char *buf,
	  struct file **filp)
{
	loff_t bufoffset = 0, soffset, pos, snum, soff, tmp;
	size_t iolen;
	int completed = 0, ds, err;

	while (len > 0) {
		tmp = offset;
		soff = do_div(tmp, spnfs_config->stripe_size);
		snum = tmp;
		ds = do_div(tmp, spnfs_config->num_ds);
		if (spnfs_config->dense_striping == 0)
			soffset = offset;
		else {
			tmp = snum;
			do_div(tmp, spnfs_config->num_ds);
			soffset = tmp * spnfs_config->stripe_size + soff;
		}
		if (len < spnfs_config->stripe_size - soff)
			iolen = len;
		else
			iolen = spnfs_config->stripe_size - soff;

		pos = soffset;
		err = vfs_write(filp[ds], buf + bufoffset, iolen, &pos);
		if (err < 0)
			return -EIO;
		filp[ds]->f_pos = pos;
		iolen = err;
		completed += iolen;
		len -= iolen;
		offset += iolen;
		bufoffset += iolen;
	}

	return completed;
}

static __be32
write(struct inode *inode, loff_t offset, size_t len, int vlen,
      struct svc_rqst *rqstp)
{
	int i, vnum, err, bytecount = 0;
	char path[128];
	struct file *filp[SPNFS_MAX_DATA_SERVERS];
	size_t iolen;
	__be32 status = nfs_ok;

	/*
	 * XXX We should just be doing this at open time, but it gets
	 * kind of messy storing this info in nfsd's state structures
	 * and piggybacking its path through the various state handling
	 * functions.  Revisit this.
	 */
	memset(filp, 0, SPNFS_MAX_DATA_SERVERS * sizeof(struct file *));
	for (i = 0; i < spnfs_config->num_ds; i++) {
		sprintf(path, "%s/%ld.%u", spnfs_config->ds_dir[i],
			inode->i_ino, inode->i_generation);
		filp[i] = filp_open(path, O_RDWR | O_LARGEFILE, 0);
		if (filp[i] == NULL) {
			status = nfserr_io;
			goto write_out;
		}
		get_file(filp[i]);
	}

	for (vnum = 0; vnum < vlen; vnum++) {
		iolen = rqstp->rq_vec[vnum].iov_len;
		err = write_one(inode, offset + bytecount, iolen,
				(char *)rqstp->rq_vec[vnum].iov_base, filp);
		if (err != iolen) {
			dprintk("spnfs_write: err=%d expected %Zd\n", err, len);
			status = nfserr_io;
			goto write_out;
		}
		bytecount += rqstp->rq_vec[vnum].iov_len;
	}

write_out:
	for (i = 0; i < spnfs_config->num_ds; i++) {
		if (filp[i]) {
			filp_close(filp[i], current->files);
			fput(filp[i]);
		}
	}

	return status;
}

__be32
spnfs_write(struct inode *inode, loff_t offset, size_t len, int vlen,
	    struct svc_rqst *rqstp)
{
	if (spnfs_config)
		return write(inode, offset, len, vlen, rqstp);
	else {
		printk(KERN_ERR "Please upgrade to latest spnfsd\n");
		return nfserr_notsupp;
	}
}

int
spnfs_commit(void)
{
	return 0;
}

/*
 * Return the state for this object.
 * At this time simply return 0 to indicate success and use the existing state
 */
int
spnfs_get_state(struct inode *inode, struct knfsd_fh *fh, struct pnfs_get_state *arg)
{
	return 0;
}

/*
 * Return the filehandle for the specified file descriptor
 */
int
spnfs_getfh(int fd, struct nfs_fh *fh)
{
	struct file *file;

	file = fget(fd);
	if (file == NULL)
		return -EIO;

	memcpy(fh, NFS_FH(file->f_dentry->d_inode), sizeof(struct nfs_fh));
	fput(file);
	return 0;
}
