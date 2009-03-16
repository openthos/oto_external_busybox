/* vi: set sw=4 ts=4: */
/*
 * Simple FTP daemon, based on vsftpd 2.0.7 (written by Chris Evans)
 *
 * Author: Adam Tkac <vonsch@gmail.com>
 *
 * Licensed under GPLv2, see file LICENSE in this tarball for details.
 *
 * Only subset of FTP protocol is implemented but vast majority of clients
 * should not have any problem. You have to run this daemon via inetd.
 *
 * Options:
 * -w	- enable FTP write commands
 *
 * TODO: implement "421 Timeout" thingy (alarm(60) while waiting for a cmd).
 */

#include "libbb.h"
#include <syslog.h>
#include <netinet/tcp.h>

#define FTP_DATACONN            150
#define FTP_NOOPOK              200
#define FTP_TYPEOK              200
#define FTP_PORTOK              200
#define FTP_STRUOK              200
#define FTP_MODEOK              200
#define FTP_ALLOOK              202
#define FTP_STATOK              211
#define FTP_STATFILE_OK         213
#define FTP_HELP                214
#define FTP_SYSTOK              215
#define FTP_GREET               220
#define FTP_GOODBYE             221
#define FTP_TRANSFEROK          226
#define FTP_PASVOK              227
/*#define FTP_EPRTOK              228*/
#define FTP_EPSVOK              229
#define FTP_LOGINOK             230
#define FTP_CWDOK               250
#define FTP_RMDIROK             250
#define FTP_DELEOK              250
#define FTP_RENAMEOK            250
#define FTP_PWDOK               257
#define FTP_MKDIROK             257
#define FTP_GIVEPWORD           331
#define FTP_RESTOK              350
#define FTP_RNFROK              350
#define FTP_BADSENDCONN         425
#define FTP_BADSENDNET          426
#define FTP_BADSENDFILE         451
#define FTP_BADCMD              500
#define FTP_COMMANDNOTIMPL      502
#define FTP_NEEDUSER            503
#define FTP_NEEDRNFR            503
#define FTP_BADSTRU             504
#define FTP_BADMODE             504
#define FTP_LOGINERR            530
#define FTP_FILEFAIL            550
#define FTP_NOPERM              550
#define FTP_UPLOADFAIL          553

#define STR1(s) #s
#define STR(s) STR1(s)

/* Convert a constant to 3-digit string, packed into uint32_t */
enum {
	/* Shift for Nth decimal digit */
	SHIFT2 =  0 * BB_LITTLE_ENDIAN + 24 * BB_BIG_ENDIAN,
	SHIFT1 =  8 * BB_LITTLE_ENDIAN + 16 * BB_BIG_ENDIAN,
	SHIFT0 = 16 * BB_LITTLE_ENDIAN + 8 * BB_BIG_ENDIAN,
};
#define STRNUM32(s) (uint32_t)(0 \
	| (('0' + ((s) / 1 % 10)) << SHIFT0) \
	| (('0' + ((s) / 10 % 10)) << SHIFT1) \
	| (('0' + ((s) / 100 % 10)) << SHIFT2) \
)

struct globals {
	char *p_control_line_buf;
	len_and_sockaddr *local_addr;
	len_and_sockaddr *port_addr;
	int pasv_listen_fd;
	int proc_self_fd;
	off_t restart_pos;
	char *ftp_cmd;
	char *ftp_arg;
#if ENABLE_FEATURE_FTP_WRITE
	char *rnfr_filename;
#endif
};
#define G (*(struct globals*)&bb_common_bufsiz1)
#define INIT_G() do { } while (0)


static char *
escape_text(const char *prepend, const char *str, unsigned escapee)
{
	unsigned retlen, remainlen, chunklen;
	char *ret, *found;
	char append;

	append = (char)escapee;
	escapee >>= 8;

	remainlen = strlen(str);
	retlen = strlen(prepend);
	ret = xmalloc(retlen + remainlen * 2 + 1 + 1);
	strcpy(ret, prepend);

	for (;;) {
		found = strchrnul(str, escapee);
		chunklen = found - str + 1;

		/* Copy chunk up to and including escapee (or NUL) to ret */
		memcpy(ret + retlen, str, chunklen);
		retlen += chunklen;

		if (*found == '\0') {
			/* It wasn't escapee, it was NUL! */
			ret[retlen - 1] = append; /* replace NUL */
			ret[retlen] = '\0'; /* add NUL */
			break;
		}
		ret[retlen++] = escapee; /* duplicate escapee */
		str = found + 1;
	}
	return ret;
}

