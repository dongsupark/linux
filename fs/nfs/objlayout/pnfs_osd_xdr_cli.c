/*
 *  pnfs_osd_xdr.c
 *
 *  Object-Based pNFS Layout XDR layer
 *
 *  Copyright (C) 2007-2009 Panasas Inc.
 *  All rights reserved.
 *
 *  Benny Halevy <bhalevy@panasas.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  See the file COPYING included with this distribution for more details.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the Panasas company nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 *  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/pnfs_osd_xdr.h>

#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

/*
 * The following implementation is based on these Internet Drafts:
 *
 * draft-ietf-nfsv4-minorversion-21
 * draft-ietf-nfsv4-pnfs-obj-12
 */

/*
 * struct pnfs_osd_objid {
 * 	struct pnfs_deviceid	oid_device_id;
 * 	u64			oid_partition_id;
 * 	u64			oid_object_id;
 * };
 */
static inline u32 *
pnfs_osd_xdr_decode_objid(u32 *p, struct pnfs_osd_objid *objid)
{
	COPYMEM(objid->oid_device_id.data, sizeof(objid->oid_device_id.data));
	READ64(objid->oid_partition_id);
	READ64(objid->oid_object_id);
	return p;
}

static inline u32 *
pnfs_osd_xdr_decode_opaque_cred(u32 *p,
				struct pnfs_osd_opaque_cred *opaque_cred)
{
	READ32(opaque_cred->cred_len);
	COPYMEM(opaque_cred->cred, opaque_cred->cred_len);
	return p;
}

/*
 * struct pnfs_osd_object_cred {
 * 	struct pnfs_osd_objid		oc_object_id;
 * 	u32				oc_osd_version;
 * 	u32				oc_cap_key_sec;
 * 	struct pnfs_osd_opaque_cred	oc_cap_key
 * 	struct pnfs_osd_opaque_cred	oc_cap;
 * };
 */
static inline u32 *
pnfs_osd_xdr_decode_object_cred(u32 *p, struct pnfs_osd_object_cred *comp,
				u8 **credp)
{
	u8 *cred;

	p = pnfs_osd_xdr_decode_objid(p, &comp->oc_object_id);
	READ32(comp->oc_osd_version);
	READ32(comp->oc_cap_key_sec);

	cred = *credp;
	comp->oc_cap_key.cred = cred;
	p = pnfs_osd_xdr_decode_opaque_cred(p, &comp->oc_cap_key);
	cred = (u8 *)((u32 *)cred + XDR_QUADLEN(comp->oc_cap_key.cred_len));
	comp->oc_cap.cred = cred;
	p = pnfs_osd_xdr_decode_opaque_cred(p, &comp->oc_cap);
	cred = (u8 *)((u32 *)cred + XDR_QUADLEN(comp->oc_cap.cred_len));
	*credp = cred;

	return p;
}

/*
 * struct pnfs_osd_data_map {
 * 	u32	odm_num_comps;
 * 	u64	odm_stripe_unit;
 * 	u32	odm_group_width;
 * 	u32	odm_group_depth;
 * 	u32	odm_mirror_cnt;
 * 	u32	odm_raid_algorithm;
 * };
 */
static inline u32 *
pnfs_osd_xdr_decode_data_map(u32 *p, struct pnfs_osd_data_map *data_map)
{
	READ32(data_map->odm_num_comps);
	READ64(data_map->odm_stripe_unit);
	READ32(data_map->odm_group_width);
	READ32(data_map->odm_group_depth);
	READ32(data_map->odm_mirror_cnt);
	READ32(data_map->odm_raid_algorithm);
	dprintk("%s: odm_num_comps=%u odm_stripe_unit=%llu odm_group_width=%u "
		"odm_group_depth=%u odm_mirror_cnt=%u odm_raid_algorithm=%u\n",
		__func__,
		data_map->odm_num_comps,
		(unsigned long long)data_map->odm_stripe_unit,
		data_map->odm_group_width,
		data_map->odm_group_depth,
		data_map->odm_mirror_cnt,
		data_map->odm_raid_algorithm);
	return p;
}

