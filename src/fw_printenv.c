/*
 * (C) Copyright 2019
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <stdbool.h>

#include "libuboot.h"

#ifndef DEFAULT_CFG_FILE
#define DEFAULT_CFG_FILE "/etc/fw_env.config"
#endif

#ifndef DEFAULT_ENV_FILE
#define DEFAULT_ENV_FILE "/etc/u-boot-initial-env"
#endif

#define PROGRAM_SET	"fw_setenv"

static struct option long_options[] = {
	{"version", no_argument, NULL, 'V'},
	{"no-header", no_argument, NULL, 'n'},
	{"help", no_argument, NULL, 'h'},
	{"config", required_argument, NULL, 'c'},
	{"defenv", required_argument, NULL, 'f'},
	{"script", required_argument, NULL, 's'},
	{"namespace", required_argument, NULL, 'm'},
	{NULL, 0, NULL, 0}
};

static void usage(char *program, bool setprogram)
{
	fprintf(stdout, "%s (compiled %s)\n", program, __DATE__);
	fprintf(stdout, "Usage %s [OPTION]\n",
			program);
	fprintf(stdout,
		" -h, --help                       : print this help\n"
		" -c, --config <filename>          : configuration file (by default: " DEFAULT_CFG_FILE ")\n"
		" -f, --defenv <filename>          : default environment if no one found (by default: " DEFAULT_ENV_FILE ")\n"
		" -m, --namespace <name>           : chose one of sets in the YAML file, default first in YAML\n"
		" -V, --version                    : print version and exit\n"
	);
	if (!setprogram)
		fprintf(stdout,
		" -n, --no-header                  : do not print variable name\n"
		);
	else
		fprintf(stdout,
		" -s, --script <filename>          : read variables to be set from a script\n"
		"\n"
		"Script Syntax:\n"
		" key=value\n"
		" lines starting with '#' are treated as comment\n"
		" lines without '=' are ignored\n"
		"\n"
		"Script Example:\n"
		" netdev=eth0\n"
		" kernel_addr=400000\n"
		" foo=empty empty empty    empty empty empty\n"
		" bar\n"
		"\n"
		);
}

int main (int argc, char **argv) {
	struct uboot_ctx *ctx = NULL;
	char *options = "Vc:f:s:nhm:";
	char *cfgfname = NULL;
	char *defenvfile = NULL;
	char *scriptfile = NULL;
	char *namespace = NULL;
	int c, i;
	int ret = 0;
	void *tmp;
	const char *name, *value;
	char *progname;
	bool is_setenv = false;
	bool noheader = false;
	bool default_used = false;
	struct uboot_version_info *version;
	char dt_namespace[32];
	size_t dt_ret;
	FILE *fp;

	/*
	 * As old tool, there is just a tool with symbolic link
	 */


	progname = strrchr(argv[0], '/');
	if (!progname)
		progname = argv[0];
	else
		progname++;

	if (!strcmp(progname, PROGRAM_SET))
		is_setenv = true;

	while ((c = getopt_long(argc, argv, options,
				long_options, NULL)) != EOF) {
		switch (c) {
		case 'c':
			cfgfname = strdup(optarg);
			break;
		case 'n':
			noheader = true;
			break;
		case 'V':
			fprintf(stdout, "%s %u\n", libuboot_version_info()->version, libuboot_version_info()->version_num);
			exit(0);
		case 'h':
			usage(progname, is_setenv);
			exit(0);
		case 'f':
			defenvfile = strdup(optarg);
			break;
		case 'm':
			namespace = strdup(optarg);
			break;
		case 's':
			scriptfile = strdup(optarg);
			break;
		}
	}

	argc -= optind;
	argv += optind;


	if (!cfgfname)
		cfgfname = DEFAULT_CFG_FILE;

	/*
	 * Try first new format, fallback to legacy
	 */
	ret = libuboot_read_config_ext(&ctx, cfgfname);
	if (ret) {
		fprintf(stderr, "Cannot initialize environment\n");
		exit(1);
	}

	if (namespace)
		ctx = libuboot_get_namespace(ctx, namespace);
	else {
		fp = fopen("/proc/device-tree/chosen/u-boot,env-config", "r");
		if(fp) {
			dt_ret = fread(dt_namespace, 1, sizeof(dt_namespace) - 1, fp);
			if (dt_ret) {
				dt_namespace[dt_ret] = 0;
				ctx = libuboot_get_namespace(ctx, dt_namespace);
			}

			fclose(fp);
		}
	}

	if (!ctx) {
		fprintf(stderr, "Namespace %s not found\n", namespace);
		exit (1);
	}

	if (!defenvfile)
		defenvfile = DEFAULT_ENV_FILE;

	if ((ret = libuboot_open(ctx)) < 0) {
		fprintf(stderr, "Cannot read environment, using default\n");
		if ((ret = libuboot_load_file(ctx, defenvfile)) < 0) {
			fprintf(stderr, "Cannot read default environment from file\n");
			exit (ret);
		}
		default_used = true;
	}

	if (!is_setenv) {
		/* No variable given, print all environment */
		if (!argc) {
			tmp = NULL;
			while ((tmp = libuboot_iterator(ctx, tmp)) != NULL) {
				name = libuboot_getname(tmp);
				value = libuboot_getvalue(tmp);
				fprintf(stdout, "%s=%s\n", name, value);
			}
		} else {
			for (i = 0; i < argc; i++) {
				value = libuboot_get_env(ctx, argv[i]);
				if (noheader)
					fprintf(stdout, "%s\n", value ? value : "");
				else
					fprintf(stdout, "%s=%s\n", argv[i], value ? value : "");
			}
		}
	} else { /* setenv branch */
		bool need_store = false;
		if (scriptfile) {
			libuboot_load_file(ctx, scriptfile);
			need_store = true;
		} else {
			for (i = 0; i < argc; i += 2) {
				value = libuboot_get_env(ctx, argv[i]);
				if (i + 1 == argc) {
					if (value != NULL) {
						int ret;

						ret = libuboot_set_env(ctx, argv[i], NULL);
						if (ret) {
							fprintf(stderr, "libuboot_set_env failed: %d\n", ret);
							exit(-ret);
						}

						need_store = true;
					}
				} else {
					if (value == NULL || strcmp(value, argv[i+1]) != 0) {
						int ret;

						ret = libuboot_set_env(ctx, argv[i], argv[i+1]);
						if (ret) {
							fprintf(stderr, "libuboot_set_env failed: %d\n", ret);
							exit(-ret);
						}

						need_store = true;
					}
				}
			}
		}

		if (need_store || default_used) {
			ret = libuboot_env_store(ctx);
			if (ret)
				fprintf(stderr, "Error storing the env\n");
		}
	}

	libuboot_close(ctx);
	libuboot_exit(ctx);

	return ret;
}
