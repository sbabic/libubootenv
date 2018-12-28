/*
 * (C) Copyright 2019
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#pragma once

/*
 * U-Boot environment should always be redundant, but
 * for compatibility reasons a single copy must
 * be also supported. Structure is different because 
 * there is no flags in the single copy
 */

struct uboot_env_noredund {
	uint32_t crc;
	char data[];
};

struct uboot_env {
	uint32_t crc;
	unsigned char flags;	/* active or obsolete */
	char data[];
};

