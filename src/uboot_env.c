/*
 * (C) Copyright 2019
 * Stefano Babic, <stefano.babic@swupdate.org>
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
#ifdef __FreeBSD__
#include <sys/disk.h>
#define BLKGETSIZE64 DIOCGMEDIASIZE
#else
#include <linux/fs.h>
#endif
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
#include <yaml.h>
#include <regex.h>

#include "uboot_private.h"

/* yaml_* functions return 1 on success and 0 on failure. */
enum yaml_status {
    SUCCESS = 0,
    FAILURE = 1
};

enum yaml_state {
	STATE_START,    /* start state */
	STATE_STREAM,   /* start/end stream */
	STATE_DOCUMENT, /* start/end document */
	STATE_SECTION,  /* top level */

	STATE_NAMESPACE,	/* Init Configuration Namespace */
	STATE_NAMESPACE_FIELDS,	/* namespace key list */
	STATE_NKEY,		/* Check key names */
	STATE_NSIZE,		/* Size key-value pair */
	STATE_NLOCKFILE,	/* Lockfile key-value pair */
	STATE_DEVVALUES,	/* Devices key names */
	STATE_WRITELIST,	/* List with vars that are accepted by write
				 * if list is missing, all vars are accepted
				 * var is in the format name:flags, see U-Boot
				 * documentation
				 */

	STATE_NPATH,
	STATE_NOFFSET,
	STATE_NSECTORSIZE,
	STATE_NUNLOCK,
	STATE_STOP      /* end state */
};

typedef enum yaml_parse_error_e {
	YAML_UNEXPECTED_STATE,
	YAML_UNEXPECTED_KEY,
	YAML_BAD_DEVICE,
	YAML_BAD_DEVNAME,
	YAML_BAD_VARLIST,
	YAML_DUPLICATE_VARLIST,
	YAML_OOM,
} yaml_parse_error_type_t;

struct parser_state {
	enum yaml_state state;		/* The current parse state */
	struct uboot_ctx *ctxsets;	/* Array of vars set ctx */
	struct uboot_ctx *ctx;		/* Current ctx in parsing */
	unsigned int nelem;		/* Number of elemets in ctxsets */
	unsigned int cdev;		/* current device in parsing */
	yaml_parse_error_type_t error;	/* error causing parser to stop */
	yaml_event_type_t event_type;	/* event type causing error */
};

#define FREE_ENTRY do { \
	free(entry->name); \
	free(entry->value); \
	free(entry); \
	} while(0)

/*
 * The default lockfile is the same as defined in U-Boot for
 * the fw_printenv utilities. Custom lockfile can be set via
 * configuration file.
 */
static const char *default_lockname = "/var/lock/fw_printenv.lock";
static struct uboot_version_info libinfo;

static int libuboot_lock(struct uboot_ctx *ctx)
{
	int lockfd = -1;
	lockfd = open(ctx->lockfile ?: default_lockname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
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
		return 'x';
	case TYPE_ATTR_BOOL:
		return 'b';
	case TYPE_ATTR_IP:
		return 'i';
	case TYPE_ATTR_MAC:
		return 'm';
	}

	return 's';
}

static void set_var_access_range(struct var_entry *entry, const char *prangeflags)
{
	int ret;

	/* parse regex range r"someregex" */
	if (!strncmp(prangeflags, "r\"", 2)) {
		char *rstart = strchr(prangeflags, '"');
		char *rend = strrchr(prangeflags, '"');

		if (rstart == rend)
			return;

		rstart++;
		if (rstart == rend)
			entry->range.u.re = strdup("");

		*rend++ = '\0';
		entry->range.u.re = strdup(rstart);
		entry->range.type = TYPE_ATTR_STRING;
	}
	/* parse bitmask range 0x[a-fA-F0-9]+ */
	else if (!strncmp(prangeflags, "0x", 2)) {
		entry->range.u.bitmask = (uint64_t) strtol(prangeflags, NULL, 16);
		entry->range.type = TYPE_ATTR_HEX;
	}

	/* parse integer range [0-9]+-[0-9]+ */
	else {
		char *lhs = strdup(prangeflags);
		char *rhs = strchr(lhs, '-');

		if (!rhs) {
			free(lhs);
			return;
		}

		*rhs++ = '\0';
		entry->range.u.int_range.min = strtol(lhs, NULL, 0);
		entry->range.u.int_range.max = strtol(rhs, NULL, 0);
		entry->range.type = TYPE_ATTR_DECIMAL;
		free(lhs);
	}

	entry->range.available = 1;
}

