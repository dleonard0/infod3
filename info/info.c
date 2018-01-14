#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#include "../lib/info.h"

static const char usage_options[] =
#ifdef SMALL
	"[-ACb] [-k[delim]] [-t secs] "
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
	"  -A        print all keys (-k= -t0 -s*)\n"
	"  -C        clear all keys\n"
#endif
	;

static struct {
	const char *key_delim;	/* -k[delim] */
	int timeout;
	unsigned int blank : 1;		/* -b print deleted as blank */
	unsigned int all : 1;		/* -A print all */
	unsigned int clear : 1;		/* -C clear all */
#ifndef SMALL
	const char *socket;	/* -S */
#endif
} options;

/* Number of times a deleted value was received */
static unsigned int deleted_count;
static int print_cb_flush;

static int
action_cb(const char *key, const char *value, unsigned int sz)
{
	if (options.clear && value) {
		if (info_delete(key) == -1)
			perror("info_delete");
		return 1;
	}
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
	info_cb_close();
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
		if (strcmp(opt, "-A") == 0) {
			options.all = 1;
			optind++;
			continue;
		}
		if (strcmp(opt, "-C") == 0) {
			options.clear = 1;
			optind++;
			continue;
		}
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

	/* Check command options. No processing at this stage,
	 * we just verify that we will understand them later inside
	 * the transaction. */
	for (i = optind; !error && i < argc; i++) {
		const char *opt = argv[i];
		const char *arg;
		if (*opt != '-')
			continue;	/* assume implied -r or -w */
		if (!strchr("rwds", opt[1])) {
#ifndef SMALL
			if (strchr("ACbkt", opt[1]))
				fprintf(stderr, "-%c specified too late\n",
					opt[1]);
#endif
			fprintf(stderr, "bad option %s\n", opt);
			error = 2;
			break;
		}
		if (opt[2])
			arg = &opt[2];
		else
			arg = argv[++i];
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

	/* Start the transaction */
	if (info_tx_begin() == -1)
		goto fail;

	if (options.all) {
		/* -A print all keys */
		if (!options.key_delim)
			options.key_delim = "=";
	}
	if (options.all || options.clear) {
		/* -A and -C subscribe to * with timeout=0 */
		if (info_tx_sub("*") == -1)
			goto fail;
		have_subs = 1;
		if (options.timeout == -1)
			options.timeout = 0;
	}

	for (i = optind; !error && i < argc; i++) {
		char *opt = argv[i];
		char *data = NULL;
		char *value;

		if (*opt != '-') {		/* data      -> -r data */
			data = opt;		/* data=data -> -w data=data */
			opt = strchr(data, '=') ? "-w" : "-r";
		} else if (opt[2])
			data = opt + 2;		/* -Xdata */
		else
			data = argv[++i];	/* -X data */

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

	/* Complete the transaction. All -r/w/d/s will be performed
	 * in sequence and transactionally on the server. All immediate
	 * results from those commands will be handled by action_cb. */
	if (info_tx_commit(action_cb) == -1)
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
		if (info_sub_wait(action_cb) == -1)
			goto fail;
	}
	exit(deleted_count);

fail:
	fprintf(stderr, "%s\n", info_get_last_error());
	exit(1);
}
