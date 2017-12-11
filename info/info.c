#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "../lib/info.h"

static const char usage_options[] =
#ifdef SMALL
	"[-k[delim]] {[-r] key | -w key=value | -d key | -s pattern}...\n"
#else
	"[opts] {[-r] key | -w key=value | -d key | -s pattern}...\n"
	"  -r/-w/-d  read/write/delete a key\n"
	"  -s        subscribe to pattern (forever)\n"
	"options:\n"
	"  -k[delim] print key name when reading/subscribing\n"
	"  -S h:p    connect to TCP host:port\n"
#endif
	;

static struct {
	const char *key_delim;	/* -k[delim] */
#ifndef SMALL
	const char *socket;	/* -S */
#endif
} options;

static int
print_cb(const char *key, const char *value, unsigned int sz)
{
	if (options.key_delim)
		printf("%s%s", key, value ? options.key_delim : "");
	if (value && sz)
		fwrite(value, sz, 1, stdout);
	if (value || options.key_delim)
		putchar('\n');
	return 1;
}


int
main(int argc, char *argv[])
{
	int error = 0;
	int optind = 1;
	int i;
	int have_subs = 0;

	/* getopt has a habit of scanning all options on the line
	 * and I want to process them one at a time, so the following
	 * is pretty manual. */

	/* Gather connection options */
	while (optind < argc) {
		const char *arg = argv[optind];
		if (strncmp(arg, "-k", 2) == 0) {
			options.key_delim = arg[2] ? &arg[2] : " ";
			optind++;
			continue;
		}
#ifndef SMALL
		if (strcmp(arg, "-S") == 0) {
			options.socket = argv[optind + 1];
			if (!options.socket) {
				error = 2;
				break;
			}
			optind += 2;
			continue;
		}
#endif
		break;
	}

	/* Check command options */
	for (i = optind; !error && i < argc; i++) {
		const char *arg = argv[i];
		if (*arg != '-')
			continue; /* assume -r */
		if (arg[2] || !strchr("rwds", arg[1])) {
			fprintf(stderr, "bad option %s\n", arg);
			error = 2;
			break;
		}
		if (!argv[i + 1]) {
			fprintf(stderr, "missing arg after %s\n", arg);
			error = 2;
			break;
		}
		if (arg[1] == 'w' && !strchr(argv[i+1], '=')) {
			fprintf(stderr, "missing '=' after -w\n");
			error = 2;
			break;
		}
		i++;
	}

	if (error) {
		fprintf(stderr, "usage: %s %s", argv[0], usage_options);
		exit(error);
	}

#ifndef SMALL
	if (options.socket) {
		if (info_open(options.socket) == -1) {
			perror("info_open");
			exit(1);
		}
	}
#endif

	if (info_tx_begin() == -1)
		goto fail;
	for (i = optind; !error && i < argc; i++) {
		char *arg = argv[i];
		char *data = NULL;
		char *value;

		if (*arg != '-') {
			data = arg;
			arg = "-r";
		} else
			data = argv[++i];

		switch (arg[1]) {
		case 'r':
			if (info_tx_read(data) == -1)
				goto fail;
			break;
		case 'w':
			value = strchr(data, '=');
			*value++ = '\0';
			if (info_tx_write(data, value, strlen(value)) == -1)
				goto fail;
			break;
		case 'd':
			if (info_tx_delete(data) == -1)
				goto fail;
			break;
		case 's':
			if (info_tx_sub(data) == -1)
				goto fail;
			have_subs = 1;
			break;
		}
	}
	if (info_tx_commit(print_cb) == -1)
		goto fail;
	if (have_subs) {
		if (info_sub_wait(print_cb) == -1)
			goto fail;
	}
	exit(0);

fail:
	fprintf(stderr, "%s\n", info_get_last_error());
	exit(1);
}
