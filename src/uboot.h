/*
 * (C) Copyright 2019
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */


#pragma once

int fw_env_set(int argc, char *argv[], struct env_opts *opts);
int fw_parse_script(char *fname, struct env_opts *opts);
int fw_env_open(struct env_opts *opts);
char *fw_getenv(char *name);
int fw_env_write(char *name, char *value);
int fw_env_flush(struct env_opts *opts);
int fw_env_close(struct env_opts *opts);
char *fw_env_version(void);
