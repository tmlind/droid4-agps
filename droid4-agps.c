#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#define XTRA2_URL		http://xtrapath2.izatcloud.net/xtra2.bin
#define XTRA2_MAX_SIZE		(100 * 1024)

#define MAX_FILE_NAME		256
#define MOTMDM_MAX_BYTES	124

static const int debug = 1;
static const char *bin_name;

static int download_almanac(const char **file)
{
	return 0;
}

#define BUF_SIZE		4096

static int motmdm_send_command(int dlci, const char *fmt, const char *cmd,
			       const char *expect)
{
	char resp[BUF_SIZE];
	char dbg[BUF_SIZE + 3];
	struct timespec ts;
	struct pollfd pfd;
	int error;

	ts.tv_sec = 3;
	ts.tv_nsec = 0;
	pfd.fd = dlci;
	pfd.events = POLLIN;
	pfd.revents = 0;

	if (debug) {
		memset(dbg, 0, BUF_SIZE);
		sprintf(dbg, "> ");
		sprintf(dbg + 2, fmt, cmd);
		printf("%s\n", dbg);
	}

	error = dprintf(dlci, fmt, cmd);
	if (error < 0)
		return error;

	fsync(dlci);

	error = ppoll(&pfd, 1, &ts, NULL);
	switch (error) {
	case 0:
		fprintf(stderr, "ERROR: reading dlci timed out\n");
		return -ETIMEDOUT;
	case -1:
		fprintf(stderr, "ERROR reading dlci: %i\n", errno);
		return errno;
	default:
		break;
	}

	memset(resp, 0, BUF_SIZE);
	error = read(dlci, resp, BUF_SIZE);
	if (error < 0)
		return error;

	if (debug) {
		memset(dbg, 0, BUF_SIZE);
		sprintf(dbg, "< ");
		sprintf(dbg + 2, "%s", resp);
		printf("%s\n", dbg);
	}

	if (strncmp(expect, resp, strlen(expect)))
		return -EIO;

	return 0;
}

static void motmdm_kick_hung(int dlci)
{
	int error, retries = 10;

	while (retries--) {
		error = motmdm_send_command(dlci, "%s",
					    "AT+MFSCLOSE=999\r",
					    "+MFSCLOSE:ERROR");
		if (!error)
			break;
	}
}

#define MOTMDM_DATA_PREFIX	"AT+MFSWRITE=0,"
#define MOTMDM_DATA_POSTFIX	",XXX\r"
#define MOTMDM_MAX_CMDLEN	(sizeof(MOTMDM_DATA_PREFIX) + \
				 MOTMDM_MAX_BYTES * 2 + \
				 sizeof(MOTMDM_DATA_POSTFIX))

/*
 * Command format is:
 * "AT+MFSOPEN=2705156891,"xtra2.bin"\r" "+MFSOPEN:0"
 * "AT+MFSWRITE=0,data,len\r" "+MFSWRITE:len"
 * "AT+MFSCLOSE=0\r" "+MFSCLOSE:OK"
 */
static int motmdm_add_almanac(const char *file)
{
	const char *channel = "/dev/motmdm6";
	unsigned char buf[MOTMDM_MAX_BYTES];
	char cmd[MOTMDM_MAX_CMDLEN];
	int error, dlci, data, i;
	ssize_t chunk, handled;
	char *p;

	dlci = open(channel, O_RDWR | O_NOCTTY | O_NDELAY);
	if (dlci < 0)
		return -ENODEV;

	error = motmdm_send_command(dlci, "%s",
				    "AT+MFSOPEN=1234567890,\"xtra2.bin\"\r",
				    "+MFSOPEN:0");
	if (error < 0)
		goto err_close_dlci;

	data = open(file, O_RDONLY);
	if (data < 0) {
		error = -EINVAL;
		goto err_mfsclose;
	}

	do {
		char hexbyte[3];
		char resp[14];

		chunk = read(data, buf, MOTMDM_MAX_BYTES);
		handled += chunk;
		if (handled > XTRA2_MAX_SIZE) {
			printf("Data too big, bailing out\n");
			error = -EINVAL;
			goto err_data;
		}

		p = cmd;
		p += snprintf(p, sizeof(MOTMDM_DATA_PREFIX), "%s", MOTMDM_DATA_PREFIX);
		for (i = 0; i < chunk; i++) {
			snprintf(hexbyte, 3, "%02x", buf[i]);
			p += snprintf(p, 2, "%c", toupper(hexbyte[0]));
			p += snprintf(p, 2, "%c", toupper(hexbyte[1]));
		}
		p += snprintf(p, sizeof(MOTMDM_DATA_POSTFIX), ",%i\r", chunk);
		sprintf(resp, "+MFSWRITE:%i", chunk);

		error = motmdm_send_command(dlci, "%s", cmd, resp);
		if (error < 0)
			goto err_data;

	} while (chunk == MOTMDM_MAX_BYTES);

	error = handled;

err_data:
	close(data);

err_mfsclose:
	if (error < 0)
		motmdm_kick_hung(dlci);

	error = motmdm_send_command(dlci, "%s",
				    "AT+MFSCLOSE=0\r",
				    "+MFSCLOSE:OK");

err_close_dlci:
	close(dlci);

	return error;
}

static int motmdm_enable_almanac(const char *file)
{
	return 0;
}

static int motmdm_add_enable_almanac(const char *file)
{
	int error;

	error = motmdm_add_almanac(file);
	if (error < 0)
		return error;

	error = motmdm_enable_almanac(file);
	if (error)
		return error;

	return 0;
}

static int print_usage(void)
{
	printf("usage: %s [--help|--download-only=file|--upload-only=file]\n",
	       bin_name);

	return -EINVAL;
}

static int check_file_name(const char *name)
{
	int len;

	len = strnlen(name, MAX_FILE_NAME);
	if (!len || len >= MAX_FILE_NAME)
		return print_usage();

	return len;
}

int main(const int argc, const char **argv)
{
	const char *file = NULL;
	int error;

	bin_name = argv[0];

	if (argc >= 2) {
		if (!strncmp("--help", argv[1], 6))
			return print_usage();

		if (!strncmp("--download-only=", argv[1], 16)) {
			file = argv[1] + 16;
			error = check_file_name(file);
			if (error < 0)
				return print_usage();

			return download_almanac(&file);
		}

		if (!strncmp("--upload-only=", argv[1], 14)) {
			file = argv[1] + 14;
			error = check_file_name(file);
			if (error < 0)
				return print_usage();

			return motmdm_add_enable_almanac(file);
		}
	}

	error = download_almanac(&file);
	if (error)
		return error;

	error = motmdm_add_enable_almanac(file);
	if (error)
		return error;

	return 0;
}