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

#if defined(CONFIG_SPNFS)
/* DMXXX: includes taken from spnfs_com.c.  Don't need so many: revisit */
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/sched.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/namei.h>

#include <linux/sunrpc/clnt.h>
#include <linux/workqueue.h>
#include <linux/sunrpc/rpc_pipe_fs.h>

#include <linux/nfs_fs.h>
#include <linux/nfs4.h>
#include <linux/nfsd4_spnfs.h>
#include <linux/nfsd/state.h>
#include <linux/nfsd/nfsd4_pnfs.h>
#include <linux/nfsd/nfs4layoutxdr.h>

#define	NFSDDBG_FACILITY		NFSDDBG_PROC

/*
 * The functions that are called from elsewhere in the kernel
 * to perform tasks in userspace
 *
 */

extern struct spnfs *global_spnfs;

#endif /* CONFIG_SPNFS */
