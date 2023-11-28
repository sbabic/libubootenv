/*
 * (C) Copyright 2019
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/queue.h>
#include <sys/types.h>
#include "libuboot.h"

#define UBI_MAX_VOLUME			128

#define DEVICE_MTD_NAME 		"/dev/mtd"
#define DEVICE_UBI_NAME 		"/dev/ubi"
#define DEVICE_UBI_CTRL 		"/dev/ubi_ctrl"
#define SYS_UBI				"/sys/class/ubi"
#define SYS_UBI_MTD_NUM			"/sys/class/ubi/ubi%d/mtd_num"
#define SYS_UBI_VOLUME_COUNT		"/sys/class/ubi/ubi%d/volumes_count"
#define SYS_UBI_VOLUME_NAME		"/sys/class/ubi/ubi%d/ubi%d_%d/name"

#if !defined(__FreeBSD__)
#include <mtd/mtd-user.h>
#include <mtd/ubi-user.h>
#define MTDLOCK(dev, psector)	\
	if (!dev->disable_mtd_lock) ioctl (dev->fd, MEMLOCK, psector)
#define MTDUNLOCK(dev, psector) \
	if (!dev->disable_mtd_lock) ioctl (dev->fd, MEMUNLOCK, psector)

#define	LIST_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = LIST_FIRST((head));				\
	    (var) != NULL &&						\
	    ((tvar) = LIST_NEXT((var), field), 1);			\
	    (var) = (tvar))
#else
#define MTD_ABSENT		0
#define MTD_RAM			1
#define MTD_ROM			2
#define MTD_NORFLASH		3
#define MTD_NANDFLASH		4	/* SLC NAND */
#define MTD_DATAFLASH		6
#define MTD_UBIVOLUME		7
#define MTD_MLCNANDFLASH	8	/* MLC NAND (including TLC) */
#define MTDLOCK
#define MTDUNLOCK

#define ENODATA ENODEV

struct mtd_info_user {
	u8 type;
	u32 flags;
	u32 size;	/* Total size of the MTD */
	u32 erasesize;
	u32 writesize;
	u32 oobsize;	/* Amount of OOB data per block (e.g. 16) */
	u64 padding;	/* Old obsolete field; do not use */
};
#endif

typedef enum {
	TYPE_ATTR_STRING,	/* default */
	TYPE_ATTR_DECIMAL,
	TYPE_ATTR_HEX,
	TYPE_ATTR_BOOL,
	TYPE_ATTR_IP,
	TYPE_ATTR_MAC
} type_attribute;

typedef enum {
	ACCESS_ATTR_ANY,	/* default */
	ACCESS_ATTR_READ_ONLY,
	ACCESS_ATTR_WRITE_ONCE,
	ACCESS_ATTR_CHANGE_DEFAULT,
} access_attribute;

enum flags_type {
	FLAGS_NONE,
	FLAGS_BOOLEAN,
	FLAGS_INCREMENTAL
};

enum device_type {
	DEVICE_NONE,
	DEVICE_FILE,
	DEVICE_MTD,
	DEVICE_UBI,
};

/**
 * U-Boot environment should always be redundant, but
 * for compatibility reasons a single copy must
 * be also supported. Structure is different because
 * there is no flags in the single copy
 */
struct uboot_env_noredund {
	/** computed crc32 value */
	uint32_t crc;
	/** placeholder to point to the env in flash */
	char data[];
};

struct uboot_env_redund {
	/** computed crc32 value */
	uint32_t crc;
	/** flags, see flags_type */
	unsigned char flags;
	/** placeholder to point to the env in flash */
	char data[];
};

struct uboot_flash_env {
	/** path to device or file where env is stored */
	char 			devname[DEVNAME_MAX_LENGTH];
	/** Start offset inside device path */
	long long int 		offset;
	/** size of environment */
	size_t 			envsize;
	/** Size of sector (for MTD) */
	size_t 			sectorsize;
	/** Number of sectors for each environment */
	long unsigned int 	envsectors;
	/** MTD structure as returned by ioctl() call */
	struct mtd_info_user	mtdinfo;
	/** Computed CRC on the stored environment */
	uint32_t		crc;
	/** file descriptor used to access the device */
	int  			fd;
	/** flags (see flags_type) are one byte in the stored environment */
	unsigned char		flags;
	/** flags according to device type */
	enum flags_type		flagstype;
	/** type of device (mtd, ubi, file, ....) */
	enum device_type	device_type;
	/** Disable lock mechanism (required by some flashes */
	int disable_mtd_lock;
};

/** Internal structure for an environment variable
 */
struct var_entry {
	/** Variable's name */
	char *name;
	/** Variable's value */
	char *value;
	/** Type of the variable, see access_attribute */
	type_attribute type;
	/** Permissions for the variable */
	access_attribute access;
	/** Pointer to next element in the list */
	LIST_ENTRY(var_entry) next;
};

LIST_HEAD(vars, var_entry);

/** libubootenv context
 */
struct uboot_ctx {
	/** true if the environment is redundant */
	bool redundant;
	/** set to valid after a successful load */
	bool valid;
	/** size of the environment */
	size_t size;
	/** devices where environment is stored */
	struct uboot_flash_env envdevs[2];
	/** Set which device contains the current(last valid) environment */
	int current;
	/** semaphore on the environment */
	int lock;
	/** pointer to the internal db */
	struct vars varlist;
	/** name of the set */
	char *name;
	/** lockfile */
	char *lockfile;
	/** Number of namespaces */
	int nelem;
	/** private pointer to list */
	struct uboot_ctx *ctxlist;
};

#if defined(__FreeBSD__)
#define libubootenv_mtdgetinfo(fd,dev) (-1)
#define libubootenv_mtdread(dev,data) (-1)
#define libubootenv_mtdwrite(dev,data) (-1)
#define libubootenv_ubiread(dev,data) (-1)
#define libubootenv_ubiwrite(dev,data) (-1)
#define libubootenv_ubi_update_name(dev) (-1)
#define libubootenv_set_obsolete_flag(dev) (-1)
#else
int libubootenv_mtdgetinfo(int fd, struct uboot_flash_env *dev);
int libubootenv_mtdread(struct uboot_flash_env *dev, void *data);
int libubootenv_mtdwrite(struct uboot_flash_env *dev, void *data);
int libubootenv_ubi_update_name(struct uboot_flash_env *dev);
int libubootenv_ubiread(struct uboot_flash_env *dev, void *data);
int libubootenv_ubiwrite(struct uboot_flash_env *dev, void *data);
int libubootenv_set_obsolete_flag(struct uboot_flash_env *dev);
#endif