struct pnfs_osd_layout *
pnfs_osd_xdr_decode_layout(struct pnfs_osd_layout *layout, u32 *p)
{
	int i;
	u32 *start = p;
	struct pnfs_osd_object_cred *comp;
	u8 *cred;

	p = pnfs_osd_xdr_decode_data_map(p, &layout->olo_map);
	READ32(layout->olo_comps_index);
	READ32(layout->olo_num_comps);
	layout->olo_comps = (struct pnfs_osd_object_cred *)(layout + 1);
	comp = layout->olo_comps;
	cred = (u8 *)(comp + layout->olo_num_comps);
	dprintk("%s: comps_index=%u num_comps=%u\n",
		__func__, layout->olo_comps_index, layout->olo_num_comps);
	for (i = 0; i < layout->olo_num_comps; i++) {
		p = pnfs_osd_xdr_decode_object_cred(p, comp, &cred);
		dprintk("%s: comp[%d]=dev(%llx:%llx) par=0x%llx obj=0x%llx "
			"key_len=%u cap_len=%u\n",
			__func__, i,
			_DEVID_LO(&comp->oc_object_id.oid_device_id),
			_DEVID_HI(&comp->oc_object_id.oid_device_id),
			comp->oc_object_id.oid_partition_id,
			comp->oc_object_id.oid_object_id,
			comp->oc_cap_key.cred_len, comp->oc_cap.cred_len);
		comp++;
	}
	dprintk("%s: xdr_size=%Zd end=%p in_core_size=%Zd\n", __func__,
	       (char *)p - (char *)start, cred, (char *)cred - (char *)layout);
	return layout;
}

/*
 * Get Device Information Decoding
 *
 * Note: since Device Information is currently done synchronously, most
 *       of the actual fields are left inside the rpc buffer and are only
 *       pointed to by the pnfs_osd_deviceaddr members. So the read buffer
 *       should not be freed while the returned information is in use.
 */

u32 *__xdr_read_calc_nfs4_string(
	u32 *p, struct nfs4_string *str, u8 **freespace)
{
	u32 len;
	char *data;
	bool need_copy;

	READ32(len);
	data = (char *)p;

	if (data[len]) { /* Not null terminated we'll need extra space */
		data = *freespace;
		*freespace += len + 1;
		need_copy = true;
	} else {
		need_copy = false;
	}

	if (str) {
		str->len = len;
		str->data = data;
		if (need_copy) {
			memcpy(data, p, len);
			data[len] = 0;
		}
	}

	p += XDR_QUADLEN(len);
	return p;
}

u32 *__xdr_read_calc_u8_opaque(
	u32 *p, struct nfs4_string *str)
{
	u32 len;

	READ32(len);

	if (str) {
		str->len = len;
		str->data = (char *)p;
	}

	p += XDR_QUADLEN(len);
	return p;
}

/*
 * struct pnfs_osd_targetid {
 * 	u32			oti_type;
 * 	struct nfs4_string	oti_scsi_device_id;
 * };
 */
u32 *__xdr_read_calc_targetid(
	u32 *p, struct pnfs_osd_targetid* targetid, u8 **freespace)
{
	u32 oti_type;

	READ32(oti_type);
	if (targetid)
		targetid->oti_type = oti_type;

	switch (oti_type) {
	case OBJ_TARGET_SCSI_NAME:
	case OBJ_TARGET_SCSI_DEVICE_ID:
		p = __xdr_read_calc_u8_opaque(p,
			targetid ? &targetid->oti_scsi_device_id : NULL);
	}

	return p;
}

/*
 * struct pnfs_osd_net_addr {
 * 	struct nfs4_string	r_netid;
 * 	struct nfs4_string	r_addr;
 * };
 */
u32 *__xdr_read_calc_net_addr(
	u32 *p, struct pnfs_osd_net_addr* netaddr, u8 **freespace)
{

	p = __xdr_read_calc_nfs4_string(p,
			netaddr ? &netaddr->r_netid : NULL,
			freespace);

	p = __xdr_read_calc_nfs4_string(p,
			netaddr ? &netaddr->r_addr : NULL,
			freespace);

	return p;
}

/*
 * struct pnfs_osd_targetaddr {
 * 	u32				ota_available;
 * 	struct pnfs_osd_net_addr	ota_netaddr;
 * };
 */
u32 *__xdr_read_calc_targetaddr(
	u32 *p, struct pnfs_osd_targetaddr *targetaddr, u8 **freespace)
{
	u32 ota_available;

	READ32(ota_available);
	if (targetaddr)
		targetaddr->ota_available = ota_available;

	if (ota_available) {
		p = __xdr_read_calc_net_addr(p,
				targetaddr ? &targetaddr->ota_netaddr : NULL,
				freespace);
	}

	return p;
}

/*
 * struct pnfs_osd_deviceaddr {
 * 	struct pnfs_osd_targetid	oda_targetid;
 * 	struct pnfs_osd_targetaddr	oda_targetaddr;
 * 	u8				oda_lun[8];
 * 	struct nfs4_string		oda_systemid;
 * 	struct pnfs_osd_object_cred	oda_root_obj_cred;
 * 	struct nfs4_string		oda_osdname;
 * };
 */