/* Returns strlen as a bonus */
static unsigned
replace_char(char *str, char from, char to)
{
	char *p = str;
	while (*p) {
		if (*p == from)
			*p = to;
		p++;
	}
	return p - str;
}

/* NB: status_str is char[4] packed into uint32_t */
static void
cmdio_write(uint32_t status_str, const char *str)
{
	char *response;
	int len;

	/* FTP allegedly uses telnet protocol for command link.
	 * In telnet, 0xff is an escape char, and needs to be escaped: */
	response = escape_text((char *) &status_str, str, (0xff << 8) + '\r');

	/* ?! does FTP send embedded LFs as NULs? wow */
	len = replace_char(response, '\n', '\0');

	response[len++] = '\n'; /* tack on trailing '\n' */
	xwrite(STDOUT_FILENO, response, len);
	free(response);
}

static void
cmdio_write_ok(int status)
{
	fdprintf(STDOUT_FILENO, "%u Operation successful\r\n", status);
}

/* TODO: output strerr(errno) if errno != 0? */
static void
cmdio_write_error(int status)
{
	fdprintf(STDOUT_FILENO, "%u Error\r\n", status);
}

static void
cmdio_write_raw(const char *p_text)
{
	xwrite_str(STDOUT_FILENO, p_text);
}

/* Simple commands */

static void
handle_pwd(void)
{
	char *cwd, *response;

	cwd = xrealloc_getcwd_or_warn(NULL);
	if (cwd == NULL)
		cwd = xstrdup("");

	/* We have to promote each " to "" */
	response = escape_text(" \"", cwd, ('"' << 8) + '"');
	free(cwd);
	cmdio_write(STRNUM32(FTP_PWDOK), response);
	free(response);
}

static void
handle_cwd(void)
{
	if (!G.ftp_arg || chdir(G.ftp_arg) != 0) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}
	cmdio_write_ok(FTP_CWDOK);
}

static void
handle_cdup(void)
{
	G.ftp_arg = (char*)"..";
	handle_cwd();
}

static void
handle_stat(void)
{
	cmdio_write_raw(STR(FTP_STATOK)"-FTP server status:\r\n"
			" TYPE: BINARY\r\n"
			STR(FTP_STATOK)" Ok\r\n");
}

/* TODO: implement FEAT. Example:
# nc -vvv ftp.kernel.org 21
ftp.kernel.org (130.239.17.4:21) open
220 Welcome to ftp.kernel.org.
FEAT
211-Features:
 EPRT
 EPSV
 MDTM
 PASV
 REST STREAM
 SIZE
 TVFS
 UTF8
211 End
HELP
214-The following commands are recognized.
 ABOR ACCT ALLO APPE CDUP CWD  DELE EPRT EPSV FEAT HELP LIST MDTM MKD
 MODE NLST NOOP OPTS PASS PASV PORT PWD  QUIT REIN REST RETR RMD  RNFR
 RNTO SITE SIZE SMNT STAT STOR STOU STRU SYST TYPE USER XCUP XCWD XMKD
 XPWD XRMD
214 Help OK.
*/
static void
handle_help(void)
{
	cmdio_write_raw(STR(FTP_HELP)"-Commands:\r\n"
			" ALLO CDUP CWD EPSV HELP LIST\r\n"
			" MODE NLST NOOP PASS PASV PORT PWD QUIT\r\n"
			" REST RETR SIZE STAT STRU SYST TYPE USER\r\n"
#if ENABLE_FEATURE_FTP_WRITE
			" APPE DELE MKD RMD RNFR RNTO STOR STOU\r\n"
#endif
			STR(FTP_HELP)" Ok\r\n");
}

/* Download commands */

static inline int
port_active(void)
{
	return (G.port_addr != NULL);
}

static inline int
pasv_active(void)
{
	return (G.pasv_listen_fd > STDOUT_FILENO);
}

static void
port_pasv_cleanup(void)
{
	free(G.port_addr);
	G.port_addr = NULL;
	if (G.pasv_listen_fd > STDOUT_FILENO)
		close(G.pasv_listen_fd);
	G.pasv_listen_fd = -1;
}

