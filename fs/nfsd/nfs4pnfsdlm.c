/******************************************************************************
 *
 * (c) 2007 Network Appliance, Inc.  All Rights Reserved.
 * (c) 2009 NetApp.  All Rights Reserved.
 *
 * NetApp provides this source code under the GPL v2 License.
 * The GPL v2 license is available at
 * http://opensource.org/licenses/gpl-license.php.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ******************************************************************************/

#include <linux/nfs4.h>
#include <linux/nfsd/debug.h>
#include <linux/nfsd/nfs4pnfsdlm.h>
#include <linux/nfsd/nfs4layoutxdr.h>

#define NFSDDBG_FACILITY                NFSDDBG_PROC

/* Just use a linked list. Do not expect more than 32 dlm_device_entries
 * the first implementation will just use one device per cluster file system
 */

static LIST_HEAD(dlm_device_list);
static DEFINE_SPINLOCK(dlm_device_list_lock);

struct dlm_device_entry {
	struct list_head	dlm_dev_list;
	char			disk_name[DISK_NAME_LEN];
	int			num_ds;
	char			ds_list[NFSD_DLM_DS_LIST_MAX];
};

static struct dlm_device_entry *
nfsd4_find_pnfs_dlm_device(char *disk_name)
{
	struct dlm_device_entry *dlm_pdev;

	spin_lock(&dlm_device_list_lock);
	list_for_each_entry(dlm_pdev, &dlm_device_list, dlm_dev_list) {
		if (!memcmp(dlm_pdev->disk_name, disk_name, strlen(disk_name))) {
			spin_unlock(&dlm_device_list_lock);
			return dlm_pdev;
		}
	}
	spin_unlock(&dlm_device_list_lock);
	return NULL;
}

/*
 * pnfs_dlm_device string format:
 *     block-device-path:<ds1 ipv4 address>,<ds2 ipv4 address>
 *
 * Examples
 *     /dev/sda:192.168.1.96,192.168.1.97' creates a data server list with
 *     two data servers for the dlm cluster file system mounted on /dev/sda.
 *
 *     /dev/sda:192.168.1.96,192.168.1.100'
 *     replaces the data server list for /dev/sda
 *
 *     Only the deviceid == 1 is supported. Can add device id to
 *     pnfs_dlm_device string when needed.
 *
 *     Only the round robin each data server once stripe index is supported.
 */
int
nfsd4_set_pnfs_dlm_device(char *pnfs_dlm_device, int len)

{
	struct dlm_device_entry *new, *found;
	char *bufp = pnfs_dlm_device;
	char *endp = bufp + strlen(bufp);
	int err = -ENOMEM;

	dprintk("--> %s len %d\n", __func__, len);

	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return err;

	err = -EINVAL;
	/* disk_name */
	/* FIXME: need to check for valid disk_name. search superblocks?
	 * check for slash dev slash ?
	 */
	len = strcspn(bufp, ":");
	if (len > DISK_NAME_LEN)
		goto out_free;
	memcpy(new->disk_name, bufp, len);

	err = -EINVAL;
	bufp += len + 1;
	if (bufp >= endp)
		goto out_free;

	/* data server list */
	/* FIXME: need to check for comma separated valid ip format */
	len = strcspn(bufp, ":");
	if (len > NFSD_DLM_DS_LIST_MAX)
		goto out_free;
	memcpy(new->ds_list, bufp, len);

	/* count the number of comma-delimited DS IPs */
	new->num_ds = 1;
	while ((bufp = strchr(bufp, ',')) != NULL) {
		new->num_ds++;
		bufp++;
	}

	dprintk("%s disk_name %s num_ds %d ds_list %s\n", __func__,
		new->disk_name, new->num_ds, new->ds_list);

	found = nfsd4_find_pnfs_dlm_device(new->disk_name);
	if (found) {
		/* FIXME: should compare found->ds_list with new->ds_list
		 * and if it is different, kick off a CB_NOTIFY change
		 * deviceid.
		 */
		dprintk("%s pnfs_dlm_device %s:%s already in cache "
			" replace ds_list with new ds_list %s\n", __func__,
			found->disk_name, found->ds_list, new->ds_list);
		memset(found->ds_list, 0, DISK_NAME_LEN);
		memcpy(found->ds_list, new->ds_list, strlen(new->ds_list));
		kfree(new);
	} else {
		dprintk("%s Adding pnfs_dlm_device %s:%s\n", __func__,
				new->disk_name, new->ds_list);
		spin_lock(&dlm_device_list_lock);
		list_add(&new->dlm_dev_list, &dlm_device_list);
		spin_unlock(&dlm_device_list_lock);
	}
	dprintk("<-- %s Success\n", __func__);
	return 0;

out_free:
	kfree(new);
	dprintk("<-- %s returns %d\n", __func__, err);
	return err;
}

void nfsd4_pnfs_dlm_shutdown(void)
{
	struct dlm_device_entry *dlm_pdev, *next;

	dprintk("--> %s\n", __func__);

	spin_lock(&dlm_device_list_lock);
	list_for_each_entry_safe (dlm_pdev, next, &dlm_device_list,
				  dlm_dev_list) {
		list_del(&dlm_pdev->dlm_dev_list);
		kfree(dlm_pdev);
	}
	spin_unlock(&dlm_device_list_lock);
}

