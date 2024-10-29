/*
 * (C) Copyright 2024
 * Stefano Babic, <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */


#include "uboot_private.h"

struct var_entry *create_var_entry(const char *name);
void set_var_access_type(struct var_entry *entry, const char *pvarflags);
int normalize_device_path(char *path, struct uboot_flash_env *dev);
int check_env_device(struct uboot_flash_env *dev);
bool check_compatible_devices(struct uboot_ctx *ctx);
