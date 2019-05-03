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

#ifndef VERSION
#define VERSION "0.1"
#endif

#define PROGRAM_SET	"fw_setenv"

static struct option long_options[] = {
	{"version", no_argument, NULL, 'V'},
	{"no-header", no_argument, NULL, 'n'},
	{"help", no_argument, NULL, 'h'},
	{"config", required_argument, NULL, 'c'},
	{"defenv", required_argument, NULL, 'f'},
	{"script", required_argument, NULL, 's'},
	{NULL, 0, NULL, 0}
};

static void usage(char *program, bool setprogram)
{
	fprintf(stdout, "%s (compiled %s)\n", program, __DATE__);
	fprintf(stdout, "Usage %s [OPTION]\n",
			program);
	fprintf(stdout,
		" -h,                              : print this help\n"
		" -c, --config <filename>          : configuration file (old fw_env.config)\n"
		" -f, --defenv <filename>          : default environment if no one found\n"
		" -V,                              : print version and exit\n"
	);
	if (!setprogram)
		fprintf(stdout,
		" -n, --no-header                  : do not print variable name\n"
		);
	else
		fprintf(stdout,
		" -s, --script <filename>          : read variables to be set from a script\n"
		);
}
	
int main (int argc, char **argv) {
	struct uboot_ctx *ctx;
	char *options = "Vc:f:s:nh";
	char *cfgfname = NULL;
	char *defenvfile = NULL;
	char *scriptfile = NULL;
	int c, i;
	int ret = 0;
	void *tmp;
	const char *name, *value;
	char *progname;
	bool is_setenv = false;
	bool noheader = false;

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
			fprintf(stdout, "%s\n", VERSION);
			exit(0);
		case 'h':
			usage(progname, is_setenv);
			exit(0);
		case 'f':
			defenvfile = strdup(optarg);
			break;
		case 's':
			scriptfile = strdup(optarg);
			break;
		}
	}
	
	argc -= optind;
	argv += optind;

	if (libuboot_initialize(&ctx, NULL) < 0) {
		fprintf(stderr, "Cannot initialize environment\n");
		exit(1);
	}

	if (!cfgfname)
		cfgfname = "/etc/fw_env.config";

	if ((ret = libuboot_read_config(ctx, cfgfname)) < 0) {
		fprintf(stderr, "Configuration file wrong or corrupted\n");
		exit (ret);
	}

	if (!defenvfile)
		defenvfile = "/etc/u-boot-initial-env";

	if ((ret = libuboot_open(ctx)) < 0) {
		fprintf(stderr, "Cannot read environment, using default\n");
		if ((ret = libuboot_load_file(ctx, defenvfile)) < 0) {
			fprintf(stderr, "Cannot read default environment from file\n");
			exit (ret);
		}
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
		if (scriptfile)
			libuboot_load_file(ctx, scriptfile);
		else {
			for (i = 0; i < argc; i += 2) {
				if (i + 1 == argc)
					libuboot_set_env(ctx, argv[i], NULL);
				else
					libuboot_set_env(ctx, argv[i], argv[i+1]);
			}
		}
		ret = libuboot_env_store(ctx);
		if (ret)
			fprintf(stderr, "Error storing the env\n");
	}

	libuboot_close(ctx);
	libuboot_exit(ctx);

	return ret;
}