u32 *__xdr_read_calc_deviceaddr(
	u32 *p, struct pnfs_osd_deviceaddr *deviceaddr, u8 **freespace)
{
	p = __xdr_read_calc_targetid(p,
			deviceaddr ? &deviceaddr->oda_targetid : NULL,
			freespace);

	p = __xdr_read_calc_targetaddr(p,
			deviceaddr ? &deviceaddr->oda_targetaddr : NULL,
			freespace);

	if (deviceaddr)
		COPYMEM(deviceaddr->oda_lun, sizeof(deviceaddr->oda_lun));
	else
		p += XDR_QUADLEN(sizeof(deviceaddr->oda_lun));

	p = __xdr_read_calc_u8_opaque(p,
			deviceaddr ? &deviceaddr->oda_systemid : NULL);

	if (deviceaddr) {
		p = pnfs_osd_xdr_decode_object_cred(p,
				&deviceaddr->oda_root_obj_cred, freespace);
	} else {
		*freespace += pnfs_osd_object_cred_incore_sz(p);
		p += pnfs_osd_object_cred_xdr_sz(p);
	}

	p = __xdr_read_calc_u8_opaque(p,
			deviceaddr ? &deviceaddr->oda_osdname : NULL);

	return p;
}

size_t pnfs_osd_xdr_deviceaddr_incore_sz(u32 *p)
{
	u8 *null_freespace = NULL;
	size_t sz;

	__xdr_read_calc_deviceaddr(p, NULL, &null_freespace);
	sz = sizeof(struct pnfs_osd_deviceaddr) + (size_t)null_freespace;

	return sz;
}

void pnfs_osd_xdr_decode_deviceaddr(
	struct pnfs_osd_deviceaddr *deviceaddr, u32 *p)
{
	u8 *freespace = (u8 *)(deviceaddr + 1);

	__xdr_read_calc_deviceaddr(p, deviceaddr, &freespace);
}

/*
 * struct pnfs_osd_layoutupdate {
 * 	u32	dsu_valid;
 * 	s64	dsu_delta;
 * 	u32	olu_ioerr_flag;
 * };
 */
int
pnfs_osd_xdr_encode_layoutupdate(struct xdr_stream *xdr,
				 struct pnfs_osd_layoutupdate *lou)
{
	__be32 *p = xdr_reserve_space(xdr, 16);

	if (!p)
		return -E2BIG;

	*p++ = cpu_to_be32(lou->dsu_valid);
	if (lou->dsu_valid)
		p = xdr_encode_hyper(p, lou->dsu_delta);
	*p++ = cpu_to_be32(lou->olu_ioerr_flag);
	return 0;
}

/*
 * struct pnfs_osd_objid {
 * 	struct pnfs_deviceid	oid_device_id;
 * 	u64			oid_partition_id;
 * 	u64			oid_object_id;
 */
static inline int pnfs_osd_xdr_encode_objid(struct xdr_stream *xdr,
					    struct pnfs_osd_objid *object_id)
{
	__be32 *p;

	p = xdr_reserve_space(xdr, 32);
	if (!p)
		return -E2BIG;

	p = xdr_encode_opaque_fixed(p, &object_id->oid_device_id.data,
				    sizeof(object_id->oid_device_id.data));
	p = xdr_encode_hyper(p, object_id->oid_partition_id);
	p = xdr_encode_hyper(p, object_id->oid_object_id);

	return 0;
}

/*
 * struct pnfs_osd_ioerr {
 * 	struct pnfs_osd_objid	oer_component;
 * 	u64			oer_comp_offset;
 * 	u64			oer_comp_length;
 * 	u32			oer_iswrite;
 * 	u32			oer_errno;
 * };
 */
int pnfs_osd_xdr_encode_ioerr(struct xdr_stream *xdr,
			      struct pnfs_osd_ioerr *ioerr)
{
	__be32 *p;
	int ret;

	ret = pnfs_osd_xdr_encode_objid(xdr, &ioerr->oer_component);
	if (ret)
		return ret;

	p = xdr_reserve_space(xdr, 24);
	if (!p)
		return -E2BIG;

	p = xdr_encode_hyper(p, ioerr->oer_comp_offset);
	p = xdr_encode_hyper(p, ioerr->oer_comp_length);
	*p++ = cpu_to_be32(ioerr->oer_iswrite);
	*p   = cpu_to_be32(ioerr->oer_errno);

	return 0;
}
