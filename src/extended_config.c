/*
 * (C) Copyright 2024
 * Stefano Babic, <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

/**
 * @file extended_config.c
 *
 * @brief Implement the extended config file YAML
	*
 */
#define _GNU_SOURCE

#if !defined(NO_YAML_SUPPORT)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <yaml.h>

#include "uboot_private.h"
#include "common.h"

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
#endif
