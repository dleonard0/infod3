#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#include "../lib/info.h"

static const char usage_options[] =
#ifdef SMALL
	"[-b] [-k[delim]] [-t secs] "
	"{[-r] key | -w key=value | -d key | -s pattern}...\n"
#else
	"[opts] {[-r] key | -w key=value | -d key | -s pattern}...\n"
	"  -r/-w/-d  read/write/delete a key\n"
	"  -s        subscribe to pattern (forever)\n"
	"options:\n"
	"  -b        output a blank line for deleted keys\n"
	"  -k[delim] print key name when reading/subscribing\n"
	"  -S h:p    connect to TCP host:port\n"
	"  -t secs   timeout a subscription\n"
#endif
	;

static struct {
	const char *key_delim;	/* -k[delim] */
	int timeout;
	int blank;
#ifndef SMALL
	const char *socket;	/* -S */
#endif
} options;

/* Number of times a deleted value was received */
static unsigned int deleted_count;
static int print_cb_flush;

static int
print_cb(const char *key, const char *value, unsigned int sz)
{
	if (!value) {
		deleted_count++;
		if (!options.blank)
			return 1;
	}
	if (options.key_delim)
		printf("%s%s", key, options.key_delim);
	if (sz)
		fwrite(value, sz, 1, stdout);
	putchar('\n');
	if (print_cb_flush)
		fflush(stdout);
	return 1;
}

static void
on_alarm(int sig)
{
	info_shutdown();
}

int
main(int argc, char *argv[])
{
	int error = 0;
	int optind = 1;
	int i;
	int have_subs = 0;

	options.timeout = -1;

	/* getopt has a habit of scanning all options on the line
	 * and I want to process them one at a time, so the following
	 * is pretty manual. */

	/* Gather connection options */
	while (optind < argc) {
		const char *opt = argv[optind];
		const char *arg = argv[optind + 1];
		if (strncmp(opt, "-k", 2) == 0) {
			options.key_delim = opt[2] ? &opt[2] : " ";
			optind++;
			continue;
		}
#ifndef SMALL
		if (strcmp(opt, "-S") == 0) {
			options.socket = arg;
			if (!options.socket) {
				error = 2;
				break;
			}
			optind += 2;
			continue;
		}
#endif
		if (strcmp(opt, "-b") == 0) {
			options.blank = 1;
			optind++;
			continue;
		}
		if (strncmp(opt, "-t", 2) == 0) {
			if (opt[2])
				arg = &opt[2];
			else
				arg = argv[++optind];
			if (!arg || sscanf(arg, "%d", &options.timeout) != 1) {
				fprintf(stderr, "invalid timeout\n");
				error = 2;
				break;
			}
			optind++;
			continue;
		}
		break;
	}

	/* Check command options */
	for (i = optind; !error && i < argc; i++) {
		const char *opt = argv[i];
		const char *arg;
		if (*opt != '-')
			continue; /* assume -r */
		if (opt[2] || !strchr("rwds", opt[1])) {
			fprintf(stderr, "bad option %s\n", opt);
			error = 2;
			break;
		}
		arg = argv[i + 1];
		if (!arg) {
			fprintf(stderr, "missing arg after %s\n", opt);
			error = 2;
			break;
		}
		if (opt[1] == 'w' && !strchr(arg, '=')) {
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
		char *opt = argv[i];
		char *data = NULL;
		char *value;

		if (*opt != '-') {
			data = opt;
			opt = "-r";
		} else
			data = argv[++i];

		switch (opt[1]) {
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
	if (options.timeout >= 0 && !have_subs)
		fprintf(stderr, "%s: timeout only applies to subscriptions\n",
			argv[0]);

	if (info_tx_commit(print_cb) == -1)
		goto fail;

	if (have_subs && options.timeout != 0) {
		/* Flush stdio buffers for each update */
		print_cb_flush = 1;
		fflush(stdout);

		if (options.timeout > 0) {
			if (signal(SIGALRM, on_alarm) == SIG_ERR) {
				perror("signal");
				exit(1);
			}
			alarm(options.timeout);
		}
		if (info_sub_wait(print_cb) == -1)
			goto fail;
	}
	exit(deleted_count);

fail:
	fprintf(stderr, "%s\n", info_get_last_error());
	exit(1);
}