/* On error, emits error code to the peer */
static int
ftpdataio_get_pasv_fd(void)
{
	int remote_fd;

	remote_fd = accept(G.pasv_listen_fd, NULL, 0);

	if (remote_fd < 0) {
		cmdio_write_error(FTP_BADSENDCONN);
		return remote_fd;
	}

	setsockopt(remote_fd, SOL_SOCKET, SO_KEEPALIVE, &const_int_1, sizeof(const_int_1));
	return remote_fd;
}

/* Clears port/pasv data.
 * This means we dont waste resources, for example, keeping
 * PASV listening socket open when it is no longer needed.
 * On error, emits error code to the peer (or exits).
 * On success, emits p_status_msg to the peer.
 */
static int
get_remote_transfer_fd(const char *p_status_msg)
{
	int remote_fd;

	if (pasv_active())
		/* On error, emits error code to the peer */
		remote_fd = ftpdataio_get_pasv_fd();
	else
		/* Exits on error */
		remote_fd = xconnect_stream(G.port_addr);

	port_pasv_cleanup();

	if (remote_fd < 0)
		return remote_fd;

	cmdio_write(STRNUM32(FTP_DATACONN), p_status_msg);
	return remote_fd;
}

/* If there were neither PASV nor PORT, emits error code to the peer */
static int
port_or_pasv_was_seen(void)
{
	if (!pasv_active() && !port_active()) {
		cmdio_write_raw(STR(FTP_BADSENDCONN)" Use PORT or PASV first\r\n");
		return 0;
	}

	return 1;
}

/* Exits on error */
static unsigned
bind_for_passive_mode(void)
{
	int fd;
	unsigned port;

	port_pasv_cleanup();

	G.pasv_listen_fd = fd = xsocket(G.local_addr->u.sa.sa_family, SOCK_STREAM, 0);
	setsockopt_reuseaddr(fd);

	set_nport(G.local_addr, 0);
	xbind(fd, &G.local_addr->u.sa, G.local_addr->len);
	xlisten(fd, 1);
	getsockname(fd, &G.local_addr->u.sa, &G.local_addr->len);

	port = get_nport(&G.local_addr->u.sa);
	port = ntohs(port);
	return port;
}

/* Exits on error */
static void
handle_pasv(void)
{
	unsigned port;
	char *addr, *response;

	port = bind_for_passive_mode();

	if (G.local_addr->u.sa.sa_family == AF_INET)
		addr = xmalloc_sockaddr2dotted_noport(&G.local_addr->u.sa);
	else /* seen this in the wild done by other ftp servers: */
		addr = xstrdup("0.0.0.0");
	replace_char(addr, '.', ',');

	response = xasprintf(STR(FTP_PASVOK)" Entering Passive Mode (%s,%u,%u)\r\n",
			addr, (int)(port >> 8), (int)(port & 255));
	free(addr);
	cmdio_write_raw(response);
	free(response);
}

/* Exits on error */
static void
handle_epsv(void)
{
	unsigned port;
	char *response;

	port = bind_for_passive_mode();
	response = xasprintf(STR(FTP_EPSVOK)" EPSV Ok (|||%u|)\r\n", port);
	cmdio_write_raw(response);
	free(response);
}

static void
handle_port(void)
{
	unsigned port, port_hi;
	char *raw, *comma;
	socklen_t peer_ipv4_len;
	struct sockaddr_in peer_ipv4;
	struct in_addr port_ipv4_sin_addr;

	port_pasv_cleanup();

	raw = G.ftp_arg;

	/* PORT command format makes sense only over IPv4 */
	if (!raw || G.local_addr->u.sa.sa_family != AF_INET) {
 bail:
		cmdio_write_error(FTP_BADCMD);
		return;
	}

	comma = strrchr(raw, ',');
	if (comma == NULL)
		goto bail;
	*comma = '\0';
	port = bb_strtou(&comma[1], NULL, 10);
	if (errno || port > 0xff)
		goto bail;

	comma = strrchr(raw, ',');
	if (comma == NULL)
		goto bail;
	*comma = '\0';
	port_hi = bb_strtou(&comma[1], NULL, 10);
	if (errno || port_hi > 0xff)
		goto bail;
	port |= port_hi << 8;

	replace_char(raw, ',', '.');

	/* We are verifying that PORT's IP matches getpeername().
	 * Otherwise peer can make us open data connections
	 * to other hosts (security problem!)
	 * This code would be too simplistic:
	 * lsa = xdotted2sockaddr(raw, port);
	 * if (lsa == NULL) goto bail;
	 */
	if (!inet_aton(raw, &port_ipv4_sin_addr))
		goto bail;
	peer_ipv4_len = sizeof(peer_ipv4);
	if (getpeername(STDIN_FILENO, &peer_ipv4, &peer_ipv4_len) != 0)
		goto bail;
	if (memcmp(&port_ipv4_sin_addr, &peer_ipv4.sin_addr, sizeof(struct in_addr)) != 0)
		goto bail;

	G.port_addr = xdotted2sockaddr(raw, port);
	cmdio_write_ok(FTP_PORTOK);
}

