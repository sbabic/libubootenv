/*
 * (C) Copyright 2024
 * Stefano Babic, <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#ifdef __FreeBSD__
#include <sys/disk.h>
#define BLKGETSIZE64 DIOCGMEDIASIZE
#else
#include <linux/fs.h>
#endif

#include "uboot_private.h"
#include "common.h"

static enum device_type get_device_type(char *device)
{
	enum device_type type = DEVICE_NONE;

	if (!strncmp(device, DEVICE_MTD_NAME, strlen(DEVICE_MTD_NAME)))
		if (strchr(device, DEVNAME_SEPARATOR)) {
			type = DEVICE_UBI;
		} else {
			type = DEVICE_MTD;
		}
	else if (!strncmp(device, DEVICE_UBI_NAME, strlen(DEVICE_UBI_NAME)))
		type = DEVICE_UBI;
	else if (strlen(device) > 0)
		type = DEVICE_FILE;

	return type;
}

int normalize_device_path(char *path, struct uboot_flash_env *dev)
{
	char *sep = NULL, *normalized = NULL;
	size_t normalized_len = 0, volume_len = 0, output_len = 0;

	/*
	 * if volume name is present, split into device path and volume
	 * since only the device path needs normalized
	 */
	sep = strchr(path, DEVNAME_SEPARATOR);
	if (sep)
	{
		volume_len = strlen(sep);
		*sep = '\0';
	}

	if ((normalized = realpath(path, NULL)) == NULL)
	{
		/* device file didn't exist */
		normalized = malloc(PATH_MAX);
		strncpy(normalized, path, PATH_MAX - 1);
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

void set_var_access_type(struct var_entry *entry, const char *pvarflags)
{
	if (entry) {
		for (int i = 0; i < strlen(pvarflags); i++) {
			switch (pvarflags[i]) {
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
}

struct var_entry *create_var_entry(const char *name)
{
	struct var_entry *entry;

	entry = (struct var_entry *)calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	entry->name = strdup(name);
	if (!entry->name) {
		free(entry);
		return NULL;
	}

	return entry;
}

bool check_compatible_devices(struct uboot_ctx *ctx)
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

int check_env_device(struct uboot_flash_env *dev)
{
	int fd, ret;
	struct stat st;

	dev->device_type = get_device_type(dev->devname);
	if (dev->device_type == DEVICE_NONE)
		return -EBADF;

	if (dev->device_type == DEVICE_UBI) {
		ret = libubootenv_ubi_update_name(dev);
		if (ret)
			return ret;
	}

	ret = stat(dev->devname, &st);
	if (ret < 0) {
		/* device is not readable, no further checks possible */
		return 0;
	}
	fd = open(dev->devname, O_RDONLY);
	if (fd < 0)
		return -EBADF;

	if (S_ISCHR(st.st_mode)) {
		if (dev->device_type == DEVICE_MTD) {
			ret = libubootenv_mtdgetinfo(fd, dev);
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