static void set_var_access_type(struct var_entry *entry, const char *pvarflags)
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
			case '@':
				set_var_access_range(entry, &pvarflags[i+1]);
				return;
			default: /* ignore it */
				break;
			}
		}
	}
}

static struct var_entry *create_var_entry(const char *name)
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

static const char *range_tostring(range_attribute *r)
{
	static char range_str[256];

	if (!r->available)
		return "";

	switch (r->type) {
	case TYPE_ATTR_STRING:
		snprintf(range_str, sizeof(range_str), "@r\"%s\"", r->u.re);
		break;
	case TYPE_ATTR_DECIMAL:
		snprintf(range_str, sizeof(range_str), "@%ld-%ld",
				 r->u.int_range.min, r->u.int_range.max);
		break;
	case TYPE_ATTR_HEX:
			snprintf(range_str, sizeof(range_str), "@0x%lx",
					 r->u.bitmask);
	}

	return range_str;
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

static void free_var_entry(struct var_entry *entry)
{
	if (entry) {
		LIST_REMOVE(entry, next);
		if (entry->range.available
			&& entry->range.type == TYPE_ATTR_STRING)
			free(entry->range.u.re);
		free(entry->name);
		free(entry->value);
		free(entry);
	}
}

static bool validate_int(bool hex, const char *value)
{
	const char *c;

	for (c = value; c != value + strlen(value); ++c) {
		if (hex && !isxdigit(*c))
			return false;

		if (!hex && !isdigit(*c))
			return false;
	}

	return true;
}

static bool libuboot_validate_flags(struct var_entry *entry, const char *value)
{
	bool ok_type = true, ok_access = true, okay_range = true;

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
		ok_type = validate_int(false, value);
		break;
	case TYPE_ATTR_HEX:
		ok_type = strlen(value) > 2 && (value[0] == '0') &&
			(value[1] == 'x' || value [1] == 'X');
		if (ok_type)
			ok_type = validate_int(true, value + 2);
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

	if (entry->range.available) {
		switch (entry->type) {
		case TYPE_ATTR_STRING:
			if (ok_type) {
				int ret;
				regex_t re;

				ret = regcomp(&re, entry->range.u.re, REG_EXTENDED);
				if (ret) {
					okay_range = false;
					break;
				}

				ret = regexec(&re, value, 0, NULL, 0);
				okay_range = !ret;
				regfree(&re);
			}
			break;
		case TYPE_ATTR_DECIMAL:
			if (ok_type) {
				int64_t ival;

				ival = strtol(value, NULL, 10);
				okay_range = ival >= entry->range.u.int_range.min
							 && ival <= entry->range.u.int_range.max;
			}
			break;
		case TYPE_ATTR_HEX:
			if (ok_type) {
				uint64_t ival;

				ival = strtol(value, NULL, 16);
				okay_range = !!(ival & entry->range.u.bitmask);
			}
			break;
		default:
			break;
		}
	}

	return ok_type & okay_range;
}


/*
 * This is used internally with additional parms
 */
static int __libuboot_set_env(struct uboot_ctx *ctx, const char *varname, const char *value, struct var_entry *validate)
{
	struct var_entry *entry, *elm, *lastentry;
	struct vars *envs = &ctx->varlist;

	/* U-Boot setenv treats '=' as an illegal character for variable names */
	if (strchr(varname, '='))
		return -EINVAL;

	/*
	 * Giving empty variable name will lead to having "=value" in U-Boot
	 * environment which will lead to problem during load of it and U-Boot
	 * will then load default environment.
	 */
	if (*varname == '\0')
		return -EINVAL;

	entry = __libuboot_get_env(envs, varname);
	if (entry) {
		bool valid = libuboot_validate_flags(entry, value);
		if (validate) {
			entry->access = validate->access;
			entry->type = validate->type;
			entry->range = validate->range;
			valid &= libuboot_validate_flags(entry, value);
		}
		if (!valid)
			return -EPERM;
		if (!value) {
			free_var_entry(entry);
		} else {
			free(entry->value);
			entry->value = strdup(value);
		}
		return 0;
	}

	if (!value)
		return 0;

	entry = create_var_entry(varname);
	if (!entry)
		return -ENOMEM;

	entry->value = strdup(value);
	if (!entry->value) {
		FREE_ENTRY;
		return -ENOMEM;
	}

	if (validate) {
		entry->access = validate->access;
		entry->type = validate->type;
		entry->range = validate->range;
		if (!libuboot_validate_flags(entry, value)) {
			FREE_ENTRY;
			return -EPERM;
		}
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

static int normalize_device_path(char *path, struct uboot_flash_env *dev)
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

static int check_env_device(struct uboot_flash_env *dev)
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
	if (ret < 0)
		return -EBADF;
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
		ret = libubootenv_mtdread(dev, data);
		break;
	case DEVICE_UBI:
		ret = libubootenv_ubiread(dev, data);
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
	if (strncmp("/dev/block/", dev->devname, 11) == 0) {
		devfile = dev->devname + 11;
	} else if (strncmp("/dev/", dev->devname, 5) == 0) {
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

	fsync(dev->fd);

	fileprotect(dev, true);  // no error handling, keep ret from write

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
		ret = libubootenv_mtdwrite(dev, data);
		break;
	case DEVICE_UBI:
		ret = libubootenv_ubiwrite(dev, data);
		break;
	default:
		ret = -1;
		break;
	};

	close(dev->fd);

	return ret;
}

const struct uboot_version_info *libuboot_version_info(void)
{
	int i;
	char *endptr = NULL;
	char *start = VERSION;
	long val = 0;
	libinfo.version = VERSION;

	for (i = 0; i < 3; i++) {
		if (!*start)
			break;
		val = val + (strtol(start, &endptr, 10) << (8 * (2 - i)));
		if (start == endptr)
			break;
		start = endptr + 1;
	}

	libinfo.version_num = val;

	return &libinfo;
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

	data = (char *)(image + offsetdata);

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
			if (entry->type || entry->access || entry->range.available) {
				buf += snprintf(buf, size, "%s%s:%c%c%s",
						first ? "" : ",",
						entry->name,
						attr_tostring(entry->type),
						access_tostring(entry->access),
						range_tostring(&entry->range));
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
			ret = libubootenv_set_obsolete_flag(&ctx->envdevs[ctx->current]);
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
	char *line, *next;
	uint8_t offsetdata = offsetof(struct uboot_env_noredund, data);
	uint8_t offsetcrc = offsetof(struct uboot_env_noredund, crc);
	uint8_t offsetflags = offsetof(struct uboot_env_redund, flags);
	char *data;
	struct var_entry *entry;

	ctx->valid = false;

	bufsize = ctx->size;
	if (ctx->redundant) {
		copies++;
		bufsize += ctx->size;
		offsetdata = offsetof(struct uboot_env_redund, data);
		offsetcrc = offsetof(struct uboot_env_redund, crc);
	}
	usable_envsize = ctx->size - offsetdata;
	buf[0] = malloc(bufsize);
	if (!buf[0])
		return -ENOMEM;

	if (copies > 1)
		buf[1] = buf[0] + ctx->envdevs[0].envsize;

	for (i = 0; i < copies; i++) {
		data = (char *)(buf[i] + offsetdata);
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

	data = (char *)(buf[ctx->current] + offsetdata);

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
				__libuboot_set_env(ctx, line, value, NULL);
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
		unsigned int flagslen = strlen(flagsvar);
		pvar = flagsvar;

		while (*pvar && (pvar - flagsvar) < flagslen) {
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
			set_var_access_type(entry, pval);
			pvar = pnext;
		}
	}
	free(flagsvar);

	free(buf[0]);

	return ctx->valid ? 0 : -ENODATA;
}

static int consume_event(struct parser_state *s, yaml_event_t *event)
{
	char *value;
	struct uboot_flash_env *dev;
	struct uboot_ctx *newctx;
	int cdev;

	switch (s->state) {
	case STATE_START:
		switch (event->type) {
		case YAML_STREAM_START_EVENT:
			s->state = STATE_STREAM;
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_STREAM:
		switch (event->type) {
		case YAML_DOCUMENT_START_EVENT:
			s->state = STATE_DOCUMENT;
			break;
		case YAML_STREAM_END_EVENT:
			s->state = STATE_STOP;
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_DOCUMENT:
		switch (event->type) {
		case YAML_MAPPING_START_EVENT:
			s->state = STATE_SECTION;
			break;
		case YAML_DOCUMENT_END_EVENT:
			s->state = STATE_STREAM;
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_SECTION:
		switch (event->type) {
		case YAML_SCALAR_EVENT:
			value = (char *)event->data.scalar.value;
			newctx = calloc (s->nelem + 1, sizeof(*newctx));
			for (int i = 0; i < s->nelem; i++) {
				newctx[i] = s->ctxsets[i];
			}
			if (s->ctxsets) free(s->ctxsets);
			s->ctxsets = newctx;
			s->ctx = &newctx[s->nelem];
			s->ctx->name = strdup(value);
			s->nelem++;
			s->state = STATE_NAMESPACE;
			break;
		case YAML_MAPPING_END_EVENT:
			s->state = STATE_DOCUMENT;
			break;
		case YAML_DOCUMENT_END_EVENT:
			s->state = STATE_STREAM;
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_NAMESPACE:
		switch (event->type) {
		case YAML_MAPPING_START_EVENT:
			s->state = STATE_NAMESPACE_FIELDS;
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_NAMESPACE_FIELDS:
		switch (event->type) {
		case YAML_SCALAR_EVENT:
			value = (char *)event->data.scalar.value;
			if (!strcmp(value, "size")) {
				s->state = STATE_NSIZE;
			} else if (!strcmp(value, "lockfile")) {
				s->state = STATE_NLOCKFILE;
			} else if (!strcmp(value, "devices")) {
				s->state = STATE_DEVVALUES;
				s->cdev = 0;
			} else if (!strcmp(value, "writelist")) {
				s->state = STATE_WRITELIST;
			} else {
				s->error = YAML_UNEXPECTED_KEY;
				s->event_type = event->type;
				return FAILURE;
			}
			break;
		case YAML_MAPPING_END_EVENT:
			s->state = STATE_SECTION;
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_NSIZE:
		switch (event->type) {
		case YAML_SCALAR_EVENT:
			value = (char *)event->data.scalar.value;
			errno = 0;
			s->ctx->size = strtoull(value, NULL, 0);
			s->state = STATE_NAMESPACE_FIELDS;
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_NLOCKFILE:
		switch (event->type) {
		case YAML_SCALAR_EVENT:
			value = (char *)event->data.scalar.value;
			s->ctx->lockfile = strdup(value);
			s->state = STATE_NAMESPACE_FIELDS;
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_DEVVALUES:
		switch (event->type) {
		case YAML_MAPPING_START_EVENT:
		case YAML_SEQUENCE_START_EVENT:
			break;
		case YAML_MAPPING_END_EVENT:
			dev = &s->ctx->envdevs[s->cdev];
			if (check_env_device(dev) < 0) {
				s->error = YAML_BAD_DEVICE;
				s->event_type = event->type;
				return FAILURE;
			}
			s->cdev++;
			break;
		case YAML_SEQUENCE_END_EVENT:
			s->state = STATE_NAMESPACE_FIELDS;
			break;
		case YAML_SCALAR_EVENT:
			value = (char *)event->data.scalar.value;
			if (s->cdev)
				s->ctx->redundant = true;
			if (!strcmp(value, "path")) {
				s->state = STATE_NPATH;
			} else if (!strcmp(value, "offset")) {
				s->state = STATE_NOFFSET;
			} else if (!strcmp(value, "sectorsize")) {
				s->state = STATE_NSECTORSIZE;
				} else if (!strcmp(value, "disablelock")) {
				s->state = STATE_NUNLOCK;
			} else {
				s->error = YAML_UNEXPECTED_KEY;
				s->event_type = event->type;
				return FAILURE;
			}
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_WRITELIST:
		switch (event->type) {

		char *varflag, *name;
		struct var_entry *entry;

		case YAML_MAPPING_START_EVENT:
		case YAML_SEQUENCE_START_EVENT:
			break;
		case YAML_MAPPING_END_EVENT:
			break;
		case YAML_SEQUENCE_END_EVENT:
			s->state = STATE_NAMESPACE_FIELDS;
			break;
		case YAML_SCALAR_EVENT:
			value = (char *)event->data.scalar.value;

			/*
			 * Format is name:flags, split it into two values
			 */
			varflag = strchr(value, ':');
			if (!varflag || varflag > value + (strlen(value) - 1)) {
				s->error = YAML_BAD_VARLIST;
				s->event_type = event->type;
				return FAILURE;
			}
			*varflag++ = '\0';

			/*
			 * Check there is not yet an entry for this variable
			 */
			LIST_FOREACH(entry, &s->ctx->writevarlist, next) {
				if (strcmp(entry->name, value) == 0) {
					s->error = YAML_DUPLICATE_VARLIST;
					s->event_type = event->type;
					return FAILURE;
				}
			}

			/*
			 * Insert variable with its configuration into the list
			 * of modifiable vars
			 */
			entry = create_var_entry(value);
			if (!entry) {
				s->error = YAML_OOM;
				s->event_type = event->type;
				return FAILURE;
			}
			set_var_access_type(entry, varflag);
			LIST_INSERT_HEAD(&s->ctx->writevarlist, entry, next);

#if !defined(NDEBUG)
			fprintf(stdout, "Writelist: %s flags %s\n", value, varflag);
#endif
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_NPATH:
		switch (event->type) {
		case YAML_SCALAR_EVENT:
			dev = &s->ctx->envdevs[s->cdev];
			value = (char *)event->data.scalar.value;
			if (normalize_device_path(value, dev) < 0) {
				s->error = YAML_BAD_DEVNAME;
				s->event_type = event->type;
				return FAILURE;
			}
			dev->envsize = s->ctx->size;
			s->state = STATE_DEVVALUES;
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_NOFFSET:
		switch (event->type) {
		case YAML_SCALAR_EVENT:
			dev = &s->ctx->envdevs[s->cdev];
			value = (char *)event->data.scalar.value;
			dev->offset = strtoull(value, NULL, 0);
			s->state = STATE_DEVVALUES;
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_NSECTORSIZE:
		switch (event->type) {
		case YAML_SCALAR_EVENT:
			dev = &s->ctx->envdevs[s->cdev];
			value = (char *)event->data.scalar.value;
			dev->sectorsize = strtoull(value, NULL, 0);
			s->state = STATE_DEVVALUES;
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

	case STATE_NUNLOCK:
		switch (event->type) {
		case YAML_SCALAR_EVENT:
			dev = &s->ctx->envdevs[s->cdev];
			value = (char *)event->data.scalar.value;
			if (!strcmp(value, "yes"))
				dev->disable_mtd_lock = 1;
			s->state = STATE_DEVVALUES;
			break;
		default:
			s->error = YAML_UNEXPECTED_STATE;
			s->event_type = event->type;
			return FAILURE;
		}
		break;

    case STATE_STOP:
        break;
    }
    return SUCCESS;
}

int parse_yaml_config(struct uboot_ctx **ctxlist, FILE *fp)
{
	yaml_parser_t parser;
	yaml_event_t  event;
	enum yaml_status status;
	struct parser_state state;
	struct uboot_ctx *ctx;

	if (!yaml_parser_initialize(&parser))
		return -ENOMEM;

	 /* Set input file */
	yaml_parser_set_input_file(&parser, fp);
	memset(&state, 0, sizeof(state));
	state.state = STATE_START;
	do {
		if (!yaml_parser_parse(&parser, &event)) {
			status = FAILURE;
			goto cleanup;
		}
		status = consume_event(&state, &event);
		yaml_event_delete(&event);
		if (status == FAILURE) {
			goto cleanup;
		}
	} while (state.state != STATE_STOP);

	state.ctxsets[0].nelem = state.nelem;

	for (int i = 0; i < state.nelem; i++) {
		ctx = &state.ctxsets[i];
		ctx->ctxlist = &state.ctxsets[0];
		if (ctx->redundant && !check_compatible_devices(ctx)) {
			status = FAILURE;
			break;
		}
	}


cleanup:
	yaml_parser_delete(&parser);
	if (status == FAILURE) {
		if (state.ctxsets) free (state.ctxsets);
		state.ctxsets = NULL;
	}
	*ctxlist = state.ctxsets;
	return status;
}

#define LINE_LENGTH 2048
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

int libuboot_read_config_ext(struct uboot_ctx **ctxlist, const char *config)
{
	FILE *fp;
	char *line = NULL;
	size_t bufsize = 0;
	int ret = 0;
	int ndev = 0;
	struct uboot_flash_env *dev;
	char *tmp;
	int retval = 0;
	struct uboot_ctx *ctx;

	if (!config)
		return -EINVAL;

	fp = fopen(config, "r");
	if (!fp)
		return -EBADF;

	if (!*ctxlist) {
		ret = parse_yaml_config(ctxlist, fp);
		if (!ret) {
			fclose(fp);
			return 0;
		}
		ret = libuboot_initialize(ctxlist, NULL);
		if (ret) {
			fclose(fp);
			return ret;
		}
	}
	ctx = *ctxlist;

	dev = ctx->envdevs;
	ctx->size = 0;
	rewind(fp);

	int len;
	while ((len = getline(&line, &bufsize, fp)) != -1) {
		/* skip comments */
		if (line[0] == '#')
			continue;

#if defined(__FreeBSD__)
		/*
		 * POSIX.1-2008 introduced the dynamic allocation conversion
		 * specifier %m which is not implemented on FreeBSD.
		 */
		tmp = calloc(1, len + 1);
		ret = sscanf(line, "%s %lli %zx %zx %lx %d",
				tmp,
#else
		(void)len;
		ret = sscanf(line, "%ms %lli %zx %zx %lx %d",
				&tmp,
#endif
				&dev->offset,
				&dev->envsize,
				&dev->sectorsize,
				&dev->envsectors,
				&dev->disable_mtd_lock);

		/*
		 * At least name offset and size should be set
		 */
		if (ret < 3) {
			free(tmp);
			continue;
		}

		/*
		 * If size is set but zero, entry is wrong
		 */
		if (!dev->envsize) {
			free(tmp);
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

		if (check_env_device(dev) < 0) {
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

int libuboot_read_config(struct uboot_ctx *ctx, const char *config)
{
	return libuboot_read_config_ext(&ctx, config);
}

int libuboot_set_env(struct uboot_ctx *ctx, const char *varname, const char *value)
{
	struct var_entry *entry, *entryvarlist = NULL;
	if (strchr(varname, '='))
		return -EINVAL;

	/*
	 * If there is a list of chosen rw vars,
	 * first check that variable belongs to the list
	 */
	if (!LIST_EMPTY(&ctx->writevarlist)) {
		entryvarlist = __libuboot_get_env(&ctx->writevarlist, varname);
		if (!entryvarlist)
			return -EPERM;
	}
	return __libuboot_set_env(ctx, varname, value, entryvarlist);
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

			if (check_env_device(dev) < 0)
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

struct uboot_ctx *libuboot_get_namespace(struct uboot_ctx *ctxlist, const char *name)
{
	struct uboot_ctx *ctx;
	int i;

	if (!ctxlist)
		return NULL;

	/*
	 * Be sure to get the whole list, pointer is stored into each
	 * CTX pointer in the list
	 */
	if (ctxlist->ctxlist)
		ctxlist = ctxlist->ctxlist;
	for (i = 0, ctx = ctxlist; i < ctxlist->nelem; i++, ctx++) {
		if (!strcmp(ctx->name, name))
			return ctx;
	}

	return NULL;
}

#define MAX_NAMESPACE_LENGTH 64
const char *libuboot_namespace_from_dt(void)
{
	FILE *fp;
	size_t dt_ret;
	char *dt_namespace;

	fp = fopen("/proc/device-tree/chosen/u-boot,env-config", "r");
	if (!fp)
		return NULL;

	dt_namespace = malloc(MAX_NAMESPACE_LENGTH);
	if (!dt_namespace) {
		fclose(fp);
		return NULL;
	}
	dt_ret = fread(dt_namespace, 1, MAX_NAMESPACE_LENGTH - 1, fp);
	fclose(fp);
	if (!dt_ret) {
		free(dt_namespace);
		return NULL;
	}
	dt_namespace[dt_ret] = 0;
	return dt_namespace;
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

void libuboot_exit(struct uboot_ctx *ctx)
{
	struct uboot_ctx *c;
	int i;

	if (!ctx)
		return;

	/* passed context might not be list start */
	if (ctx->ctxlist) {
		ctx = ctx->ctxlist;
	} else {
		/* but in case we don't have a list at all, fixup nelem so that
		 * we enter the loop to free the name and lockfile correctly */
		ctx->nelem = 1;
	}

	for (i = 0, c = ctx; i < ctx->nelem; i++, c++) {
		free(c->name);
		free(c->lockfile);
	}

	free(ctx);
}