static void
handle_rest(void)
{
	/* When ftp_arg == NULL simply restart from beginning */
	G.restart_pos = G.ftp_arg ? xatoi_u(G.ftp_arg) : 0;
	cmdio_write_ok(FTP_RESTOK);
}

static void
handle_retr(void)
{
	struct stat statbuf;
	off_t bytes_transferred;
	int remote_fd;
	int local_file_fd;
	off_t offset = G.restart_pos;
	char *response;

	G.restart_pos = 0;

	if (!port_or_pasv_was_seen())
		return; /* port_or_pasv_was_seen emitted error response */

	/* O_NONBLOCK is useful if file happens to be a device node */
	local_file_fd = G.ftp_arg ? open(G.ftp_arg, O_RDONLY | O_NONBLOCK) : -1;
	if (local_file_fd < 0) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}

	if (fstat(local_file_fd, &statbuf) != 0 || !S_ISREG(statbuf.st_mode)) {
		/* Note - pretend open failed */
		cmdio_write_error(FTP_FILEFAIL);
		goto file_close_out;
	}

	/* Now deactive O_NONBLOCK, otherwise we have a problem
	 * on DMAPI filesystems such as XFS DMAPI.
	 */
	ndelay_off(local_file_fd);

	/* Set the download offset (from REST) if any */
	if (offset != 0)
		xlseek(local_file_fd, offset, SEEK_SET);

	response = xasprintf(
		" Opening BINARY mode data connection for %s (%"OFF_FMT"u bytes)",
		G.ftp_arg, statbuf.st_size);
	remote_fd = get_remote_transfer_fd(response);
	free(response);
	if (remote_fd < 0)
		goto file_close_out;

/* TODO: if we'll implement timeout, this will need more clever handling.
 * Perhaps alarm(N) + checking that current position on local_file_fd
 * is advancing. As of now, peer may stall us indefinitely.
 */

	bytes_transferred = bb_copyfd_eof(local_file_fd, remote_fd);
	close(remote_fd);
	if (bytes_transferred < 0)
		cmdio_write_error(FTP_BADSENDFILE);
	else
		cmdio_write_ok(FTP_TRANSFEROK);

 file_close_out:
	close(local_file_fd);
}

/* List commands */

static int
popen_ls(const char *opt)
{
	char *cwd;
	const char *argv[5] = { "ftpd", opt, NULL, G.ftp_arg, NULL };
	struct fd_pair outfd;
	pid_t pid;

	cwd = xrealloc_getcwd_or_warn(NULL);
	xpiped_pair(outfd);

	/*fflush(NULL); - so far we dont use stdio on output */
	pid = vfork();
	switch (pid) {
	case -1:  /* failure */
		bb_perror_msg_and_die("vfork");
	case 0:  /* child */
		/* NB: close _first_, then move fds! */
		close(outfd.rd);
		xmove_fd(outfd.wr, STDOUT_FILENO);
		close(STDIN_FILENO);
		/* xopen("/dev/null", O_RDONLY); - chroot may lack it! */
		if (fchdir(G.proc_self_fd) == 0) {
			close(G.proc_self_fd);
			argv[2] = cwd;
			/* ftpd ls helper chdirs to argv[2],
			 * preventing peer from seeing /proc/self
			 */
			execv("exe", (char**) argv);
		}
		_exit(127);
	}

	/* parent */
	close(outfd.wr);
	free(cwd);
	return outfd.rd;
}

enum {
	USE_CTRL_CONN = 1,
	LONG_LISTING = 2,
};

