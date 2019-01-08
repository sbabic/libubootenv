/*
 * (C) Copyright 2019
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */


#pragma once

struct uboot_ctx;

#define DEVNAME_MAX_LENGTH	40

/** Configuration passed in initialization 
 *
 */
struct uboot_env_device {
	/** path to device or file where env is stored */
	char 		*devname;	
	/** Start offset inside device path */
	long long int 	offset;
	/** size of environment */
	size_t 		envsize;
	/** Size of sector (for MTD) */
	size_t 		sectorsize;
 	/** Number of sectors for each environment */
	unsigned long 	envsectors;
};

/** @brief Read U-Boot environment configuration from a file
 *
 * @param[in] ctx libuboot context
 * @param[in] config path to the configuration file
 * @return 0 in case of success, else negative value
 */
int libuboot_read_config(struct uboot_ctx *ctx, const char *config);

/** @brief Read U-Boot environment configuration from structure
 *
 * @param[in] ctx libuboot context
 * @param[in] envdevs array of two uboot_env_device 
 * @return 0 in case of success, else negative value
 */
int libuboot_configure(struct uboot_ctx *ctx,
			struct uboot_env_device *envdevs);

/** @brief Import environment from file
 *
 * Read and parses variable from a file in the same way as
 * U-Boot does with "env import -t"
 * The file has the format:
 * 	<variable name>=<value>
 * Comments starting with "#" are allowed.
 *
 * @param[in] ctx libuboot context
 * @param[in] filename path to the file to be imported
 * @return 0 in case of success, else negative value
 */
int libuboot_load_file(struct uboot_ctx *ctx, const char *filename);

/** @brief Flush environment to the storage
 *
 * Write the environment back to the storage and handle
 * redundant devices.
 *
 * @param[in] ctx libuboot context
 * @return 0 in case of success, else negative value
 */
int libuboot_env_store(struct uboot_ctx *ctx);

/** @brief Initialize the library
 *
 * Initialize the library and get the context structure
 *
 * @param[out] *ctx struct uboot_ctx **out allocated structure 
 * @param[in] struct uboot_env_device *envdevs
 * @return 0 in case of success, else negative value
 */
int libuboot_initialize(struct uboot_ctx **out,
			struct uboot_env_device *envdevs);

/** @brief Release all resources and exit the library
 *
 * @param[in] ctx libuboot context
 */
void libuboot_exit(struct uboot_ctx *ctx);

/** @brief Load an environment
 *
 * @param[in] ctx libuboot context
 * @return 0 in case of success
 */
int libuboot_open(struct uboot_ctx *ctx);

/** @brief Release an environment
 *
 * Release allocated recource for the environment, but
 * maintain the context. This allows to call
 * libuboot_open() again.
 *
 * @param[in] ctx libuboot context
 * @return 0 in case of success
 */
void libuboot_close(struct uboot_ctx *ctx);

/** @brief Set a variable
 *
 * Set a variable. It creates a new variable if not present in
 * the database, changes it or drops if value is NULL 
 *
 * @param[in] ctx libuboot context
 * @param[in] variable name
 * @param[in] value. In case this is NULL, the variable is dropped
 * @return 0 in case of success
 */
int libuboot_set_env(struct uboot_ctx *ctx, const char *varname, const char *value);

/** @brief Get a variable
 *
 * Return value of a variable as string or NULL if
 * variable is not present in the database.
 * The returned string must be freed by the caller when not
 * used anymore.
 *
 * @param[in] ctx libuboot context
 * @param[in] variable name
 * @return value in case of success, NULL in case of error
 */
char *libuboot_get_env(struct uboot_ctx *ctx, const char *varname);

/** @brief Iterator
 *
 * Return a pointer to an entry in the database
 * Used to iterate all variables in the database.
 *
 * @param[in] ctx libuboot context
 * @param[in] next
 * @return pointer to next entry or NULL
 */
void *libuboot_iterator(struct uboot_ctx *ctx, void *next);

/** @brief Accessor to get variable name from DB entry
 *
 * @param[in] entry element in the database
 * @return pointer to name or NULL
 */
const char *libuboot_getname(void *entry);

/** @brief Accessor to get variable value from DB entry
 *
 * @param[in] entry element in the database
 * @return pointer to name or NULL
 */
const char *libuboot_getvalue(void *entry);
