/*
 *  pnfs_osd_xdr_enc.c
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

#include <linux/sunrpc/svc.h>
#include <linux/nfsd/nfsd.h>
#include <linux/nfsd/state.h>
#include <linux/nfsd/nfs4layoutxdr.h>
#include <linux/nfsd/nfsd4_pnfs.h>
#include <linux/nfsd/xdr4.h>

#include "pnfs_osd_xdr.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS

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
static int pnfs_osd_xdr_encode_data_map(
	u32 **pp, u32 *end,
	struct pnfs_osd_data_map *data_map)
{
	u32 *p = *pp;

	if (p + 7 > end)
		return -E2BIG;

	*p++ = cpu_to_be32(data_map->odm_num_comps);
	p = xdr_encode_hyper(p, data_map->odm_stripe_unit);
	*p++ = cpu_to_be32(data_map->odm_group_width);
	*p++ = cpu_to_be32(data_map->odm_group_depth);
	*p++ = cpu_to_be32(data_map->odm_mirror_cnt);
	*p++ = cpu_to_be32(data_map->odm_raid_algorithm);
	*pp = p;

	return 0;
}

/*
 * struct pnfs_osd_objid {
 * 	struct pnfs_deviceid	oid_device_id;
 * 	u64			oid_partition_id;
 * 	u64			oid_object_id;
 * };
 */
static inline int pnfs_osd_xdr_encode_objid(
	u32 **pp, u32 *end,
	struct pnfs_osd_objid *object_id)
{
	u32 *p = *pp;
	deviceid_t *dev_id = (deviceid_t *)&object_id->oid_device_id;

	if (p + 8 > end)
		return -E2BIG;

	p = xdr_encode_hyper(p, dev_id->pnfs_fsid);
	p = xdr_encode_hyper(p, dev_id->pnfs_devid);
	p = xdr_encode_hyper(p, object_id->oid_partition_id);
	p = xdr_encode_hyper(p, object_id->oid_object_id);
	*pp = p;

	return 0;
}

/*
 * enum pnfs_osd_cap_key_sec4 {
 * 	PNFS_OSD_CAP_KEY_SEC_NONE = 0,
 * 	PNFS_OSD_CAP_KEY_SEC_SSV  = 1
 * };
 *
 * struct pnfs_osd_object_cred {
 * 	struct pnfs_osd_objid		oc_object_id;
 * 	u32				oc_osd_version;
 * 	u32				oc_cap_key_sec;
 * 	struct pnfs_osd_opaque_cred	oc_cap_key
 * 	struct pnfs_osd_opaque_cred	oc_cap;
 * };
 */
static int pnfs_osd_xdr_encode_object_cred(
	u32 **pp, u32 *end, struct pnfs_osd_object_cred *olo_comp)
{
	u32 *p = *pp;
	int err;

	err = pnfs_osd_xdr_encode_objid(&p, end, &olo_comp->oc_object_id);
	if (err)
		return err;

	if (p + 4 > end)
		return -E2BIG;

	*p++ = cpu_to_be32(olo_comp->oc_osd_version);

	/* No sec for now */
	*p++ = cpu_to_be32(PNFS_OSD_CAP_KEY_SEC_NONE);
	*p++ = cpu_to_be32(0); /*opaque oc_capability_key<>*/

	*pp = xdr_encode_opaque(p, olo_comp->oc_cap.cred,
				olo_comp->oc_cap.cred_len);

	return 0;
}

/*
 * struct pnfs_osd_layout {
 * 	struct pnfs_osd_data_map	olo_map;
 * 	u32				olo_comps_index;
 * 	u32				olo_num_comps;
 * 	struct pnfs_osd_object_cred	*olo_comps;
 * };
 */
int pnfs_osd_xdr_encode_layout(
	u32 **pp, u32 *end,
	struct pnfs_osd_layout *pol)
{
	u32 *p = *pp;

	u32 i;
	int err;

	err = pnfs_osd_xdr_encode_data_map(&p, end, &pol->olo_map);
	if (err)
		return err;

	if (p + 2 > end)
		return -E2BIG;