static void
handle_dir_common(int opts)
{
	FILE *ls_fp;
	char *line;
	int ls_fd;

	if (!(opts & USE_CTRL_CONN) && !port_or_pasv_was_seen())
		return; /* port_or_pasv_was_seen emitted error response */

	/* -n prevents user/groupname display,
	 * which can be problematic in chroot */
	ls_fd = popen_ls((opts & LONG_LISTING) ? "-l" : "-1");
	ls_fp = fdopen(ls_fd, "r");
	if (!ls_fp) /* never happens. paranoia */
		bb_perror_msg_and_die("fdopen");

	if (opts & USE_CTRL_CONN) {
		/* STAT <filename> */
		cmdio_write_raw(STR(FTP_STATFILE_OK)"-Status follows:\r\n");
		while (1) {
    			line = xmalloc_fgetline(ls_fp);
			if (!line)
				break;
			cmdio_write(0, line); /* hack: 0 results in no status at all */
			free(line);
		}
		cmdio_write_ok(FTP_STATFILE_OK);
	} else {
		/* LIST/NLST [<filename>] */
		int remote_fd = get_remote_transfer_fd(" Here comes the directory listing");
		if (remote_fd >= 0) {
			while (1) {
    				line = xmalloc_fgetline(ls_fp);
				if (!line)
					break;
				/* I've seen clients complaining when they
				 * are fed with ls output with bare '\n'.
				 * Pity... that would be much simpler.
				 */
				xwrite_str(remote_fd, line);
				xwrite(remote_fd, "\r\n", 2);
				free(line);
			}
		}
		close(remote_fd);
		cmdio_write_ok(FTP_TRANSFEROK);
	}
	fclose(ls_fp); /* closes ls_fd too */
}
static void
handle_list(void)
{
	handle_dir_common(LONG_LISTING);
}
static void
handle_nlst(void)
{
	handle_dir_common(0);
}
static void
handle_stat_file(void)
{
	handle_dir_common(LONG_LISTING + USE_CTRL_CONN);
}

static void
handle_size(void)
{
	struct stat statbuf;
	char buf[sizeof(STR(FTP_STATFILE_OK)" %"OFF_FMT"u\r\n") + sizeof(off_t)*3];

	if (!G.ftp_arg
	 || stat(G.ftp_arg, &statbuf) != 0
	 || !S_ISREG(statbuf.st_mode)
	) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}
	sprintf(buf, STR(FTP_STATFILE_OK)" %"OFF_FMT"u\r\n", statbuf.st_size);
	cmdio_write_raw(buf);
}

/* Upload commands */

#if ENABLE_FEATURE_FTP_WRITE
static void
handle_mkd(void)
{
	if (!G.ftp_arg || mkdir(G.ftp_arg, 0777) != 0) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}
	cmdio_write_ok(FTP_MKDIROK);
}

static void
handle_rmd(void)
{
	if (!G.ftp_arg || rmdir(G.ftp_arg) != 0) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}
	cmdio_write_ok(FTP_RMDIROK);
}

static void
handle_dele(void)
{
	if (!G.ftp_arg || unlink(G.ftp_arg) != 0) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}
	cmdio_write_ok(FTP_DELEOK);
}

static void
handle_rnfr(void)
{
	free(G.rnfr_filename);
	G.rnfr_filename = xstrdup(G.ftp_arg);
	cmdio_write_ok(FTP_RNFROK);
}

static void
handle_rnto(void)
{
	int retval;

	/* If we didn't get a RNFR, throw a wobbly */
	if (G.rnfr_filename == NULL || G.ftp_arg == NULL) {
		cmdio_write_raw(STR(FTP_NEEDRNFR)" RNFR required first\r\n");
		return;
	}

	retval = rename(G.rnfr_filename, G.ftp_arg);
	free(G.rnfr_filename);
	G.rnfr_filename = NULL;

	if (retval) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}
	cmdio_write_ok(FTP_RENAMEOK);
}

