/*
 * Copyright (C) 2012
 * Sachin Bhamare <sbhamare@panasas.com>
 *
 * This file is part of exofs.
 *
 * exofs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.  Since it is based on ext2, and the only
 * valid version of GPL for the Linux kernel is version 2, the only valid
 * version of GPL for exofs is version 2.
 *
 * exofs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with exofs; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __SYS_DOT_H__
#define __SYS_DOT_H__

struct exofs_dev;
struct exofs_sb_info;
struct exofs_dt_device_info;

int exofs_sysfs_odev_add(struct exofs_dev *edev,
			 struct exofs_sb_info *sbi);
int exofs_sysfs_cluster_add(struct exofs_dt_device_info *dt_dev,
		struct exofs_sb_info *sbi);
void exofs_sysfs_cluster_del(struct exofs_sb_info *sbi);

void exofs_sysfs_print(void);
int exofs_sysfs_init(void);
void exofs_sysfs_uninit(void);

#endif /* __SYS_DOT_H__ */