static int nfsd4_pnfs_dlm_getdeviter(struct super_block *sb,
				     u32 layout_type,
				     struct nfsd4_pnfs_dev_iter_res *res)
{
	if (layout_type != LAYOUT_NFSV4_1_FILES) {
		printk(KERN_ERR "%s: ERROR: layout type isn't 'file' "
			"(type: %x)\n", __func__, layout_type);
		return -ENOTSUPP;
	}

	res->gd_eof = 1;
	if (res->gd_cookie)
		return -ENOENT;

	res->gd_cookie = 1;
	res->gd_verf = 1;
	res->gd_devid = 1;
	return 0;
}

static int nfsd4_pnfs_dlm_getdevinfo(struct super_block *sb,
				     struct exp_xdr_stream *xdr,
				     u32 layout_type,
				     const struct nfsd4_pnfs_deviceid *devid)
{
	int err, len, i = 0;
	struct pnfs_filelayout_device fdev;
	struct pnfs_filelayout_devaddr *daddr;
	struct dlm_device_entry *dlm_pdev;
	char   *bufp;

	err = -ENOTSUPP;
	if (layout_type != LAYOUT_NFSV4_1_FILES) {
		dprintk("%s: ERROR: layout type isn't 'file' "
			"(type: %x)\n", __func__, layout_type);
		return err;
	}

	/* We only hand out a deviceid of 1 in LAYOUTGET, so a GETDEVICEINFO
	 * with a gdia_device_id != 1 is invalid.
	 */
	err = -EINVAL;
	if (devid->devid != 1) {
		dprintk("%s: WARNING: didn't receive a deviceid of "
			"1 (got: 0x%llx)\n", __func__, devid->devid);
		return err;
	}

	/*
	 * If the DS list has not been established, return -EINVAL
	 */
	dlm_pdev = nfsd4_find_pnfs_dlm_device(sb->s_bdev->bd_disk->disk_name);
	if (!dlm_pdev) {
		dprintk("%s: DEBUG: disk %s Not Found\n", __func__,
			sb->s_bdev->bd_disk->disk_name);
		return err;
	}

	dprintk("%s: Found disk %s with DS list |%s|\n",
		__func__, dlm_pdev->disk_name, dlm_pdev->ds_list);

	memset(&fdev, '\0', sizeof(fdev));
	fdev.fl_device_length = dlm_pdev->num_ds;

	err = -ENOMEM;
	len = sizeof(*fdev.fl_device_list) * fdev.fl_device_length;
	fdev.fl_device_list = kzalloc(len, GFP_KERNEL);
	if (!fdev.fl_device_list) {
		printk(KERN_ERR "%s: ERROR: unable to kmalloc a device list "
			"buffer for %d DSes.\n", __func__, i);
		fdev.fl_device_length = 0;
		goto out;
	}

	/* Set a simple stripe indicie */
	fdev.fl_stripeindices_length = fdev.fl_device_length;
	fdev.fl_stripeindices_list = kzalloc(sizeof(u32) *
				     fdev.fl_stripeindices_length, GFP_KERNEL);

	if (!fdev.fl_stripeindices_list) {
		printk(KERN_ERR "%s: ERROR: unable to kmalloc a stripeindices "
			"list buffer for %d DSes.\n", __func__, i);
		goto out;
	}
	for (i = 0; i < fdev.fl_stripeindices_length; i++)
		fdev.fl_stripeindices_list[i] = i;

	/* Transfer the data server list with a single multipath entry */
	bufp = dlm_pdev->ds_list;
	for (i = 0; i < fdev.fl_device_length; i++) {
		daddr = kmalloc(sizeof(*daddr), GFP_KERNEL);
		if (!daddr) {
			printk(KERN_ERR "%s: ERROR: unable to kmalloc a device "
				"addr buffer.\n", __func__);
			goto out;
		}

		daddr->r_netid.data = "tcp";
		daddr->r_netid.len = 3;

		len = strcspn(bufp, ",");
		daddr->r_addr.data = kmalloc(len + 4, GFP_KERNEL);
		memcpy(daddr->r_addr.data, bufp, len);
		/*
		 * append the port number.  interpreted as two more bytes
		 * beyond the quad: ".8.1" -> 0x08.0x01 -> 0x0801 = port 2049.
		 */
		memcpy(daddr->r_addr.data + len, ".8.1", 4);
		daddr->r_addr.len = len + 4;

		fdev.fl_device_list[i].fl_multipath_length = 1;
		fdev.fl_device_list[i].fl_multipath_list = daddr;

		dprintk("%s: encoding DS |%s|\n", __func__, bufp);

		bufp += len + 1;
	}

	/* have nfsd encode the device info */
	err = filelayout_encode_devinfo(xdr, &fdev);
out:
	for (i = 0; i < fdev.fl_device_length; i++)
		kfree(fdev.fl_device_list[i].fl_multipath_list);
	kfree(fdev.fl_device_list);
	kfree(fdev.fl_stripeindices_list);
	dprintk("<-- %s returns %d\n", __func__, err);
	return err;
}
