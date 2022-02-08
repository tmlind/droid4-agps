#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/timex.h>

#define ID_LEN			5

#define XTRA2_URL		http://xtrapath2.izatcloud.net/xtra2.bin
#define XTRA2_MAX_SIZE		(100 * 1024)

#define MAX_FILE_NAME		256
#define MOTMDM_MAX_BYTES	124

static const int debug = 1;
static const char *bin_name;
static int mfd;

static int download_almanac(const char **file)
{
	return 0;
}

#define BUF_SIZE		4096

static int gsmtty_send_command(int dlci, const char *fmt, const char *cmd,
			       const char *expect)
{
	char mesg[BUF_SIZE];
	char resp[BUF_SIZE];
	char dbg[BUF_SIZE + 3];
	struct timespec ts;
	struct pollfd pfd;
	unsigned short id;
	int error;
	char *m;

	pfd.fd = dlci;
	pfd.events = POLLIN;
	pfd.revents = 0;

	if (debug) {
		memset(dbg, 0, BUF_SIZE);
		sprintf(dbg, "> ");
		sprintf(dbg + 2, fmt, cmd);
		printf("%s\n", dbg);
	}

	error = clock_gettime(CLOCK_REALTIME, &ts);
	if (error)
		return error;

	id = (ts.tv_sec % 100) * 100;
	id += (ts.tv_nsec / 1000 / 1000 / 10);

	m = mesg;
	memset(m, 0, BUF_SIZE);
	m += sprintf(m, "U%04u", id);
	m += sprintf(m, fmt, cmd);

	error = dprintf(dlci, mesg);
	if (error < 0)
		return error;

	fsync(dlci);

	ts.tv_sec = 3;
	ts.tv_nsec = 0;
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
		sprintf(dbg + 2, "%s", resp + ID_LEN);
		printf("%s\n", dbg);
	}

	if (strncmp(expect, resp + ID_LEN, strlen(expect)))
		return -EIO;

	if (!strncmp("AT+MFSOPEN=", cmd, 11)) {
		/* +MFSOPEN:ERROR */
		if (!strncmp("ERROR", resp + ID_LEN + 8 + 1, 5))
			return -EIO;

		/* +MFSOPEN:n where n is a modem fd */
		error = atoi(resp + ID_LEN + 8 + 1);
	}

	return error;
}

static void gsmtty_kick_hung(int dlci)
{
	int error, retries = 10;

	while (retries--) {
		error = gsmtty_send_command(dlci, "%s",
					    "AT+MFSCLOSE=999\r",
					    "+MFSCLOSE:ERROR");
		if (!error)
			break;
	}
}

#define MOTMDM_DATA_PREFIX	"AT+MFSWRITE=%i,"
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
static int gsmtty_add_almanac(const char *file)
{
	const char *channel = "/dev/gsmtty6";
	unsigned char buf[MOTMDM_MAX_BYTES];
	char cmd[MOTMDM_MAX_CMDLEN];
	int error, dlci, data, i;
	ssize_t chunk, handled;
	char *p;

	dlci = open(channel, O_RDWR | O_NOCTTY | O_NDELAY);
	if (dlci < 0)
		return -ENODEV;

	error = gsmtty_send_command(dlci, "%s",
				    "AT+MFSOPEN=1234567890,\"xtra2.bin\"\r",
				    "+MFSOPEN:");
	if (error < 0)
		goto err_close_dlci;

	mfd = error;

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
		p += snprintf(p, sizeof(MOTMDM_DATA_PREFIX), MOTMDM_DATA_PREFIX, mfd);
		for (i = 0; i < chunk; i++) {
			snprintf(hexbyte, 3, "%02x", buf[i]);
			p += snprintf(p, 2, "%c", toupper(hexbyte[0]));
			p += snprintf(p, 2, "%c", toupper(hexbyte[1]));
		}
		p += snprintf(p, sizeof(MOTMDM_DATA_POSTFIX), ",%i\r", chunk);
		sprintf(resp, "+MFSWRITE:%i", chunk);

		error = gsmtty_send_command(dlci, "%s", cmd, resp);
		if (error < 0)
			goto err_data;

	} while (chunk == MOTMDM_MAX_BYTES);

	error = handled;

