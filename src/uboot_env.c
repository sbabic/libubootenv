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

static int check_env_device(struct uboot_ctx *ctx, struct uboot_flash_env *dev)
{
	int fd, ret;
	struct stat st;

	ret = stat(dev->devname, &st);
	if (ret < 0)
		return -EBADF;
	fd = open(dev->devname, O_RDONLY);
	if (fd < 0)
		return -EBADF;

	if (S_ISCHR(st.st_mode)) {
		ret = ioctl(fd, MEMGETINFO, &dev->mtdinfo);
		if (ret < 0 || (dev->mtdinfo.type != MTD_NORFLASH &&
		    dev->mtdinfo.type != MTD_NANDFLASH &&
		    dev->mtdinfo.type != MTD_DATAFLASH &&
		    dev->mtdinfo.type != MTD_UBIVOLUME)) {
			close(fd);
			return -EBADF;
		}
	} else {
		dev->mtdinfo.type = MTD_ABSENT;
	}

	switch (dev->mtdinfo.type ) {
	case MTD_NORFLASH:
	case MTD_DATAFLASH: 
		dev->flagstype = FLAGS_BOOLEAN;
		break;
	case MTD_NANDFLASH: 
	case MTD_UBIVOLUME: 
	case MTD_ABSENT: 
		dev->flagstype = FLAGS_INCREMENTAL;
		break;
	}

	close(fd);

	return 0;
}

static bool check_compatible_devices(struct uboot_ctx *ctx)
{
	struct uboot_flash_env *dev;

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

static int devread(struct uboot_ctx *ctx, unsigned int copy, void *data)
{
	int ret;
	struct uboot_flash_env *dev;
	size_t count;
	size_t blocksize;
	loff_t start;
	void *buf;
	int sectors, skip;

	if (copy > 1)
		return -EINVAL;

	dev = &ctx->envdevs[copy];

   	dev->fd = open(dev->devname, O_RDONLY);
   	if (dev->fd < 0)
    		return -EBADF;

	switch (dev->mtdinfo.type) {
	case MTD_ABSENT:
	case MTD_NORFLASH:
		if (dev->offset)
			lseek(dev->fd, dev->offset, SEEK_SET);
		ret = read(dev->fd, data, dev->envsize);
		break;
	case MTD_NANDFLASH:
		if (dev->offset)
			lseek(dev->fd, dev->offset, SEEK_SET);

		count = dev->envsize;
		start = dev->offset;
		blocksize = dev->envsize;
		sectors = dev->envsectors ? dev->envsectors : 1;
		ret = 0;

		while (count > 0) {
			skip = is_nand_badblock(dev, start);
			if (skip < 0) {
				close(dev->fd);
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
				close(dev->fd);
				return -EIO;
			}
			if (read(dev->fd, data, blocksize) != blocksize) {
				close(dev->fd);
				return -EIO;
			}
			start += dev->sectorsize;
			data += blocksize;
			count -= blocksize;
		}
		ret = count;
		break;
	}

	close(dev->fd);
	return ret;
}

static int devwrite(struct uboot_ctx *ctx, unsigned int copy, void *data)
{
	int ret;
	struct uboot_flash_env *dev;
	struct erase_info_user erase;
	size_t count;
	size_t blocksize;
	loff_t start;
	void *buf;
	int sectors, skip;

	if (copy > 1)
		return -EINVAL;

	dev = &ctx->envdevs[copy];

   	dev->fd = open(dev->devname, O_RDWR);
   	if (dev->fd < 0)
    		return -EBADF;

	switch (dev->mtdinfo.type) {
	case MTD_ABSENT:
		if (dev->offset)
			lseek(dev->fd, dev->offset, SEEK_SET);
		ret = write(dev->fd, data, dev->envsize);
		break;
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
		}
		break;
	}

devwrite_out:
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
			}
		}
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
	size_t bufsize;
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
		dev->crc = crc32(0, (uint8_t *)data, ctx->size - offsetdata);
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
			char *name, *value, *tmp;

			/*
			 * Search the end of the string pointed by line
			 */
			for (next = line; *next; ++next) {
				if ((next - (char *)data) > ctx->size) {
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
	int fd;
	FILE *fp;
	char *buf;
	char *name, *value;

	if (!filename)
		return -EBADF;

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

		ret = sscanf(line, "%ms %lli %lx %lx %lx",
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

		if (!ctx->size)
			ctx->size = dev->envsize;

		if (tmp) {
			strncpy(dev->devname, tmp, sizeof(dev->devname));
			free(tmp);
		}

		if (check_env_device(ctx, dev) < 0)
			return -EINVAL;

		ndev++;
		dev++; 

		if (ndev >= 2) {
			ctx->redundant = true;
			if (check_compatible_devices(ctx) < 0)
				return -EINVAL;
			break;
		}
	}

	fclose(fp);
	free(line);

	return 0;
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
			strncpy(dev->devname, envdevs->devname, sizeof(dev->devname));
			dev->envsize = envdevs->envsize;
			dev->sectorsize = envdevs->sectorsize;
			dev->envsectors = envdevs->envsectors;

			if (check_env_device(ctx, dev) < 0)
				return -EINVAL;

			if (i > 0) {
				ctx->redundant = true;
				if (check_compatible_devices(ctx) < 0)
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
	if (!ctx || !ctx->envdevs)
		return -EINVAL;
	libuboot_lock(ctx);

	return libuboot_load(ctx);
}

void libuboot_close(struct uboot_ctx *ctx) {
	struct var_entry *e, *tmp;

	if (!ctx || !ctx->envdevs)
		return;
	ctx->valid = false;
	libuboot_unlock(ctx);

	LIST_FOREACH_SAFE(e, &ctx->varlist, next, tmp) {
		if (e->name)
			free(e->name);
		if (e->value)
			free(e->value);
		free(e);
	}
}

void libuboot_exit(struct uboot_ctx *ctx) {
	free(ctx);
}
