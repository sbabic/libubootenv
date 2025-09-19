/*
 * (C) Copyright 2023
 * Stefano Babic, <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

/**
 * @file uboot_mtd.c
 *
 * @brief function used with MTD subsystem
 *
 */

#if !defined(__FreeBSD__)
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <linux/fs.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <mtd/mtd-user.h>
#include <mtd/ubi-user.h>
#include "uboot_private.h"

static int ubi_get_dev_id(char *device)
{
	int dev_id = -1;
	char *sep;

	sep = strrchr(device, 'i');
	if (sep)
		sscanf(sep + 1, "%d", &dev_id);

	return dev_id;
}

static int mtd_get_dev_id(char *device)
{
	int dev_id = -1;
	char *sep;

	sep = strrchr(device, 'd');
	if (sep)
		sscanf(sep + 1, "%d", &dev_id);

	return dev_id;
}

static int is_nand_badblock(struct uboot_flash_env *dev, loff_t start)
{
	int bad;
	if (dev->mtdinfo.type != MTD_NANDFLASH)
		return 0;
	bad = ioctl(dev->fd, MEMGETBADBLOCK, &start);
	return bad;
}


static int ubi_get_dev_id_from_mtd(char *device)
{
	DIR *sysfs_ubi;
	struct dirent *dirent;
	int mtd_id;

	mtd_id = mtd_get_dev_id(device);
	if (mtd_id < 0)
		return -1;

	sysfs_ubi = opendir(SYS_UBI);
	if (!sysfs_ubi)
		return -1;

	while (1) {
		int ubi_num, ret;

		dirent = readdir(sysfs_ubi);
		if (!dirent)
			break;
		if (strlen(dirent->d_name) >= 255) {
			closedir(sysfs_ubi);
			return -1;
		}

		ret = sscanf(dirent->d_name, "ubi%d", &ubi_num);
		if (ret == 1) {
			char filename[DEVNAME_MAX_LENGTH];
			char data[DEVNAME_MAX_LENGTH];
			int fd, n, num_mtd = -1;

			snprintf(filename, sizeof(filename), SYS_UBI_MTD_NUM, ubi_num);
			fd = open(filename, O_RDONLY);
			if (fd < 0)
				continue;

			n = read(fd, data, sizeof(data));
			close(fd);
			if (n < 0)
				continue;

			if (sscanf(data, "%d", &num_mtd) != 1)
				num_mtd = -1;

			if (num_mtd < 0)
				continue;

			if (num_mtd == mtd_id) {
				closedir(sysfs_ubi);
				return ubi_num;
			}
		}
	}

	closedir(sysfs_ubi);
	return -1;
}

static int ubi_get_num_volume(char *device)
{
	char filename[DEVNAME_MAX_LENGTH];
	char data[DEVNAME_MAX_LENGTH];
	int dev_id, fd, n, num_vol = -1;

	dev_id = ubi_get_dev_id(device);
	if (dev_id < 0)
		return -1;

	snprintf(filename, sizeof(filename), SYS_UBI_VOLUME_COUNT, dev_id);
	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return -1;

	n = read(fd, data, sizeof(data));
	if (n < 0)
		goto out;

	if (sscanf(data, "%d", &num_vol) != 1)
		num_vol = -1;

out:
	close(fd);
	return num_vol;
}

static int ubi_get_volume_name(char *device, int vol_id, char vol_name[DEVNAME_MAX_LENGTH])
{
	char filename[80];
	char data[DEVNAME_MAX_LENGTH];
	int dev_id, fd, n, ret = -1;

	dev_id = ubi_get_dev_id(device);
	if (dev_id < 0)
		return -1;

	snprintf(filename, sizeof(filename), SYS_UBI_VOLUME_NAME, dev_id, dev_id, vol_id);
	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return -1;

	memset(data, 0, DEVNAME_MAX_LENGTH);
	n = read(fd, data, DEVNAME_MAX_LENGTH);
	if (n < 0)
		goto out;

	memset(vol_name, 0, DEVNAME_MAX_LENGTH);
	if (sscanf(data, "%s", vol_name) != 1)
		goto out;

	ret = 0;

out:
	close(fd);
	return ret;
}

