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
 *****************************************************************************/

#include "pnfsd.h"

#define NFSDDBG_FACILITY                NFSDDBG_PNFS

static DEFINE_SPINLOCK(layout_lock);

/* hash table for nfsd4_pnfs_deviceid.sbid */
#define SBID_HASH_BITS	8
#define SBID_HASH_SIZE	(1 << SBID_HASH_BITS)
#define SBID_HASH_MASK	(SBID_HASH_SIZE - 1)

struct sbid_tracker {
	u64 id;
	struct super_block *sb;
	struct list_head hash;
};

static u64 current_sbid;
static struct list_head sbid_hashtbl[SBID_HASH_SIZE];

static unsigned long
sbid_hashval(struct super_block *sb)
{
	return hash_ptr(sb, SBID_HASH_BITS);
}

static struct sbid_tracker *
alloc_sbid(void)
{
	return kmalloc(sizeof(struct sbid_tracker), GFP_KERNEL);
}

static void
destroy_sbid(struct sbid_tracker *sbid)
{
	spin_lock(&layout_lock);
	list_del(&sbid->hash);
	spin_unlock(&layout_lock);
	kfree(sbid);
}

void
nfsd4_free_pnfs_slabs(void)
{
	int i;
	struct sbid_tracker *sbid;

	for (i = 0; i < SBID_HASH_SIZE; i++) {
		while (!list_empty(&sbid_hashtbl[i])) {
			sbid = list_first_entry(&sbid_hashtbl[i],
						struct sbid_tracker,
						hash);
			destroy_sbid(sbid);
		}
	}
}

int
nfsd4_init_pnfs_slabs(void)
{
	int i;

	for (i = 0; i < SBID_HASH_SIZE; i++)
		INIT_LIST_HEAD(&sbid_hashtbl[i]);

	return 0;
}

static u64
alloc_init_sbid(struct super_block *sb)
{
	struct sbid_tracker *sbid;
	struct sbid_tracker *new = alloc_sbid();
	unsigned long hash_idx = sbid_hashval(sb);
	u64 id = 0;

	if (likely(new)) {
		spin_lock(&layout_lock);
		id = ++current_sbid;
		new->id = (id << SBID_HASH_BITS) | (hash_idx & SBID_HASH_MASK);
		id = new->id;
		BUG_ON(id == 0);
		new->sb = sb;

		list_for_each_entry (sbid, &sbid_hashtbl[hash_idx], hash)
			if (sbid->sb == sb) {
				kfree(new);
				id = sbid->id;
				spin_unlock(&layout_lock);
				return id;
			}
		list_add(&new->hash, &sbid_hashtbl[hash_idx]);
		spin_unlock(&layout_lock);
	}
	return id;
}

static u64
find_create_sbid(struct super_block *sb)
{
	struct sbid_tracker *sbid;
	unsigned long hash_idx = sbid_hashval(sb);
	int pos = 0;
	u64 id = 0;

	spin_lock(&layout_lock);
	list_for_each_entry (sbid, &sbid_hashtbl[hash_idx], hash) {
		pos++;
		if (sbid->sb != sb)
			continue;
		if (pos > 1) {
			list_del(&sbid->hash);
			list_add(&sbid->hash, &sbid_hashtbl[hash_idx]);
		}
		id = sbid->id;
		break;
	}
	spin_unlock(&layout_lock);

	if (!id)
		id = alloc_init_sbid(sb);

	return id;
}