static void
handle_upload_common(int is_append, int is_unique)
{
	struct stat statbuf;
	char *tempname;
	off_t bytes_transferred;
	off_t offset;
	int local_file_fd;
	int remote_fd;

	offset = G.restart_pos;
	G.restart_pos = 0;

	if (!port_or_pasv_was_seen())
		return; /* port_or_pasv_was_seen emitted error response */

	tempname = NULL;
	local_file_fd = -1;
	if (is_unique) {
		tempname = xstrdup(" FILE: uniq.XXXXXX");
		local_file_fd = mkstemp(tempname + 7);
	} else if (G.ftp_arg) {
		int flags = O_WRONLY | O_CREAT | O_TRUNC;
		if (is_append)
			flags = O_WRONLY | O_CREAT | O_APPEND;
		if (offset)
			flags = O_WRONLY | O_CREAT;
		local_file_fd = open(G.ftp_arg, flags, 0666);
	}

	if (local_file_fd < 0
	 || fstat(local_file_fd, &statbuf) != 0
	 || !S_ISREG(statbuf.st_mode)
	) {
		cmdio_write_error(FTP_UPLOADFAIL);
		if (local_file_fd >= 0)
			goto close_local_and_bail;
		return;
	}

	if (offset)
		xlseek(local_file_fd, offset, SEEK_SET);

	remote_fd = get_remote_transfer_fd(tempname ? tempname : " Ok to send data");
	free(tempname);

	if (remote_fd < 0)
		goto close_local_and_bail;

/* TODO: if we'll implement timeout, this will need more clever handling.
 * Perhaps alarm(N) + checking that current position on local_file_fd
 * is advancing. As of now, peer may stall us indefinitely.
 */

	bytes_transferred = bb_copyfd_eof(remote_fd, local_file_fd);
	close(remote_fd);
	if (bytes_transferred < 0)
		cmdio_write_error(FTP_BADSENDFILE);
	else
		cmdio_write_ok(FTP_TRANSFEROK);

 close_local_and_bail:
	close(local_file_fd);
}

static void
handle_stor(void)
{
	handle_upload_common(0, 0);
}

static void
handle_appe(void)
{
	G.restart_pos = 0;
	handle_upload_common(1, 0);
}

static void
handle_stou(void)
{
	G.restart_pos = 0;
	handle_upload_common(0, 1);
}
#endif /* ENABLE_FEATURE_FTP_WRITE */

static uint32_t
cmdio_get_cmd_and_arg(void)
{
	size_t len;
	uint32_t cmdval;
	char *cmd;

	free(G.ftp_cmd);
	len = 8 * 1024; /* Paranoia. Peer may send 1 gigabyte long cmd... */
	G.ftp_cmd = cmd = xmalloc_reads(STDIN_FILENO, NULL, &len);
	if (!cmd)
		exit(0);

	/* Trailing '\n' is already stripped, strip '\r' */
	len = strlen(cmd) - 1;
	while ((ssize_t)len >= 0 && cmd[len] == '\r') {
		cmd[len] = '\0';
		len--;
	}

	G.ftp_arg = strchr(cmd, ' ');
	if (G.ftp_arg != NULL)
		*G.ftp_arg++ = '\0';

	/* Uppercase and pack into uint32_t first word of the command */
	cmdval = 0;
	while (*cmd)
		cmdval = (cmdval << 8) + ((unsigned char)*cmd++ & (unsigned char)~0x20);

	return cmdval;
}

#define mk_const4(a,b,c,d) (((a * 0x100 + b) * 0x100 + c) * 0x100 + d)
#define mk_const3(a,b,c)    ((a * 0x100 + b) * 0x100 + c)
enum {
	const_ALLO = mk_const4('A', 'L', 'L', 'O'),
	const_APPE = mk_const4('A', 'P', 'P', 'E'),
	const_CDUP = mk_const4('C', 'D', 'U', 'P'),
	const_CWD  = mk_const3('C', 'W', 'D'),
	const_DELE = mk_const4('D', 'E', 'L', 'E'),
	const_EPSV = mk_const4('E', 'P', 'S', 'V'),
	const_HELP = mk_const4('H', 'E', 'L', 'P'),
	const_LIST = mk_const4('L', 'I', 'S', 'T'),
	const_MKD  = mk_const3('M', 'K', 'D'),
	const_MODE = mk_const4('M', 'O', 'D', 'E'),
	const_NLST = mk_const4('N', 'L', 'S', 'T'),
	const_NOOP = mk_const4('N', 'O', 'O', 'P'),
	const_PASS = mk_const4('P', 'A', 'S', 'S'),
	const_PASV = mk_const4('P', 'A', 'S', 'V'),
	const_PORT = mk_const4('P', 'O', 'R', 'T'),
	const_PWD  = mk_const3('P', 'W', 'D'),
	const_QUIT = mk_const4('Q', 'U', 'I', 'T'),
	const_REST = mk_const4('R', 'E', 'S', 'T'),
	const_RETR = mk_const4('R', 'E', 'T', 'R'),
	const_RMD  = mk_const3('R', 'M', 'D'),
	const_RNFR = mk_const4('R', 'N', 'F', 'R'),
	const_RNTO = mk_const4('R', 'N', 'T', 'O'),
	const_SIZE = mk_const4('S', 'I', 'Z', 'E'),
	const_STAT = mk_const4('S', 'T', 'A', 'T'),
	const_STOR = mk_const4('S', 'T', 'O', 'R'),
	const_STOU = mk_const4('S', 'T', 'O', 'U'),
	const_STRU = mk_const4('S', 'T', 'R', 'U'),
	const_SYST = mk_const4('S', 'Y', 'S', 'T'),
	const_TYPE = mk_const4('T', 'Y', 'P', 'E'),
	const_USER = mk_const4('U', 'S', 'E', 'R'),