static int ubi_get_vol_id(char *device, char *volname)
{
	int i, n, ret, num_vol, vol_id = -1;

	num_vol = ubi_get_num_volume(device);
	if (num_vol < 0)
		goto out;

	i = 0;
	n = 0;
	while ((n < num_vol) && (i < UBI_MAX_VOLUME))
	{
		char name[DEVNAME_MAX_LENGTH];

		ret = ubi_get_volume_name(device, i, name);
		if (!ret && !strcmp(name, volname)) {
			vol_id = i;
			break;
		}

		i++;
		if (!ret)
			n++;
	}

out:
	return vol_id;
}

int libubootenv_ubi_update_name(struct uboot_flash_env *dev)
{
	const size_t VOLNAME_MAX_LENGTH = DEVNAME_MAX_LENGTH - 20;
	char device[DEVNAME_MAX_LENGTH];
	char volume[VOLNAME_MAX_LENGTH];
	int dev_id, vol_id, fd, ret = -EBADF;
	char *sep;

	if (!strncmp(dev->devname, DEVICE_MTD_NAME, strlen(DEVICE_MTD_NAME)))
	{
		sep = strchr(dev->devname, DEVNAME_SEPARATOR);
		if (sep)
		{
			memset(device, 0, DEVNAME_MAX_LENGTH);
			memcpy(device, dev->devname, sep - dev->devname);

			memset(volume, 0, VOLNAME_MAX_LENGTH);
			sscanf(sep + 1, "%s", &volume[0]);

			ret = ubi_get_dev_id_from_mtd(device);
			if (ret < 0) {
				struct ubi_attach_req req;

				memset(&req, 0, sizeof(struct ubi_attach_req));
				req.ubi_num = UBI_DEV_NUM_AUTO;
				req.mtd_num = mtd_get_dev_id(device);
				req.vid_hdr_offset = 0;

				fd = open(DEVICE_UBI_CTRL, O_RDONLY);
				if (fd == -1)
					return -EBADF;

				ret = ioctl(fd, UBI_IOCATT, &req);
				close(fd);
				if (ret == -1) {
					/* Handle race condition where MTD was already being attached. */
					if (errno == EEXIST) {
						ret = ubi_get_dev_id_from_mtd(device);
						if (ret >= 0)
							req.ubi_num = ret;
						else
							return -EBADF;
					} else {
						return -EBADF;
					}
				}

				snprintf(dev->devname, sizeof(dev->devname) - 1, DEVICE_UBI_NAME"%d:%s", req.ubi_num, volume);
			} else {
				snprintf(dev->devname, sizeof(dev->devname) - 1, DEVICE_UBI_NAME"%d:%s", ret, volume);
			}
		} else {
			return -EBADF;
		}
	}

	sep = strchr(dev->devname, DEVNAME_SEPARATOR);
	ret = 0;
	if (sep)
	{
		memset(device, 0, DEVNAME_MAX_LENGTH);
		memcpy(device, dev->devname, sep - dev->devname);

		memset(volume, 0, VOLNAME_MAX_LENGTH);
		sscanf(sep + 1, "%s", &volume[0]);

		dev_id = ubi_get_dev_id(device);
		if (dev_id < 0)
			goto out;

		vol_id = ubi_get_vol_id(device, volume);
		if (vol_id < 0)
			goto out;

		if (snprintf(dev->devname, sizeof(dev->devname) - 1, "%s_%d", device, vol_id) < 0)
			ret = -EBADF;
	}


out:
	return ret;
}

int libubootenv_mtdread(struct uboot_flash_env *dev, void *data)
{
	size_t count;
	size_t blocksize;
	loff_t start;
	int sectors, skip;
	int ret = 0;

	switch (dev->mtdinfo.type) {
	case MTD_ABSENT:
	case MTD_NORFLASH:
		if (dev->offset)
			if (lseek(dev->fd, dev->offset, SEEK_SET) < 0) {
				ret = -EIO;
				break;
			}
		ret = read(dev->fd, data, dev->envsize);
		break;
	case MTD_NANDFLASH:
		if (dev->offset)
			if (lseek(dev->fd, dev->offset, SEEK_SET) < 0) {
				ret = -EIO;
				break;
			}

		count = dev->envsize;
		start = dev->offset;
		blocksize = dev->envsize;
		sectors = dev->envsectors ? dev->envsectors : 1;

		while (count > 0) {
			skip = is_nand_badblock(dev, start);
			if (skip < 0) {
				return -EIO;
			}

			if (skip > 0) {
				start += dev->sectorsize;
				sectors--;
				if (sectors > 0)
					continue;
				return  -EIO;
			}

			if (count > dev->sectorsize)
				blocksize = dev->sectorsize;
			else
				blocksize = count;

			if (lseek(dev->fd, start, SEEK_SET) < 0) {
				return -EIO;
			}
			if (read(dev->fd, data, blocksize) != blocksize) {
				return -EIO;
			}
			start += dev->sectorsize;
			data += blocksize;
			count -= blocksize;
			ret += blocksize;
		}
		break;
	}

	return ret;
}