err_data:
	close(data);

err_mfsclose:
	if (error < 0)
		gsmtty_kick_hung(dlci);

	sprintf(cmd, "AT+MFSCLOSE=%i\r", mfd);
	error = gsmtty_send_command(dlci, "%s", cmd,
				    "+MFSCLOSE:OK");

err_close_dlci:
	close(dlci);

	return error;
}

static int gsmtty_inject_time(const char *file)
{
	const char *fmt = "U1234AT+MPDTIME=%u,%u,%u\r";
	const char *dev = "/dev/gnss0";
	unsigned long time_hi, time_lo;
	signed long long now_ms, gps_ms;
	int error, maxerr, fd;
	struct timeval *tv;
	struct timex tmx;

	fd = open(dev, O_RDWR | O_NOCTTY | O_NDELAY);
	if (fd < 0) {
		fprintf(stderr,
			"ERROR: %s %s open, check permissions: %i\n",
				__func__, dev, fd);

		return -ENODEV;
	}

	memset(&tmx, 0, sizeof(struct timex));
	error = adjtimex(&tmx);
	if (error)
		goto err_out;
	tv = &tmx.time;
	now_ms = tv->tv_sec * 1000;
	now_ms += tv->tv_usec / 1000;
	gps_ms = now_ms - 315964800000; /* Jan 6th 1980, no leap seconds */
	time_hi = gps_ms >> 32;
	time_lo = gps_ms & 0xffffffff;
	maxerr = tmx.maxerror / 1000;
	dprintf(fd, fmt, time_hi, time_lo, maxerr);

	printf("Injected time: %llu gps time: %llu (%lu,%lu,%i)\n",
		now_ms, gps_ms, time_hi, time_lo, maxerr);

	close(fd);

err_out:
	return error;
}

/* Note we need to manually prefix U1234 as we write via dev/gnss0 */
static int gsmtty_enable_almanac(const char *file)
{
	const char *cmd = "U1234AT+MPDXDATA=\"/mot_rmt/xtra2.bin\"";
	const char *dev = "/dev/gnss0";
	int fd;

	fd = open(dev, O_RDWR | O_NOCTTY | O_NDELAY);
	if (fd < 0) {
		fprintf(stderr,
			"ERROR: %s %s open, check permissions: %i\n",
				__func__, dev, fd);

		return -ENODEV;
	}

	dprintf(fd, "%s\r", cmd);

	close(fd);

	return 0;
}

static int gsmtty_clear_almanac(const char *file)
{
	const char *cmd = "U1234AT+MPDCLEAR=65535";
	const char *dev = "/dev/gnss0";
	int fd;

	fd = open(dev, O_RDWR | O_NOCTTY | O_NDELAY);
	if (fd < 0) {
		fprintf(stderr,
			"ERROR: %s %s open, check permissions: %i\n",
				__func__, dev, fd);

		return -ENODEV;
	}

	dprintf(fd, "%s\r", cmd);

	close(fd);

	return 0;
}

static int gsmtty_add_enable_almanac(const char *file)
{
	int error;

	error = gsmtty_add_almanac(file);
	if (error < 0)
		return error;

	error = gsmtty_enable_almanac(file);
	if (error)
		return error;

	return 0;
}

static int print_usage(void)
{
	printf("usage: %s [--help|--inject-time|--clear|--download-only=file|--upload-only=file]\n",
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

		if (!strncmp("--inject-time", argv[1], 13))
			return gsmtty_inject_time(file);

		if (!strncmp("--clear", argv[1], 7))
			return gsmtty_clear_almanac(file);

		if (!strncmp("--upload-only=", argv[1], 14)) {
			file = argv[1] + 14;
			error = check_file_name(file);
			if (error < 0)
				return print_usage();

			return gsmtty_add_enable_almanac(file);
		}

		if (!strncmp("--enable-only", argv[1], 13))
			return gsmtty_enable_almanac(NULL);
	}

	error = download_almanac(&file);
	if (error)
		return error;

	error = gsmtty_add_enable_almanac(file);
	if (error)
		return error;

	return 0;
}