	OPT_l = (1 << 0),
	OPT_1 = (1 << 1),
	OPT_v = (1 << 2),
	OPT_S = (1 << 3),
	OPT_w = (1 << 4),
};

int ftpd_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int ftpd_main(int argc, char **argv)
{
	smallint opts;

	opts = getopt32(argv, "l1vS" USE_FEATURE_FTP_WRITE("w"));

	if (opts & (OPT_l|OPT_1)) {
		/* Our secret backdoor to ls */
/* TODO: pass -n too? */
		xchdir(argv[2]);
		argv[2] = (char*)"--";
		return ls_main(argc, argv);
	}

	INIT_G();

	G.local_addr = get_sock_lsa(STDIN_FILENO);
	if (!G.local_addr) {
		/* This is confusing:
		 * bb_error_msg_and_die("stdin is not a socket");
		 * Better: */
		bb_show_usage();
		/* Help text says that ftpd must be used as inetd service,
		 * which is by far the most usual cause of get_sock_lsa
		 * failure */
	}

	if (!(opts & OPT_v))
		logmode = LOGMODE_NONE;
	if (opts & OPT_S) {
		/* LOG_NDELAY is needed since we may chroot later */
		openlog(applet_name, LOG_PID | LOG_NDELAY, LOG_DAEMON);
		logmode |= LOGMODE_SYSLOG;
	}

	G.proc_self_fd = xopen("/proc/self", O_RDONLY | O_DIRECTORY);

	if (argv[optind]) {
		xchdir(argv[optind]);
		chroot(".");
	}

	//umask(077); - admin can set umask before starting us

	/* Signals. We'll always take -EPIPE rather than a rude signal, thanks */
	signal(SIGPIPE, SIG_IGN);

	/* Set up options on the command socket (do we need these all? why?) */
	setsockopt(STDIN_FILENO, IPPROTO_TCP, TCP_NODELAY, &const_int_1, sizeof(const_int_1));
	setsockopt(STDIN_FILENO, SOL_SOCKET, SO_KEEPALIVE, &const_int_1, sizeof(const_int_1));
	setsockopt(STDIN_FILENO, SOL_SOCKET, SO_OOBINLINE, &const_int_1, sizeof(const_int_1));

	cmdio_write_ok(FTP_GREET);

#ifdef IF_WE_WANT_TO_REQUIRE_LOGIN
	{
		smallint user_was_specified = 0;
		while (1) {
			uint32_t cmdval = cmdio_get_cmd_and_arg();

			if (cmdval == const_USER) {
				if (G.ftp_arg == NULL || strcasecmp(G.ftp_arg, "anonymous") != 0)
					cmdio_write_raw(STR(FTP_LOGINERR)" Server is anonymous only\r\n");
				else {
					user_was_specified = 1;
					cmdio_write_raw(STR(FTP_GIVEPWORD)" Please specify the password\r\n");
				}
			} else if (cmdval == const_PASS) {
				if (user_was_specified)
					break;
				cmdio_write_raw(STR(FTP_NEEDUSER)" Login with USER\r\n");
			} else if (cmdval == const_QUIT) {
				cmdio_write_ok(FTP_GOODBYE);
				return 0;
			} else {
				cmdio_write_raw(STR(FTP_LOGINERR)" Login with USER and PASS\r\n");
			}
		}
	}
	cmdio_write_ok(FTP_LOGINOK);
#endif

	/* RFC-959 Section 5.1
	 * The following commands and options MUST be supported by every
	 * server-FTP and user-FTP, except in cases where the underlying
	 * file system or operating system does not allow or support
	 * a particular command.
	 * Type: ASCII Non-print, IMAGE, LOCAL 8
	 * Mode: Stream
	 * Structure: File, Record*
	 * (Record structure is REQUIRED only for hosts whose file
	 *  systems support record structure).
	 * Commands:
	 * USER, PASS, ACCT, [bbox: ACCT not supported]
	 * PORT, PASV,
	 * TYPE, MODE, STRU,
	 * RETR, STOR, APPE,
	 * RNFR, RNTO, DELE,
	 * CWD,  CDUP, RMD,  MKD,  PWD,
	 * LIST, NLST,
	 * SYST, STAT,
	 * HELP, NOOP, QUIT.
	 */
	/* ACCOUNT (ACCT)
	 * "The argument field is a Telnet string identifying the user's account.
	 * The command is not necessarily related to the USER command, as some
	 * sites may require an account for login and others only for specific
	 * access, such as storing files. In the latter case the command may
	 * arrive at any time.
	 * There are reply codes to differentiate these cases for the automation:
	 * when account information is required for login, the response to
	 * a successful PASSword command is reply code 332. On the other hand,
	 * if account information is NOT required for login, the reply to
	 * a successful PASSword command is 230; and if the account information
	 * is needed for a command issued later in the dialogue, the server
	 * should return a 332 or 532 reply depending on whether it stores
	 * (pending receipt of the ACCounT command) or discards the command,
	 * respectively."
	 */

	while (1) {
		uint32_t cmdval = cmdio_get_cmd_and_arg();

		if (cmdval == const_QUIT) {
			cmdio_write_ok(FTP_GOODBYE);
			return 0;
		}
		else if (cmdval == const_USER)
			cmdio_write_ok(FTP_GIVEPWORD);
		else if (cmdval == const_PASS)
			cmdio_write_ok(FTP_LOGINOK);
		else if (cmdval == const_NOOP)
			cmdio_write_ok(FTP_NOOPOK);
		else if (cmdval == const_TYPE)
			cmdio_write_ok(FTP_TYPEOK);
		else if (cmdval == const_STRU)
			cmdio_write_ok(FTP_STRUOK);
		else if (cmdval == const_MODE)
			cmdio_write_ok(FTP_MODEOK);
		else if (cmdval == const_ALLO)
			cmdio_write_ok(FTP_ALLOOK);
		else if (cmdval == const_SYST)
			cmdio_write_raw(STR(FTP_SYSTOK)" UNIX Type: L8\r\n");
		else if (cmdval == const_PWD)
			handle_pwd();
		else if (cmdval == const_CWD)
			handle_cwd();
		else if (cmdval == const_CDUP) /* cd .. */
			handle_cdup();
		else if (cmdval == const_HELP)
			handle_help();
		else if (cmdval == const_LIST) /* ls -l */
			handle_list();
		else if (cmdval == const_NLST) /* "name list", bare ls */
			handle_nlst();
		else if (cmdval == const_SIZE)
			handle_size();
		else if (cmdval == const_STAT) {
			if (G.ftp_arg == NULL)
				handle_stat();
			else
				handle_stat_file();
		}
		else if (cmdval == const_PASV)
			handle_pasv();
		else if (cmdval == const_EPSV)
			handle_epsv();
		else if (cmdval == const_RETR)
			handle_retr();
		else if (cmdval == const_PORT)
			handle_port();
		else if (cmdval == const_REST)
			handle_rest();
#if ENABLE_FEATURE_FTP_WRITE
		else if (opts & OPT_w) {
			if (cmdval == const_STOR)
				handle_stor();
			else if (cmdval == const_MKD)
				handle_mkd();
			else if (cmdval == const_RMD)
				handle_rmd();
			else if (cmdval == const_DELE)
				handle_dele();
			else if (cmdval == const_RNFR) /* "rename from" */
				handle_rnfr();
			else if (cmdval == const_RNTO) /* "rename to" */
				handle_rnto();
			else if (cmdval == const_APPE)
				handle_appe();
			else if (cmdval == const_STOU) /* "store unique" */
				handle_stou();
		}
#endif
#if 0
		else if (cmdval == const_STOR
		 || cmdval == const_MKD
		 || cmdval == const_RMD
		 || cmdval == const_DELE
		 || cmdval == const_RNFR
		 || cmdval == const_RNTO
		 || cmdval == const_APPE
		 || cmdval == const_STOU
		) {
			cmdio_write_raw(STR(FTP_NOPERM)" Permission denied\r\n");
		}
#endif
		else {
			/* Which unsupported commands were seen in the wild?
			 * (doesn't necessarily mean "we must support them")
			 * lftp 3.6.3: FEAT - is it useful?
			 *             MDTM - works fine without it anyway
			 */
			cmdio_write_raw(STR(FTP_BADCMD)" Unknown command\r\n");
		}
	}
}