int libubootenv_ubiread(struct uboot_flash_env *dev, void *data)
{
	int ret = 0;

	ret = read(dev->fd, data, dev->envsize);

	return ret;
}

int libubootenv_mtdwrite(struct uboot_flash_env *dev, void *data)
{
	int ret = 0;
	struct erase_info_user erase;
	size_t count;
	size_t blocksize;
	loff_t start;
	void *buf;
	int sectors, skip;

	switch (dev->mtdinfo.type) {
	case MTD_NORFLASH:
	case MTD_NANDFLASH:
		count = dev->envsize;
		start = dev->offset;
		blocksize = dev->envsize;
		erase.length = dev->sectorsize;
		sectors = dev->envsectors ? dev->envsectors : 1;
		buf = data;
		while (count > 0) {
			erase.start = start;

			skip = is_nand_badblock(dev, start);
			if (skip < 0) {
				ret = -EIO;
				goto devwrite_out;
			}

			if (skip > 0) {
				start += dev->sectorsize;
				sectors--;
				if (sectors > 0)
					continue;
				ret = -EIO;
				goto devwrite_out;
			}

			if (count > dev->sectorsize)
				blocksize = dev->sectorsize;
			else
				blocksize = count;

			/*
			 * unlock could fail, no check
			 */
			MTDUNLOCK(dev, &erase);
			if (ioctl(dev->fd, MEMERASE, &erase) != 0) {
				ret =-EIO;
				goto devwrite_out;
			}
			if (lseek(dev->fd, start, SEEK_SET) < 0) {
				ret =-EIO;
				goto devwrite_out;
			}
			if (write(dev->fd, buf, blocksize) != blocksize) {
				ret =-EIO;
				goto devwrite_out;
			}
			MTDLOCK(dev, &erase);
			start += dev->sectorsize;
			buf += blocksize;
			count -= blocksize;
			ret += blocksize;
		}
		break;
	}

devwrite_out:
	return ret;
}

int libubootenv_ubi_update_volume(struct uboot_flash_env *dev)
{
	int64_t envsize = dev->envsize;
	return ioctl(dev->fd, UBI_IOCVOLUP, &envsize);
}

int libubootenv_ubiwrite(struct uboot_flash_env *dev, void *data)
{
	int ret;

	if (libubootenv_ubi_update_volume(dev) < 0)
		return -1;

	ret = write(dev->fd, data, dev->envsize);

	return ret;
}

int libubootenv_mtdgetinfo(int fd, struct uboot_flash_env *dev)
{
	return ioctl(fd, MEMGETINFO, &dev->mtdinfo);
}

int libubootenv_set_obsolete_flag(struct uboot_flash_env *dev)
{
	uint8_t offsetflags = offsetof(struct uboot_env_redund, flags);
	unsigned char flag = 0;
	struct erase_info_user erase;
	int ret = 0;

	dev->fd = open(dev->devname, O_RDWR);
	if (dev->fd < 0)
		return -EBADF;
	if (lseek(dev->fd, dev->offset + offsetflags, SEEK_SET) < 0) {
		close(dev->fd);
		return -EBADF;
	}
	erase.start = dev->offset;
	erase.length = dev->sectorsize;
	MTDUNLOCK(dev, &erase);
	ret = write(dev->fd, &flag, sizeof(flag));
	if (ret == sizeof(flag))
		ret = 0;
	else if (ret >= 0)
		ret = -EIO;
	MTDLOCK(dev, &erase);
	close(dev->fd);

	return ret;
}
#endif
