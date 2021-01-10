/*
 * (C) Copyright 2019
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

/**
 * @file uboot_env.c
 *
 * @brief This is the implementation of libubootenv library
 *
 */
 
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
#include <zlib.h>
#include <mtd/mtd-user.h>
#include <mtd/ubi-user.h>

#include "uboot_private.h"

#define UBI_MAX_VOLUME			128

#define DEVICE_MTD_NAME 		"/dev/mtd"
#define DEVICE_UBI_NAME 		"/dev/ubi"
#define SYS_UBI_VOLUME_COUNT		"/sys/class/ubi/ubi%d/volumes_count"
#define SYS_UBI_VOLUME_NAME		"/sys/class/ubi/ubi%d/ubi%d_%d/name"

#define	LIST_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = LIST_FIRST((head));				\
	    (var) != NULL &&						\
	    ((tvar) = LIST_NEXT((var), field), 1);			\
	    (var) = (tvar))

/*
 * The lockfile is the same as defined in U-Boot for
 * the fw_printenv utilities
 */
static const char *lockname = "/var/lock/fw_printenv.lock";
static int libuboot_lock(struct uboot_ctx *ctx)
{
	int lockfd = -1;
	lockfd = open(lockname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (lockfd < 0) {
		return -EBUSY;
	}
	if (flock(lockfd, LOCK_EX) < 0) {
		close(lockfd);
		return -EIO;
	}

	ctx->lock = lockfd;
	return 0;
}

static void libuboot_unlock(struct uboot_ctx *ctx)
{
	if (ctx && (ctx->lock > 0)) {
		flock(ctx->lock, LOCK_UN);
		close(ctx->lock);
		ctx->lock = -1;
	}
}

static char attr_tostring(type_attribute a)
{
	switch(a) {
	case TYPE_ATTR_STRING:
		return 's';
	case TYPE_ATTR_DECIMAL:
		return 'd';
	case TYPE_ATTR_HEX:
		return 'h';
	case TYPE_ATTR_BOOL:
		return 'b';
	case TYPE_ATTR_IP:
		return 'i';
	case TYPE_ATTR_MAC:
		return 'm';
	}

	return 's';
}

static char access_tostring(access_attribute a)
{
	switch(a) {
	case ACCESS_ATTR_ANY:
		return 'a';
	case ACCESS_ATTR_READ_ONLY:
		return 'r';
	case ACCESS_ATTR_WRITE_ONCE:
		return 'o';
	case ACCESS_ATTR_CHANGE_DEFAULT:
		return 'c';
	}

	return 'a';
}

static struct var_entry *__libuboot_get_env(struct vars *envs, const char *varname)
{
	struct var_entry *entry;

	LIST_FOREACH(entry, envs, next) {
		if (strcmp(varname, entry->name) == 0)
			return entry;
	}

	return NULL;
}

static void free_var_entry(struct vars *envs, struct var_entry *entry)
{
	if (entry) {
		LIST_REMOVE(entry, next);
		free(entry->name);
		free(entry->value);
		free(entry);
	}
}

static void remove_var(struct vars *envs, const char *varname)
{
	struct var_entry *entry;

	entry = __libuboot_get_env(envs, varname);

	free_var_entry(envs, entry);
}

static enum device_type get_device_type(char *device)
{
	enum device_type type = DEVICE_NONE;

	if (!strncmp(device, DEVICE_MTD_NAME, strlen(DEVICE_MTD_NAME)))
		type = DEVICE_MTD;
	else if (!strncmp(device, DEVICE_UBI_NAME, strlen(DEVICE_UBI_NAME)))
		type = DEVICE_UBI;
	else if (strlen(device) > 0)
		type = DEVICE_FILE;

	return type;
}

static int ubi_get_dev_id(char *device)
{
	int dev_id = -1;
	char *sep;

	sep = rindex(device, 'i');
	if (sep)
		sscanf(sep + 1, "%d", &dev_id);

	return dev_id;
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

static int ubi_update_name(struct uboot_flash_env *dev)
{
	char device[DEVNAME_MAX_LENGTH];
	char volume[DEVNAME_MAX_LENGTH];
	int dev_id, vol_id, ret = -EBADF;
	char *sep;

	sep = index(dev->devname, DEVNAME_SEPARATOR);
	if (sep)
	{
		memset(device, 0, DEVNAME_MAX_LENGTH);
		memcpy(device, dev->devname, sep - dev->devname);

		memset(volume, 0, DEVNAME_MAX_LENGTH);
		sscanf(sep + 1, "%s", &volume[0]);

		dev_id = ubi_get_dev_id(device);
		if (dev_id < 0)
			goto out;

		vol_id = ubi_get_vol_id(device, volume);
		if (vol_id < 0)
			goto out;

		sprintf(dev->devname, "%s_%d", device, vol_id);
	}

	ret = 0;

out:
	return ret;
}

static int normalize_device_path(char *path, struct uboot_flash_env *dev)
{
	char *sep = NULL, *normalized = NULL;
	size_t normalized_len = 0, volume_len = 0, output_len = 0;

	/*
	 * if volume name is present, split into device path and volume
	 * since only the device path needs normalized
	 */
	sep = index(path, DEVNAME_SEPARATOR);
	if (sep)
	{
		volume_len = strlen(sep);
		*sep = '\0';
	}

	if ((normalized = realpath(path, NULL)) == NULL)
	{
		/* device file didn't exist */
		return -EINVAL;
	}

	normalized_len = strlen(normalized);
	output_len = sizeof(dev->devname) - 1; /* leave room for null */
	if ((normalized_len + volume_len) > output_len)
	{
		/* full name is too long to fit */
		free(normalized);
		return -EINVAL;
	}

	/*
	 * save normalized path to device file,
	 * and possibly append separator char & volume name
	 */
	memset(dev->devname, 0, sizeof(dev->devname));
	strncpy(dev->devname, normalized, output_len);
	free(normalized);

	if (sep)
	{
		*sep = DEVNAME_SEPARATOR;
		strncpy(dev->devname + normalized_len, sep, output_len - normalized_len);
	}

	return 0;
}

static int check_env_device(struct uboot_ctx *ctx, struct uboot_flash_env *dev)
{
	int fd, ret;
	struct stat st;

	dev->device_type = get_device_type(dev->devname);
	if (dev->device_type == DEVICE_NONE)
		return -EBADF;

	if (dev->device_type == DEVICE_UBI) {
		ret = ubi_update_name(dev);
		if (ret)
			return ret;
	}

	ret = stat(dev->devname, &st);
	if (ret < 0)
		return -EBADF;
	fd = open(dev->devname, O_RDONLY);
	if (fd < 0)
		return -EBADF;

	if (S_ISCHR(st.st_mode)) {
		if (dev->device_type == DEVICE_MTD) {
			ret = ioctl(fd, MEMGETINFO, &dev->mtdinfo);
			if (ret < 0 || (dev->mtdinfo.type != MTD_NORFLASH &&
					dev->mtdinfo.type != MTD_NANDFLASH)) {
				close(fd);
				return -EBADF;
			}
			if (dev->sectorsize == 0) {
				dev->sectorsize = dev->mtdinfo.erasesize;
			}
		}
	}

	switch (dev->device_type) {
	case DEVICE_FILE:
		dev->flagstype = FLAGS_INCREMENTAL;
		break;
	case DEVICE_MTD:
		switch (dev->mtdinfo.type) {
		case MTD_NORFLASH:
			dev->flagstype = FLAGS_BOOLEAN;
			break;
		case MTD_NANDFLASH:
			dev->flagstype = FLAGS_INCREMENTAL;
		};
		break;
	case DEVICE_UBI:
		dev->flagstype = FLAGS_INCREMENTAL;
		break;
	default:
		close(fd);
		return -EBADF;
	};

	/*
	 * Check for negative offsets, treat it as backwards offset
	 * from the end of the block device
	 */
	if (dev->offset < 0) {
		uint64_t blkdevsize;
		int rc;

		rc = ioctl(fd, BLKGETSIZE64, &blkdevsize);
		if (rc < 0) {
			close(fd);
			return -EINVAL;
		}

		dev->offset += blkdevsize;
	}

	close(fd);

	return 0;
}

static bool check_compatible_devices(struct uboot_ctx *ctx)
{
	if (!ctx->redundant)
		return true;

	if (ctx->envdevs[0].mtdinfo.type != ctx->envdevs[1].mtdinfo.type) 
		return false;
	if (ctx->envdevs[0].flagstype != ctx->envdevs[1].flagstype) 
		return false;
	if (ctx->envdevs[0].envsize != ctx->envdevs[1].envsize) 
		return false;

	return true;
}

static int is_nand_badblock(struct uboot_flash_env *dev, loff_t start)
{
	int bad;
	if (dev->mtdinfo.type != MTD_NANDFLASH)
		return 0;
	bad = ioctl(dev->fd, MEMGETBADBLOCK, &start);
	return bad;
}

static int fileread(struct uboot_flash_env *dev, void *data)
{
	int ret = 0;

	if (dev->offset)
		ret = lseek(dev->fd, dev->offset, SEEK_SET);

	if (ret < 0)
		return ret;

	size_t remaining = dev->envsize;

	while (1) {
		ret = read(dev->fd, data, remaining);

		if (ret == 0 && remaining > 0)
		    return -1;

		if (ret < 0)
			break;

		remaining -= ret;
		data += ret;

		if (!remaining) {
			ret = dev->envsize;
			break;
		}
	}

	return ret;
}

static int mtdread(struct uboot_flash_env *dev, void *data)
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

static int ubiread(struct uboot_flash_env *dev, void *data)
{
	int ret = 0;

	ret = read(dev->fd, data, dev->envsize);

	return ret;
}

static int devread(struct uboot_ctx *ctx, unsigned int copy, void *data)
{
	int ret;
	struct uboot_flash_env *dev;

	if (copy > 1)
		return -EINVAL;

	dev = &ctx->envdevs[copy];

	dev->fd = open(dev->devname, O_RDONLY);
	if (dev->fd < 0)
		return -EBADF;

	switch (dev->device_type) {
	case DEVICE_FILE:
		ret = fileread(dev, data);
		break;
	case DEVICE_MTD:
		ret = mtdread(dev, data);
		break;
	case DEVICE_UBI:
		ret = ubiread(dev, data);
		break;
	default:
		ret = -1;
		break;
	};

	close(dev->fd);
	return ret;
}

static int fileprotect(struct uboot_flash_env *dev, bool on)
{
	const char c_sys_path_1[] = "/sys/class/block/";
	const char c_sys_path_2[] = "/force_ro";
	const char c_dev_name_1[] = "mmcblk";
	const char c_dev_name_2[] = "boot";
	const char c_unprot_char = '0';
	const char c_prot_char = '1';
	const char *devfile = dev->devname;
	int ret = 0;  // 0 means OK, negative means error
	int ret_int = 0;
	char *sysfs_path = NULL;
	int fd_force_ro;

	// Devices without ro flag at /sys/class/block/mmcblk?boot?/force_ro are ignored
	if (strncmp("/dev/", dev->devname, 5) == 0) {
		devfile = dev->devname + 5;
	} else {
		return ret;
	}

	ret_int = strncmp(devfile, c_dev_name_1, sizeof(c_dev_name_1) - 1);
	if (ret_int != 0) {
		return ret;
	}

	if (strncmp(devfile + sizeof(c_dev_name_1), c_dev_name_2, sizeof(c_dev_name_2) - 1) != 0) {
		return ret;
	}

	if (*(devfile + sizeof(c_dev_name_1) - 1) < '0' ||
	    *(devfile + sizeof(c_dev_name_1) - 1) > '9') {
		return ret;
	}

	if (*(devfile + sizeof(c_dev_name_1) + sizeof(c_dev_name_2) - 1) < '0' ||
	    *(devfile + sizeof(c_dev_name_1) + sizeof(c_dev_name_2) - 1) > '9') {
		return ret;
	}

	// There is a ro flag, the device needs to be protected or unprotected
	ret_int = asprintf(&sysfs_path, "%s%s%s", c_sys_path_1, devfile, c_sys_path_2);
	if(ret_int < 0) {
		ret = -ENOMEM;
		goto fileprotect_out;
	}

	if (access(sysfs_path, W_OK) == -1) {
		goto fileprotect_out;
	}

	fd_force_ro = open(sysfs_path, O_RDWR);
	if (fd_force_ro == -1) {
		ret = -EBADF;
		goto fileprotect_out;
	}

	if(on == false){
		ret_int = write(fd_force_ro, &c_unprot_char, 1);
	} else {
		fsync(dev->fd);
		ret_int = write(fd_force_ro, &c_prot_char, 1);
	}
	close(fd_force_ro);

fileprotect_out:
	if(sysfs_path)
		free(sysfs_path);
	return ret;
}

static int filewrite(struct uboot_flash_env *dev, void *data)
{
	int ret = 0;

	ret = fileprotect(dev, false);
	if (ret < 0)
		return ret;

	if (dev->offset)
		ret = lseek(dev->fd, dev->offset, SEEK_SET);

	if (ret < 0)
		return ret;

	size_t remaining = dev->envsize;

	while (1) {
		ret = write(dev->fd, data, remaining);

		if (ret < 0)
			break;

		remaining -= ret;
		data += ret;

		if (!remaining) {
			ret = dev->envsize;
			break;
		}
	}

	fileprotect(dev, true);  // no error handling, keep ret from write

	return ret;
}

static int mtdwrite(struct uboot_flash_env *dev, void *data)
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
			ioctl(dev->fd, MEMUNLOCK, &erase);
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
			ioctl(dev->fd, MEMLOCK, &erase);
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

static int ubi_update_volume(struct uboot_flash_env *dev)
{
	int64_t envsize = dev->envsize;
	return ioctl(dev->fd, UBI_IOCVOLUP, &envsize);
}

static int ubiwrite(struct uboot_flash_env *dev, void *data)
{
	int ret;

	if (ubi_update_volume(dev) < 0)
		return -1;

	ret = write(dev->fd, data, dev->envsize);

	return ret;
}

static int devwrite(struct uboot_ctx *ctx, unsigned int copy, void *data)
{
	int ret;
	struct uboot_flash_env *dev;

	if (copy > 1)
		return -EINVAL;

	dev = &ctx->envdevs[copy];
	dev->fd = open(dev->devname, O_RDWR);
	if (dev->fd < 0)
		return -EBADF;

	switch (dev->device_type) {
	case DEVICE_FILE:
		ret = filewrite(dev, data);
		break;
	case DEVICE_MTD:
		ret = mtdwrite(dev, data);
		break;
	case DEVICE_UBI:
		ret = ubiwrite(dev, data);
		break;
	default:
		ret = -1;
		break;
	};

	close(dev->fd);

	return ret;
}

static int set_obsolete_flag(struct uboot_flash_env *dev)
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
	ioctl(dev->fd, MEMUNLOCK, &erase);
	ret = write(dev->fd, &flag, sizeof(flag));
	if (ret == sizeof(flag))
		ret = 0;
	else if (ret >= 0)
		ret = -EIO;
	ioctl (dev->fd, MEMLOCK, &erase);
	close(dev->fd);

	return ret;
}

int libuboot_env_store(struct uboot_ctx *ctx)
{
	struct var_entry *entry;
	void *image;
	char *data;
	char *buf;
	bool saveflags = false;
	size_t size;
	uint8_t offsetdata;
	int ret;
	int copy;

	/*
	 * Allocate the bigger of the case
	 */
	image = malloc(sizeof(struct uboot_env_redund) + ctx->size);
	if (!image)
		return -ENOMEM;

	if (ctx->redundant)
		offsetdata = offsetof(struct uboot_env_redund, data);
	else
		offsetdata = offsetof(struct uboot_env_noredund, data);

	data = (uint8_t *)(image + offsetdata);

	buf = data;
	LIST_FOREACH(entry, &ctx->varlist, next) {
		size = (ctx->size - offsetdata)  - (buf - data); 
		if ((strlen(entry->name) + strlen(entry->value) + 2) > size) 
			return -ENOMEM;

		if (entry->type || entry->access)
			saveflags = true;

		buf += snprintf(buf, size, "%s=%s", entry->name, entry->value); 
		buf++;
	}

	/*
	 * Now save the .flags
	 */
	if (saveflags) {
		bool first = true;
		size = (ctx->size - offsetdata)  - (buf - data); 
		buf += snprintf(buf, size, ".flags=");

		LIST_FOREACH(entry, &ctx->varlist, next) {
			size = (ctx->size - offsetdata)  - (buf - data); 
			if (entry->type || entry->access) {
				buf += snprintf(buf, size, "%s%s:%c%c",
						first ? "" : ",",
						entry->name,
						attr_tostring(entry->type),
						access_tostring(entry->access));
				first = false;
			}
		}
		buf++;
	}
	*buf++ = '\0';

	if (ctx->redundant) {
		unsigned char flags = ctx->envdevs[ctx->current].flags;
		switch(ctx->envdevs[ctx->current].flagstype) {
		case FLAGS_INCREMENTAL:
			flags++;
			break;
		case FLAGS_BOOLEAN:
			flags = 1;
			break;
		}
		((struct uboot_env_redund *)image)->flags = flags;
	}

	*(uint32_t *)image = crc32(0, (uint8_t *)data, ctx->size - offsetdata);

	copy = ctx->redundant ? (ctx->current ? 0 : 1) : 0;
	ret = devwrite(ctx, copy, image);
	free(image);

	if (ret == ctx->size)
		ret = 0;

	if (ctx->redundant && !ret) {
		if (ctx->envdevs[ctx->current].flagstype == FLAGS_BOOLEAN)
			ret = set_obsolete_flag(&ctx->envdevs[ctx->current]);
	}

	if (!ret)
		ctx->current = ctx->current ? 0 : 1;

	return ret;
}

static int libuboot_load(struct uboot_ctx *ctx)
{
	int ret, i;
	int copies = 1;
	void *buf[2];
	size_t bufsize, usable_envsize;
	struct uboot_flash_env *dev;
	bool crcenv[2];
	unsigned char flags[2];
	char *line, *next;
	uint8_t offsetdata = offsetof(struct uboot_env_noredund, data);
	uint8_t offsetcrc = offsetof(struct uboot_env_noredund, crc);
	uint8_t offsetflags = offsetof(struct uboot_env_redund, flags);
	uint8_t *data;
	struct var_entry *entry;

	ctx->valid = false;
	usable_envsize = ctx->size - offsetdata;
    
	bufsize = ctx->size;
	if (ctx->redundant) {
		copies++;
		bufsize += ctx->size; 
		offsetdata = offsetof(struct uboot_env_redund, data);
		offsetcrc = offsetof(struct uboot_env_redund, crc);
	}
	buf[0] = malloc(bufsize);
	if (!buf[0])
		return -ENOMEM;

	if (copies > 1)
		buf[1] = buf[0] + ctx->envdevs[0].envsize; 

	for (i = 0; i < copies; i++) {
		data = (uint8_t *)(buf[i] + offsetdata);
		uint32_t crc;

		dev = &ctx->envdevs[i];
		ret = devread(ctx, i, buf[i]);
		if (ret != ctx->size) {
			free(buf[0]);
			return -EIO;
		}
		crc = *(uint32_t *)(buf[i] + offsetcrc);
		dev->crc = crc32(0, (uint8_t *)data, usable_envsize);
		crcenv[i] = dev->crc == crc;
		if (ctx->redundant)
			dev->flags = *(uint8_t *)(buf[i] + offsetflags);
	}
	if (!ctx->redundant) {
		ctx->current = 0;
		ctx->valid = crcenv[0];
	} else {
		if (crcenv[0] && !crcenv[1]) {
			ctx->valid = true;
			ctx->current = 0;
		} else if (!crcenv[0] && crcenv[1]) {
			ctx->valid = true;
			ctx->current = 1;
		} else if (!crcenv[0] && !crcenv[1]) {
			ctx->valid = false;
			ctx->current = 0;
		} else { /* both valid, check flags */
			ctx->valid = true;
			if (ctx->envdevs[1].flags > ctx->envdevs[0].flags)
				ctx->current = 1;
			else
				ctx->current = 0;
			switch (ctx->envdevs[0].flagstype) {
			case FLAGS_BOOLEAN:
				if (ctx->envdevs[1].flags == 0xFF) 
					ctx->current = 1;
				else if (ctx->envdevs[0].flags == 0xFF) 
					ctx->current = 0;
				break;
			case FLAGS_INCREMENTAL:
				/* check overflow */
				if (ctx->envdevs[0].flags == 0xFF && 
					ctx->envdevs[1].flags == 0)
					ctx->current = 1;
				else if (ctx->envdevs[1].flags == 0xFF && 
					ctx->envdevs[0].flags == 0)
					ctx->current = 0;
				break;
			}
		}
	}

#if !defined(NDEBUG)
	fprintf(stdout, "Environment %s, copy %d\n",
			ctx->valid ? "OK" : "WRONG", ctx->current);
#endif

	data = (uint8_t *)(buf[ctx->current] + offsetdata);

	char *flagsvar = NULL;

	if (ctx->valid) {
		for (line = data; *line; line = next + 1) {
			char *value;

			/*
			 * Search the end of the string pointed by line
			 */
			for (next = line; *next; ++next) {
				if ((next - (char *)data) > usable_envsize) {
					free(buf[0]);
					return -EIO;
				}
			}

			value = strchr(line, '=');
			if (!value)
				continue;

			*value++ = '\0';

			if (!strcmp(line, ".flags"))
				flagsvar = strdup(value);
			else
				libuboot_set_env(ctx, line, value);
		}
	}

	/*
	 * Parse .flags and set the attributes for a variable
	 */
	char *pvar;
	char *pval;
	if (flagsvar) {
#if !defined(NDEBUG)
	fprintf(stdout, "Environment FLAGS %s\n", flagsvar);
#endif
		pvar = flagsvar;

		while (*pvar && (pvar - flagsvar) < strlen(flagsvar)) {
			char *pnext;
			pval = strchr(pvar, ':');
			if (!pval)
				break;

			*pval++ = '\0';
			pnext = strchr(pval, ',');
			if (!pnext)
				pnext = flagsvar + strlen(flagsvar);
			else
				*pnext++ = '\0';

			entry = __libuboot_get_env(&ctx->varlist, pvar);
			if (entry) {
				for (int i = 0; i < strlen(pval); i++) {
					switch (pval[i]) {
					case 's':
						entry->type = TYPE_ATTR_STRING;
						break;
					case 'd':
						entry->type = TYPE_ATTR_DECIMAL;
						break;
					case 'x':
						entry->type = TYPE_ATTR_HEX;
						break;
					case 'b':
						entry->type = TYPE_ATTR_BOOL;
						break;
					case 'i':
						entry->type = TYPE_ATTR_IP;
						break;
					case 'm':
						entry->type = TYPE_ATTR_MAC;
						break;
					case 'a':
						entry->access = ACCESS_ATTR_ANY;
						break;
					case 'r':
						entry->access = ACCESS_ATTR_READ_ONLY;
						break;
					case 'o':
						entry->access = ACCESS_ATTR_WRITE_ONCE;
						break;
					case 'c':
						entry->access = ACCESS_ATTR_CHANGE_DEFAULT;
						break;
					default: /* ignore it */
						break;
					}
				}
			}

			pvar = pnext;
		}
	}
	free(flagsvar);

	free(buf[0]);

	return ctx->valid ? 0 : -ENODATA;
}

#define LINE_LENGTH 1024
int libuboot_load_file(struct uboot_ctx *ctx, const char *filename)
{
	FILE *fp;
	char *buf;
	char *name, *value;

	if (!filename)
		return -EBADF;

	if (strcmp(filename, "-") == 0)
		fp = fdopen(0, "r");
	else
		fp = fopen(filename, "r");
	if (!fp)
		return -EACCES;

	buf = (char *)malloc(LINE_LENGTH);
	if (!buf) {
		fclose(fp);
		return -ENOMEM;
	}

	while (fgets(buf, LINE_LENGTH, fp)) {
		int len = strlen(buf);

		while (len && (buf[len - 1] == '\n' || buf [len - 1] == '\r'))
			buf[--len] = '\0';

		/* Skip comment or empty lines */
		if (len == 0 || buf[0] == '#')
			continue;

		value = strchr(buf, '=');
		if (!value)
			continue;

		*value++ = '\0';

		name = buf;

		if (!strlen(value))
			value = NULL;

		libuboot_set_env(ctx, name, value);
	}
	fclose(fp);
	free(buf);

	return 0;
}

int libuboot_read_config(struct uboot_ctx *ctx, const char *config)
{
	FILE *fp;
	char *line = NULL;
	size_t bufsize = 0;
	int index = 0;
	int ret = 0;
	int ndev = 0;
	struct uboot_flash_env *dev;
	char *tmp;
	int retval = 0;

	if (!config)
		return -EINVAL;

	fp = fopen(config, "r");
	if (!fp)
		return -EBADF;

	dev = ctx->envdevs;
	ctx->size = 0;

	while (getline(&line, &bufsize, fp) != -1) {
		/* skip comments */
		if (line[0] == '#')
			continue;

		ret = sscanf(line, "%ms %lli %zx %zx %lx",
				&tmp,
				&dev->offset,
				&dev->envsize,
				&dev->sectorsize,
				&dev->envsectors);

		/*
		 * At least name offset and size should be set
		 */
		if (ret < 3 || !tmp)
			continue;

		/*
		 * If size is set but zero, entry is wrong
		 */
		if (!dev->envsize) {
			retval = -EINVAL;
			break;
		}

		if (!ctx->size)
			ctx->size = dev->envsize;

		if (tmp) {
			if (normalize_device_path(tmp, dev) < 0) {
				free(tmp);
				retval = -EINVAL;
				break;
			}
			free(tmp);
		}

		if (check_env_device(ctx, dev) < 0) {
			retval = -EINVAL;
			break;
		}

		ndev++;
		dev++; 

		if (ndev >= 2) {
			ctx->redundant = true;
			if (!check_compatible_devices(ctx))
				retval = -EINVAL;
			break;
		}
	}
	if (ndev == 0)
		retval = -EINVAL;

	fclose(fp);
	free(line);

	return retval;
}

static bool libuboot_validate_flags(struct var_entry *entry, const char *value)
{
	bool ok_type = true, ok_access = true;
	unsigned long long test;

	switch (entry->access) {
	case ACCESS_ATTR_ANY:
		ok_access = true;
		break;
	case ACCESS_ATTR_READ_ONLY:
	case ACCESS_ATTR_WRITE_ONCE:
		ok_access = false;
		break;
	case ACCESS_ATTR_CHANGE_DEFAULT:
		break;
	}

	if (!ok_access)
		return false;

	if (!value)
		return true;

	switch (entry->type) {
	case TYPE_ATTR_STRING:
		ok_type = true;
		break;
	case TYPE_ATTR_DECIMAL:
	case TYPE_ATTR_HEX:
		errno = 0;
		ok_type = strlen(value) > 2 && (value[0] == 0) &&
			(value[1] == 'x' || value [1] == 'X');
		if (ok_type) {
			test = strtoull(value, NULL, 16);
			if (errno)
				ok_type = false;
		}
		break;
	case TYPE_ATTR_BOOL:
		ok_access = (value[0] == '1' || value[0] == 'y' || value[0] == 't' ||
			value[0] == 'Y' || value[0] == 'T' ||
			value[0] == '0' || value[0] == 'n' || value[0] == 'f' ||
 			value[0] == 'N' || value[0] == 'F') && (strlen(value) != 1);
		break;
	case TYPE_ATTR_IP:
	case TYPE_ATTR_MAC:
		break;
	}
	return ok_type;
}

int libuboot_set_env(struct uboot_ctx *ctx, const char *varname, const char *value)
{
	struct var_entry *entry, *elm, *lastentry;
	struct vars *envs = &ctx->varlist;
	entry = __libuboot_get_env(envs, varname);
	if (entry) {
		if (libuboot_validate_flags(entry, value)) {
			if (!value) {
				free_var_entry(envs, entry);
			} else {
				free(entry->value);
				entry->value = strdup(value);
			}
			return 0;
		} else {
			return -EPERM;
		}
	}

	if (!value)
		return 0;

	entry = (struct var_entry *)calloc(1, sizeof(*entry));
	if (!entry)
		return -ENOMEM;
	entry->name = strdup(varname);
	if (!entry->name) {
		free(entry);
		return -ENOMEM;
	}
	entry->value = strdup(value);
	if (!entry->value) {
		free(entry->name);
		free(entry);
		return -ENOMEM;
	}
	lastentry = NULL;
	LIST_FOREACH(elm, envs, next) {
		if (strcmp(elm->name, varname) > 0) {
			LIST_INSERT_BEFORE(elm, entry, next);
			return 0;
		}
		lastentry = elm;
	}
	if (lastentry)
		LIST_INSERT_AFTER(lastentry, entry, next);
	else
		LIST_INSERT_HEAD(envs, entry, next);

	return 0;
}

char *libuboot_get_env(struct uboot_ctx *ctx, const char *varname)
{
	struct var_entry *entry;
	struct vars *envs = &ctx->varlist;

	entry = __libuboot_get_env(envs, varname);
	if (!entry)
		return NULL;

	return strdup(entry->value);
}

const char *libuboot_getname(void *entry)
{
	struct var_entry *e = entry;

	return e ? e->name : NULL;
}

const char *libuboot_getvalue(void *entry)
{
	struct var_entry *e = entry;

	return e ? e->value : NULL;
}

void *libuboot_iterator(struct uboot_ctx *ctx, void *next)
{

	if (!next)
		return  ctx->varlist.lh_first;
	else
		return ((struct var_entry *)next)->next.le_next;
}

int libuboot_configure(struct uboot_ctx *ctx,
			struct uboot_env_device *envdevs)
{
	if (envdevs) {
		struct uboot_flash_env *dev;
		int i;
		dev = &ctx->envdevs[0];
		for (i = 0; i < 2; i++, envdevs++, dev++) {
			if (!envdevs)
				break;
			memset(dev->devname, 0, sizeof(dev->devname));
			strncpy(dev->devname, envdevs->devname, sizeof(dev->devname) - 1);
			dev->offset = envdevs->offset;
			dev->envsize = envdevs->envsize;
			dev->sectorsize = envdevs->sectorsize;
			dev->envsectors = envdevs->envsectors;

			if (!ctx->size)
				ctx->size = dev->envsize;

			if (check_env_device(ctx, dev) < 0)
				return -EINVAL;

			if (i > 0) {
				ctx->redundant = true;
				if (!check_compatible_devices(ctx))
					return -EINVAL;
			}
		}
	}

	return 0;
}

int libuboot_initialize(struct uboot_ctx **out,
			struct uboot_env_device *envdevs) {
	struct uboot_ctx *ctx;
	int ret;

	*out = NULL;
	ctx = calloc (1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;

	ctx->valid = false;
	ret = libuboot_configure(ctx, envdevs);

	if (ret < 0) {
		free(ctx);
		return ret;
	}

	*out = ctx;
	return 0;
}

int libuboot_open(struct uboot_ctx *ctx) {
	if (!ctx)
		return -EINVAL;
	libuboot_lock(ctx);

	return libuboot_load(ctx);
}

void libuboot_close(struct uboot_ctx *ctx) {
	struct var_entry *e, *tmp;

	if (!ctx)
		return;
	ctx->valid = false;
	libuboot_unlock(ctx);

	LIST_FOREACH_SAFE(e, &ctx->varlist, next, tmp) {
		if (e->name)
			free(e->name);
		if (e->value)
			free(e->value);
		LIST_REMOVE(e, next);
		free(e);
	}
}

void libuboot_exit(struct uboot_ctx *ctx) {
	free(ctx);
}