	*p++ = cpu_to_be32(pol->olo_comps_index);
	*p++ = cpu_to_be32(pol->olo_num_comps);

	for (i = 0; i < pol->olo_num_comps; i++) {
		err = pnfs_osd_xdr_encode_object_cred(
					&p, end, &pol->olo_comps[i]);
		if (err)
			return err;
	}

	*pp = p;
	return 0;
}

static int _encode_string(u32 **pp, u32 *end, struct nfs4_string *str)
{
	u32 *p = *pp;

	if (p + 1 + XDR_QUADLEN(str->len) > end)
		return -E2BIG;
	p = xdr_encode_opaque(p, str->data, str->len);

	*pp = p;
	return 0;
}

/* struct pnfs_osd_deviceaddr {
 * 	struct pnfs_osd_targetid	oda_targetid;
 * 	struct pnfs_osd_targetaddr	oda_targetaddr;
 * 	u8				oda_lun[8];
 * 	struct nfs4_string		oda_systemid;
 * 	struct pnfs_osd_object_cred	oda_root_obj_cred;
 * 	struct nfs4_string		oda_osdname;
 * };
 */
int pnfs_osd_xdr_encode_deviceaddr(
	u32 **pp, u32 *end, struct pnfs_osd_deviceaddr *devaddr)
{
	u32 *p = *pp;
	int err;

	if (p + 1 + 1 + sizeof(devaddr->oda_lun)/4 > end)
		return -E2BIG;

	/* Empty oda_targetid */
	*p++ = cpu_to_be32(OBJ_TARGET_ANON);

	/* Empty oda_targetaddr for now */
	*p++ = cpu_to_be32(0);

	/* oda_lun */
	p = xdr_encode_opaque_fixed(p, devaddr->oda_lun,
				    sizeof(devaddr->oda_lun));

	err = _encode_string(&p, end, &devaddr->oda_systemid);
	if (err)
		return err;

	err = pnfs_osd_xdr_encode_object_cred(&p, end,
					      &devaddr->oda_root_obj_cred);
	if (err)
		return err;

	err = _encode_string(&p, end, &devaddr->oda_osdname);
	if (err)
		return err;

	*pp = p;
	return 0;
}

/*
 * struct pnfs_osd_layoutupdate {
 * 	u32	dsu_valid;
 * 	s64	dsu_delta;
 * 	u32	olu_ioerr_flag;
 * };
 */
__be32 *
pnfs_osd_xdr_decode_layoutupdate(struct pnfs_osd_layoutupdate *lou, __be32 *p)
{
	lou->dsu_valid = be32_to_cpu(*p++);
	if (lou->dsu_valid)
		p = xdr_decode_hyper(p, &lou->dsu_delta);
	lou->olu_ioerr_flag = be32_to_cpu(*p++);
	return p;
}

/*
 * struct pnfs_osd_objid {
 * 	struct pnfs_deviceid	oid_device_id;
 * 	u64			oid_partition_id;
 * 	u64			oid_object_id;
 * };
 */
static inline __be32 *
pnfs_osd_xdr_decode_objid(__be32 *p, struct pnfs_osd_objid *objid)
{
	/* FIXME: p = xdr_decode_fixed(...) */
	memcpy(objid->oid_device_id.data, p, sizeof(objid->oid_device_id.data));
	p += XDR_QUADLEN(sizeof(objid->oid_device_id.data));

	p = xdr_decode_hyper(p, &objid->oid_partition_id);
	p = xdr_decode_hyper(p, &objid->oid_object_id);
	return p;
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
__be32 *
pnfs_osd_xdr_decode_ioerr(struct pnfs_osd_ioerr *ioerr, __be32 *p)
{
	p = pnfs_osd_xdr_decode_objid(p, &ioerr->oer_component);
	p = xdr_decode_hyper(p, &ioerr->oer_comp_offset);
	p = xdr_decode_hyper(p, &ioerr->oer_comp_length);
	ioerr->oer_iswrite = be32_to_cpu(*p++);
	ioerr->oer_errno = be32_to_cpu(*p++);
	return p;
}
