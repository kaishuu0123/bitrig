/*	$OpenBSD: server.c,v 1.22 2003/04/06 17:57:45 ho Exp $	*/

/*
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef lint
/* from: static char sccsid[] = "@(#)server.c	8.1 (Berkeley) 6/9/93"; */
static char *rcsid = "$OpenBSD: server.c,v 1.22 2003/04/06 17:57:45 ho Exp $";
#endif /* not lint */

#include <sys/wait.h>

#include <stdarg.h>
#include <libgen.h>

#include "defs.h"

#define	ack() 	(void) write(rem, "\0\n", 2)
#define	err() 	(void) write(rem, "\1\n", 2)

struct	linkbuf *ihead;		/* list of files with more than one link */
char	buf[BUFSIZ];		/* general purpose buffer */
char	target[BUFSIZ];		/* target/source directory name */
char	source[BUFSIZ];		/* source directory name */
char	*tp;			/* pointer to end of target name */
char	*Tdest;			/* pointer to last T dest*/
int	catname;		/* cat name to target name */
char	*stp[32];		/* stack of saved tp's for directories */
int	oumask;			/* old umask for creating files */

extern	FILE *lfp;		/* log file for mailing changes */

static int	chkparent(char *);
static void	clean(char *);
static void	comment(char *);
static void	dospecial(char *);
static int	fchog(int, char *, char *, char *, int);
static void	hardlink(char *);
static void	note(const char *, ...);
static void	query(char *);
static void	recvf(char *, int);
static void	removeit(struct stat *);
static int	response(void);
static void	rmchk(int);
static struct linkbuf *
		    savelink(struct stat *);
static void	sendf(char *, int);
static int	update(char *, int, struct stat *);

/*
 * Server routine to read requests and process them.
 * Commands are:
 *	Tname	- Transmit file if out of date
 *	Vname	- Verify if file out of date or not
 *	Qname	- Query if file exists. Return mtime & size if it does.
 */
void
server()
{
	char cmdbuf[BUFSIZ];
	char *cp;

	signal(SIGHUP, cleanup);
	signal(SIGINT, cleanup);
	signal(SIGQUIT, cleanup);
	signal(SIGTERM, cleanup);
	signal(SIGPIPE, cleanup);

	rem = 0;
	oumask = umask(0);
	(void) snprintf(buf, sizeof(buf), "V%d\n", VERSION);
	(void) write(rem, buf, strlen(buf));

#if	!defined(DIRECT_RCMD)
	if (getuid() != geteuid()) {
		error("This version of rdist should not be installed setuid.\n");
		return;
	}
#endif	/* DIRECT_RCMD */

	for (;;) {
		cp = cmdbuf;
		if (read(rem, cp, 1) <= 0)
			return;
		if (*cp++ == '\n') {
			error("server: expected control record\n");
			continue;
		}
		do {
			if (read(rem, cp, 1) != 1)
				cleanup(0);
		} while (*cp++ != '\n' && cp < &cmdbuf[BUFSIZ]);
		*--cp = '\0';
		cp = cmdbuf;
		switch (*cp++) {
		case 'T':  /* init target file/directory name */
			catname = 1;	/* target should be directory */
			goto dotarget;

		case 't':  /* init target file/directory name */
			catname = 0;
		dotarget:
			if (exptilde(target, cp, sizeof (target)) == NULL)
				continue;
			tp = target;
			while (*tp)
				tp++;
			ack();
			continue;

		case 'R':  /* Transfer a regular file. */
			recvf(cp, S_IFREG);
			continue;

		case 'D':  /* Transfer a directory. */
			recvf(cp, S_IFDIR);
			continue;

		case 'K':  /* Transfer symbolic link. */
			recvf(cp, S_IFLNK);
			continue;

		case 'k':  /* Transfer hard link. */
			hardlink(cp);
			continue;

		case 'E':  /* End. (of directory) */
			*tp = '\0';
			if (catname <= 0) {
				error("server: too many 'E's\n");
				continue;
			}
			tp = stp[--catname];
			*tp = '\0';
			ack();
			continue;

		case 'C':  /* Clean. Cleanup a directory */
			clean(cp);
			continue;

		case 'Q':  /* Query. Does the file/directory exist? */
			query(cp);
			continue;

		case 'S':  /* Special. Execute commands */
			dospecial(cp);
			continue;

#ifdef notdef
		/*
		 * These entries are reserved but not currently used.
		 * The intent is to allow remote hosts to have master copies.
		 * Currently, only the host rdist runs on can have masters.
		 */
		case 'X':  /* start a new list of files to exclude */
			except = bp = NULL;
		case 'x':  /* add name to list of files to exclude */
			if (*cp == '\0') {
				ack();
				continue;
			}
			if (*cp == '~') {
				if (exptilde(buf, cp, sizeof (buf)) == NULL)
					continue;
				cp = buf;
			}
			if (bp == NULL)
				except = bp = expand(makeblock(NAME, cp), E_VARS);
			else
				bp->b_next = expand(makeblock(NAME, cp), E_VARS);
			while (bp->b_next != NULL)
				bp = bp->b_next;
			ack();
			continue;

		case 'I':  /* Install. Transfer file if out of date. */
			opts = 0;
			while (*cp >= '0' && *cp <= '7')
				opts = (opts << 3) | (*cp++ - '0');
			if (*cp++ != ' ') {
				error("server: options not delimited\n");
				return;
			}
			install(cp, opts);
			continue;

		case 'L':  /* Log. save message in log file */
			log(lfp, cp);
			continue;
#endif

		case '\1':
			nerrs++;
			continue;

		case '\2':
			return;

		default:
			error("server: unknown command '%s'\n", cp);
		case '\0':
			continue;
		}
	}
}

/*
 * Update the file(s) if they are different.
 * destdir = 1 if destination should be a directory
 * (i.e., more than one source is being copied to the same destination).
 */
void
install(src, dest, destdir, opts)
	char *src, *dest;
	int destdir, opts;
{
	char *rname;
	char destcopy[BUFSIZ];

	if (opts & WHOLE)
		source[0] = '\0';
	else
		strlcpy(source, src, sizeof source);

	if (dest == NULL) {
		opts &= ~WHOLE; /* WHOLE mode only useful if renaming */
		dest = src;
	}

	if (nflag || debug) {
		printf("%s%s%s%s%s %s %s\n", opts & VERIFY ? "verify":"install",
			opts & WHOLE ? " -w" : "",
			opts & YOUNGER ? " -y" : "",
			opts & COMPARE ? " -b" : "",
			opts & REMOVE ? " -R" : "", src, dest);
		if (nflag)
			return;
	}

	rname = exptilde(target, src, sizeof(target));
	if (rname == NULL)
		return;
	tp = target;
	while (*tp)
		tp++;
	/*
	 * If we are renaming a directory and we want to preserve
	 * the directory heirarchy (-w), we must strip off the leading
	 * directory name and preserve the rest.
	 */
	if (opts & WHOLE) {
		while (*rname == '/')
			rname++;
		destdir = 1;
	} else {
		rname = basename(target);
	}
	if (debug)
		printf("target = %s, rname = %s\n", target, rname);
	/*
	 * Pass the destination file/directory name to remote.
	 */
	(void) snprintf(buf, sizeof(buf), "%c%s\n", destdir ? 'T' : 't', dest);
	if (debug)
		printf("buf = %s", buf);
	(void) write(rem, buf, strlen(buf));
	if (response() < 0)
		return;

	/*
	 * Save the name of the remote target destination if we are
	 * in WHOLE mode (destdir > 0) or if the source and destination
	 * are not the same.  This info will be used later for maintaining
	 * hardlink info.
	 */
	if (destdir || (src && dest && strcmp(src, dest))) {
		strlcpy(destcopy, dest, sizeof destcopy);
		Tdest = destcopy;
	}
	sendf(rname, opts);
	Tdest = 0;
}

static char *
remotename(pathname, src)
	char *pathname;
	char *src;
{
	char *cp;
	int len;

	cp = pathname;
	len = strlen(src);
	if (0 == strncmp(pathname, src, len))
	        cp += len;
	if (*cp == '/')
	        cp ++;
	return(cp);
}

void
installlink(lp, rname, opts)
	struct linkbuf *lp;
	char *rname;
	int opts;
{
	if (*lp->target == 0)
		(void) snprintf(buf, sizeof(buf), "k%o %s %s\n",
		    opts, lp->pathname, rname);
	else
		(void) snprintf(buf, sizeof(buf), "k%o %s/%s %s\n",
		    opts, lp->target,
		    remotename(lp->pathname, lp->src), rname);

	if (debug) {
	        printf("lp->src      = %s\n", lp->src);
	        printf("lp->target   = %s\n", lp->target);
	        printf("lp->pathname = %s\n", lp->pathname);
	        printf("rname        = %s\n", rname);
	        printf("buf          = %s",   buf);
	}
	(void) write(rem, buf, strlen(buf));
	(void) response();
}

#define protoname() (pw ? pw->pw_name : user)
#define protogroup() (gr ? gr->gr_name : group)
/*
 * Transfer the file or directory in target[].
 * rname is the name of the file on the remote host.
 */
static void
sendf(rname, opts)
	char *rname;
	int opts;
{
	struct subcmd *sc;
	struct stat stb;
	int sizerr, f, u, len;
	off_t i;
	DIR *d;
	struct direct *dp;
	char *otp, *cp;
	extern struct subcmd *subcmds;
	static char user[15], group[15];

	if (debug)
		printf("sendf(%s, %x)\n", rname, opts);

	if (except(target))
		return;
	if ((opts & FOLLOW ? stat(target, &stb) : lstat(target, &stb)) < 0) {
		error("%s: %s\n", target, strerror(errno));
		return;
	}
	if ((u = update(rname, opts, &stb)) == 0) {
		if ((stb.st_mode & S_IFMT) == S_IFREG && stb.st_nlink > 1)
			(void) savelink(&stb);
		return;
	}

	if (pw == NULL || pw->pw_uid != stb.st_uid)
		if ((pw = getpwuid(stb.st_uid)) == NULL) {
			log(lfp, "%s: no password entry for uid %u \n",
				target, stb.st_uid);
			pw = NULL;
			(void) snprintf(user, sizeof(user), ":%lu", stb.st_uid);
		}
	if (gr == NULL || gr->gr_gid != stb.st_gid)
		if ((gr = getgrgid(stb.st_gid)) == NULL) {
			log(lfp, "%s: no name for group %u\n",
				target, stb.st_gid);
			gr = NULL;
			(void) snprintf(group, sizeof(group), ":%lu",
				stb.st_gid);
		}
	if (u == 1) {
		if (opts & VERIFY) {
			log(lfp, "need to install: %s\n", target);
			goto dospecial;
		}
		log(lfp, "installing: %s\n", target);
		opts &= ~(COMPARE|REMOVE);
	}

	switch (stb.st_mode & S_IFMT) {
	case S_IFDIR:
		if ((d = opendir(target)) == NULL) {
			error("%s: %s\n", target, strerror(errno));
			return;
		}
		(void) snprintf(buf, sizeof(buf), "D%o %04o 0 0 %s %s %s\n",
			opts, stb.st_mode & 07777, protoname(), protogroup(),
			rname);
		if (debug)
			printf("buf = %s", buf);
		(void) write(rem, buf, strlen(buf));
		if (response() < 0) {
			closedir(d);
			return;
		}

		if (opts & REMOVE)
			rmchk(opts);

		otp = tp;
		len = tp - target;
		while (dp = readdir(d)) {
			if (!strcmp(dp->d_name, ".") ||
			    !strcmp(dp->d_name, ".."))
				continue;
			if (len + 1 + strlen(dp->d_name) >= BUFSIZ - 1) {
				error("%s/%s: Name too long\n", target,
					dp->d_name);
				continue;
			}
			tp = otp;
			*tp++ = '/';
			cp = dp->d_name;
			while (*tp++ = *cp++)
				;
			tp--;
			sendf(dp->d_name, opts);
		}
		closedir(d);
		(void) write(rem, "E\n", 2);
		(void) response();
		tp = otp;
		*tp = '\0';
		return;

	case S_IFLNK:
		if (u != 1)
			opts |= COMPARE;
		if (stb.st_nlink > 1) {
			struct linkbuf *lp;

			if ((lp = savelink(&stb)) != NULL) {
				installlink(lp, rname, opts);
				return;
			}
		}
		(void) snprintf(buf, sizeof(buf), "K%o %o %qd %ld %s %s %s\n",
			opts, stb.st_mode & 07777, stb.st_size, stb.st_mtime,
			protoname(), protogroup(), rname);
		if (debug)
			printf("buf = %s", buf);
		(void) write(rem, buf, strlen(buf));
		if (response() < 0)
			return;
		sizerr = (readlink(target, buf, BUFSIZ-1) != stb.st_size);
		(void) write(rem, buf, stb.st_size);
		if (debug)
			printf("readlink = %.*s\n", (int)stb.st_size, buf);
		goto done;

	case S_IFREG:
		break;

	default:
		error("%s: not a file or directory\n", target);
		return;
	}

	if (u == 2) {
		if (opts & VERIFY) {
			log(lfp, "need to update: %s\n", target);
			goto dospecial;
		}
		log(lfp, "updating: %s\n", target);
	}

	if (stb.st_nlink > 1) {
		struct linkbuf *lp;

		if ((lp = savelink(&stb)) != NULL) {
			installlink(lp, rname, opts);
			return;
		}
	}

	if ((f = open(target, O_RDONLY)) < 0) {
		error("%s: %s\n", target, strerror(errno));
		return;
	}
	(void) snprintf(buf, sizeof(buf), "R%o %o %qd %ld %s %s %s\n", opts,
		stb.st_mode & 07777, stb.st_size, stb.st_mtime,
		protoname(), protogroup(), rname);
	if (debug)
		printf("buf = %s", buf);
	(void) write(rem, buf, strlen(buf));
	if (response() < 0) {
		(void) close(f);
		return;
	}
	sizerr = 0;
	for (i = 0; i < stb.st_size; i += BUFSIZ) {
		int amt = BUFSIZ;
		if (i + amt > stb.st_size)
			amt = stb.st_size - i;
		if (sizerr == 0 && read(f, buf, amt) != amt)
			sizerr = 1;
		(void) write(rem, buf, amt);
	}
	(void) close(f);
done:
	if (sizerr) {
		error("%s: file changed size\n", target);
		err();
	} else
		ack();
	f = response();
	if (f < 0 || f == 0 && (opts & COMPARE))
		return;
dospecial:
	for (sc = subcmds; sc != NULL; sc = sc->sc_next) {
		if (sc->sc_type != SPECIAL)
			continue;
		if (sc->sc_args != NULL && !inlist(sc->sc_args, target))
			continue;
		log(lfp, "special \"%s\"\n", sc->sc_name);
		if (opts & VERIFY)
			continue;
		(void) snprintf(buf, sizeof(buf), "SFILE=%s;%s\n", target,
			sc->sc_name);
		if (debug)
			printf("buf = %s", buf);
		(void) write(rem, buf, strlen(buf));
		while (response() > 0)
			;
	}
}

static struct linkbuf *
savelink(stp)
	struct stat *stp;
{
	struct linkbuf *lp;

	for (lp = ihead; lp != NULL; lp = lp->nextp)
		if (lp->inum == stp->st_ino && lp->devnum == stp->st_dev) {
			lp->count--;
			return(lp);
		}
	lp = (struct linkbuf *) malloc(sizeof(*lp));
	if (lp == NULL)
		log(lfp, "out of memory, link information lost\n");
	else {
		lp->nextp = ihead;
		ihead = lp;
		lp->inum = stp->st_ino;
		lp->devnum = stp->st_dev;
		lp->count = stp->st_nlink - 1;
		strlcpy(lp->pathname, target, sizeof lp->pathname);
		strlcpy(lp->src, source, sizeof lp->src);
		if (Tdest)
			strlcpy(lp->target, Tdest, sizeof lp->target);
		else
			*lp->target = 0;
	}
	return(NULL);
}

/*
 * Check to see if file needs to be updated on the remote machine.
 * Returns 0 if no update, 1 if remote doesn't exist, 2 if out of date
 * and 3 if comparing binaries to determine if out of date.
 */
static int
update(rname, opts, stp)
	char *rname;
	int opts;
	struct stat *stp;
{
	char *cp, *s;
	off_t size;
	time_t mtime;

	if (debug) 
		printf("update(%s, %x, %x)\n", rname, opts, stp);

	/*
	 * Check to see if the file exists on the remote machine.
	 */
	(void) snprintf(buf, sizeof(buf), "Q%s\n", rname);
	if (debug)
		printf("buf = %s", buf);
	(void) write(rem, buf, strlen(buf));
again:
	cp = s = buf;
	do {
		if (read(rem, cp, 1) != 1)
			lostconn(0);
	} while (*cp++ != '\n' && cp < &buf[BUFSIZ]);

	switch (*s++) {
	case 'Y':
		break;

	case 'N':  /* file doesn't exist so install it */
		return(1);

	case '\1':
		nerrs++;
		if (*s != '\n') {
			if (!iamremote) {
				fflush(stdout);
				(void) write(2, s, cp - s);
			}
			if (lfp != NULL)
				(void) fwrite(s, 1, cp - s, lfp);
		}
		return(0);

	case '\3':
		*--cp = '\0';
		if (lfp != NULL) 
			log(lfp, "update: note: %s\n", s);
		goto again;

	default:
		*--cp = '\0';
		error("update: unexpected response '%s'\n", s);
		return(0);
	}

	if (*s == '\n')
		return(2);

	if (opts & COMPARE)
		return(3);

	size = 0;
	while (isdigit(*s))
		size = size * 10 + (*s++ - '0');
	if (*s++ != ' ') {
		error("update: size not delimited\n");
		return(0);
	}
	mtime = 0;
	while (isdigit(*s))
		mtime = mtime * 10 + (*s++ - '0');
	if (*s != '\n') {
		error("update: mtime not delimited\n");
		return(0);
	}
	/*
	 * File needs to be updated?
	 */
	if (opts & YOUNGER) {
		if (stp->st_mtime == mtime)
			return(0);
		if (stp->st_mtime < mtime) {
			log(lfp, "Warning: %s: remote copy is newer\n", target);
			return(0);
		}
	} else if (stp->st_mtime == mtime && stp->st_size == size)
		return(0);
	return(2);
}

/*
 * Query. Check to see if file exists. Return one of the following:
 *	N\n		- doesn't exist
 *	Ysize mtime\n	- exists and its a regular file (size & mtime of file)
 *	Y\n		- exists and its a directory or symbolic link
 *	^Aerror message\n
 */
static void
query(name)
	char *name;
{
	struct stat stb;

	if (catname)
		(void) snprintf(tp, sizeof(target) - (tp - target),
			"/%s", name);

	if (lstat(target, &stb) < 0) {
		if (errno == ENOENT)
			(void) write(rem, "N\n", 2);
		else
			error("%s:%s: %s\n", host, target, strerror(errno));
		*tp = '\0';
		return;
	}

	switch (stb.st_mode & S_IFMT) {
	case S_IFREG:
		(void) snprintf(buf, sizeof(buf), "Y%qd %ld\n", stb.st_size,
			stb.st_mtime);
		(void) write(rem, buf, strlen(buf));
		break;

	case S_IFLNK:
	case S_IFDIR:
		(void) write(rem, "Y\n", 2);
		break;

	default:
		error("%s: not a file or directory\n", name);
		break;
	}
	*tp = '\0';
}

static void
recvf(cmd, type)
	char *cmd;
	int type;
{
	char *cp = cmd;
	int f = -1, mode, opts = 0, wrerr, olderrno;
	off_t i, size;
	time_t mtime;
	struct stat stb;
	struct timeval tvp[2];
	char *owner, *group;
	char new[BUFSIZ];
	extern char *tempname;

	while (*cp >= '0' && *cp <= '7')
		opts = (opts << 3) | (*cp++ - '0');
	if (*cp++ != ' ') {
		error("recvf: options not delimited\n");
		return;
	}
	mode = 0;
	while (*cp >= '0' && *cp <= '7')
		mode = (mode << 3) | (*cp++ - '0');
	if (*cp++ != ' ') {
		error("recvf: mode not delimited\n");
		return;
	}
	size = 0;
	while (isdigit(*cp))
		size = size * 10 + (*cp++ - '0');
	if (*cp++ != ' ') {
		error("recvf: size not delimited\n");
		return;
	}
	mtime = 0;
	while (isdigit(*cp))
		mtime = mtime * 10 + (*cp++ - '0');
	if (*cp++ != ' ') {
		error("recvf: mtime not delimited\n");
		return;
	}
	owner = cp;
	while (*cp && *cp != ' ')
		cp++;
	if (*cp != ' ') {
		error("recvf: owner name not delimited\n");
		return;
	}
	*cp++ = '\0';
	group = cp;
	while (*cp && *cp != ' ')
		cp++;
	if (*cp != ' ') {
		error("recvf: group name not delimited\n");
		return;
	}
	*cp++ = '\0';

	if (type == S_IFDIR) {
		if (catname >= sizeof(stp)) {
			error("%s:%s: too many directory levels\n",
				host, target);
			return;
		}
		stp[catname] = tp;
		if (catname++) {
			*tp++ = '/';
			while (*tp++ = *cp++)
				;
			tp--;
		}
		if (opts & VERIFY) {
			ack();
			return;
		}
		if (lstat(target, &stb) == 0) {
			if (ISDIR(stb.st_mode)) {
				if ((stb.st_mode & 07777) == mode) {
					ack();
					return;
				}
				buf[0] = '\0';
				(void) snprintf(buf + 1, sizeof(buf) - 1,
					"%s: Warning: remote mode %o != local mode %o\n",
					target, stb.st_mode & 07777, mode);
				(void) write(rem, buf, strlen(buf + 1) + 1);
				return;
			}
			errno = ENOTDIR;
		} else if (errno == ENOENT && (mkdir(target, mode) == 0 ||
		    chkparent(target) == 0 && mkdir(target, mode) == 0)) {
			if (fchog(-1, target, owner, group, mode) == 0)
				ack();
			return;
		}
		error("%s:%s: %s\n", host, target, strerror(errno));
		tp = stp[--catname];
		*tp = '\0';
		return;
	}

	if (catname)
		(void) snprintf(tp, sizeof(target) - (tp - target), "/%s", cp);
	cp = strrchr(target, '/');
	if (cp == NULL)
		strlcpy(new, tempname, sizeof new);
	else if (cp == target)
		(void) snprintf(new, sizeof(new), "/%s", tempname);
	else {
		*cp = '\0';
		(void) snprintf(new, sizeof(new), "%s/%s", target, tempname);
		*cp = '/';
	}

	if (type == S_IFLNK) {
		int j;

		ack();
		cp = buf;
		for (i = 0; i < size; i += j) {
			if ((j = read(rem, cp, size - i)) <= 0)
				cleanup(0);
			cp += j;
		}
		*cp = '\0';
		if (response() < 0) {
			err();
			return;
		}
		if (symlink(buf, new) < 0) {
			if (errno != ENOENT || chkparent(new) < 0 ||
			    symlink(buf, new) < 0)
				goto badnew1;
		}
		mode &= 0777;
		if (opts & COMPARE) {
			char tbuf[BUFSIZ];

			if ((i = readlink(target, tbuf, BUFSIZ)) >= 0 &&
			    i == size && strncmp(buf, tbuf, size) == 0) {
				(void) unlink(new);
				ack();
				return;
			}
			if (opts & VERIFY)
				goto differ;
		}
		goto fixup;
	}

	if ((f = creat(new, mode)) < 0) {
		if (errno != ENOENT || chkparent(new) < 0 ||
		    (f = creat(new, mode)) < 0)
			goto badnew1;
	}

	ack();
	wrerr = 0;
	for (i = 0; i < size; i += BUFSIZ) {
		int amt = BUFSIZ;

		cp = buf;
		if (i + amt > size)
			amt = size - i;
		do {
			int j = read(rem, cp, amt);

			if (j <= 0) {
				(void) close(f);
				(void) unlink(new);
				cleanup(0);
			}
			amt -= j;
			cp += j;
		} while (amt > 0);
		amt = BUFSIZ;
		if (i + amt > size)
			amt = size - i;
		if (wrerr == 0 && write(f, buf, amt) != amt) {
			olderrno = errno;
			wrerr++;
		}
	}
	if (response() < 0) {
		err();
		goto badnew2;
	}
	if (wrerr)
		goto badnew1;
	if (opts & COMPARE) {
		FILE *f1, *f2;
		int c;

		if ((f1 = fopen(target, "r")) == NULL)
			goto badtarget;
		if ((f2 = fopen(new, "r")) == NULL) {
badnew1:		error("%s:%s: %s\n", host, new, strerror(errno));
			goto badnew2;
		}
		while ((c = getc(f1)) == getc(f2))
			if (c == EOF) {
				(void) fclose(f1);
				(void) fclose(f2);
				ack();
				goto badnew2;
			}
		(void) fclose(f1);
		(void) fclose(f2);
		if (opts & VERIFY) {
differ:			buf[0] = '\0';
			(void) snprintf(buf + 1, sizeof(buf) - 1,
				"need to update: %s\n",target);
			(void) write(rem, buf, strlen(buf + 1) + 1);
			goto badnew2;
		}
	}

	/*
	 * Set last modified time
	 */
	tvp[0].tv_sec = time(0);
	tvp[0].tv_usec = 0;
	tvp[1].tv_sec = mtime;
	tvp[1].tv_usec = 0;
	if (utimes(new, tvp) < 0)
		note("%s: utimes failed %s: %s\n", host, new, strerror(errno));

	if (fchog(f, new, owner, group, mode) < 0) {
badnew2:	
		if (f >= 0)
			(void) close(f);
		(void) unlink(new);
		return;
	}
	(void) close(f);

fixup:	if (rename(new, target) < 0) {
badtarget:	error("%s:%s: %s\n", host, target, strerror(errno));
		(void) unlink(new);
		return;
	}

	if (opts & COMPARE) {
		buf[0] = '\0';
		(void) snprintf(buf + 1, sizeof(buf) - 1,
			"updated %s\n", target);
		(void) write(rem, buf, strlen(buf + 1) + 1);
	} else
		ack();
}

/*
 * Creat a hard link to existing file.
 */
static void
hardlink(cmd)
	char *cmd;
{
	char *cp = cmd;
	struct stat stb;
	char *oldname;
	int opts = 0, exists = 0;

	while (*cp >= '0' && *cp <= '7')
		opts = (opts << 3) | (*cp++ - '0');
	if (*cp++ != ' ') {
		error("hardlink: options not delimited\n");
		return;
	}
	oldname = cp;
	while (*cp && *cp != ' ')
		cp++;
	if (*cp != ' ') {
		error("hardlink: oldname name not delimited\n");
		return;
	}
	*cp++ = '\0';

	if (catname) {
		(void) snprintf(tp, sizeof(target) - (tp - target), "/%s", cp);
	}
	if (lstat(target, &stb) == 0) {
		int mode = stb.st_mode & S_IFMT;
		if (mode != S_IFREG && mode != S_IFLNK) {
			error("%s:%s: not a regular file\n", host, target);
			return;
		}
		exists = 1;
	}
	if (chkparent(target) < 0 ) {
		error("%s:%s: %s (no parent)\n",
			host, target, strerror(errno));
		return;
	}
	if (exists && (unlink(target) < 0)) {
		error("%s:%s: %s (unlink)\n",
			host, target, strerror(errno));
		return;
	}
	if (link(oldname, target) < 0) {
		error("%s:can't link %s to %s\n",
			host, target, oldname);
		return;
	}
	ack();
}

/*
 * Check to see if parent directory exists and create one if not.
 */
static int
chkparent(name)
	char *name;
{
	char *cp;
	struct stat stb;

	cp = strrchr(name, '/');
	if (cp == NULL || cp == name)
		return(0);
	*cp = '\0';
	if (lstat(name, &stb) < 0) {
		if (errno == ENOENT && chkparent(name) >= 0 &&
		    mkdir(name, 0777 & ~oumask) >= 0) {
			*cp = '/';
			return(0);
		}
	} else if (ISDIR(stb.st_mode)) {
		*cp = '/';
		return(0);
	}
	*cp = '/';
	return(-1);
}

/*
 * Change owner, group and mode of file.
 */
static int
fchog(fd, file, owner, group, mode)
	int fd;
	char *file, *owner, *group;
	int mode;
{
	int i;
	uid_t uid;
	gid_t gid;
	extern char user[];
	extern uid_t userid;

	uid = userid;
	if (userid == 0) {
		if (*owner == ':') {
			uid = atoi(owner + 1);
		} else if (pw == NULL || strcmp(owner, pw->pw_name) != 0) {
			if ((pw = getpwnam(owner)) == NULL) {
				if (mode & 04000) {
					note("%s:%s: unknown login name, clearing setuid",
						host, owner);
					mode &= ~04000;
					uid = 0;
				}
			} else
				uid = pw->pw_uid;
		} else
			uid = pw->pw_uid;
		if (*group == ':') {
			gid = atoi(group + 1);
			goto ok;
		}
	} else if ((mode & 04000) && strcmp(user, owner) != 0)
		mode &= ~04000;
	gid = -1;
	if (gr == NULL || strcmp(group, gr->gr_name) != 0) {
		if ((*group == ':' && (getgrgid(gid = atoi(group + 1)) == NULL))
		   || ((gr = getgrnam(group)) == NULL)) {
			if (mode & 02000) {
				note("%s:%s: unknown group", host, group);
				mode &= ~02000;
			}
		} else
			gid = gr->gr_gid;
	} else
		gid = gr->gr_gid;
	if (userid && gid >= 0) {
		if (gr) for (i = 0; gr->gr_mem[i] != NULL; i++)
			if (!(strcmp(user, gr->gr_mem[i])))
				goto ok;
		mode &= ~02000;
		gid = -1;
	}
ok:	if (fd != -1 && fchown(fd, uid, gid) < 0 || chown(file, uid, gid) < 0)
		note("%s: %s chown: %s", host, file, strerror(errno));
	else if (mode & 07000 &&
	   (fd != -1 && fchmod(fd, mode) < 0 || chmod(file, mode) < 0))
		note("%s: %s chmod: %s", host, file, strerror(errno));
	return(0);
}

/*
 * Check for files on the machine being updated that are not on the master
 * machine and remove them.
 */
static void
rmchk(opts)
	int opts;
{
	char *cp, *s;
	struct stat stb;

	if (debug)
		printf("rmchk()\n");

	/*
	 * Tell the remote to clean the files from the last directory sent.
	 */
	(void) snprintf(buf, sizeof(buf), "C%o\n", opts & VERIFY);
	if (debug)
		printf("buf = %s", buf);
	(void) write(rem, buf, strlen(buf));
	if (response() < 0)
		return;
	for (;;) {
		cp = s = buf;
		do {
			if (read(rem, cp, 1) != 1)
				lostconn(0);
		} while (*cp++ != '\n' && cp < &buf[BUFSIZ]);

		switch (*s++) {
		case 'Q': /* Query if file should be removed */
			/*
			 * Return the following codes to remove query.
			 * N\n -- file exists - DON'T remove.
			 * Y\n -- file doesn't exist - REMOVE.
			 */
			*--cp = '\0';
			(void) snprintf(tp, sizeof(target) - (tp - target),
				"/%s", s);
			if (debug)
				printf("check %s\n", target);
			if (except(target))
				(void) write(rem, "N\n", 2);
			else if (lstat(target, &stb) < 0)
				(void) write(rem, "Y\n", 2);
			else
				(void) write(rem, "N\n", 2);
			break;

		case '\0':
			*--cp = '\0';
			if (*s != '\0')
				log(lfp, "%s\n", s);
			break;

		case 'E':
			*tp = '\0';
			ack();
			return;

		case '\1':
		case '\2':
			nerrs++;
			if (*s != '\n') {
				if (!iamremote) {
					fflush(stdout);
					(void) write(2, s, cp - s);
				}
				if (lfp != NULL)
					(void) fwrite(s, 1, cp - s, lfp);
			}
			if (buf[0] == '\2')
				lostconn(0);
			break;

		default:
			error("rmchk: unexpected response '%s'\n", buf);
			err();
		}
	}
}

/*
 * Check the current directory (initialized by the 'T' command to server())
 * for extraneous files and remove them.
 */
static void
clean(cp)
	char *cp;
{
	DIR *d;
	struct direct *dp;
	struct stat stb;
	char *otp;
	int len, opts;

	opts = 0;
	while (*cp >= '0' && *cp <= '7')
		opts = (opts << 3) | (*cp++ - '0');
	if (*cp != '\0') {
		error("clean: options not delimited\n");
		return;
	}
	if ((d = opendir(target)) == NULL) {
		error("%s:%s: %s\n", host, target, strerror(errno));
		return;
	}
	ack();

	otp = tp;
	len = tp - target;
	while (dp = readdir(d)) {
		if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
			continue;
		if (len + 1 + strlen(dp->d_name) >= BUFSIZ - 1) {
			error("%s:%s/%s: Name too long\n",
				host, target, dp->d_name);
			continue;
		}
		tp = otp;
		*tp++ = '/';
		cp = dp->d_name;;
		while (*tp++ = *cp++)
			;
		tp--;
		if (lstat(target, &stb) < 0) {
			error("%s:%s: %s\n", host, target, strerror(errno));
			continue;
		}
		(void) snprintf(buf, sizeof(buf), "Q%s\n", dp->d_name);
		(void) write(rem, buf, strlen(buf));
		cp = buf;
		do {
			if (read(rem, cp, 1) != 1)
				cleanup(0);
		} while (*cp++ != '\n' && cp < &buf[BUFSIZ]);
		*--cp = '\0';
		cp = buf;
		if (*cp != 'Y')
			continue;
		if (opts & VERIFY) {
			cp = buf;
			*cp++ = '\0';
			(void) snprintf(cp, sizeof(buf) - 1,
				"need to remove: %s\n", target);
			(void) write(rem, buf, strlen(cp) + 1);
		} else
			removeit(&stb);
	}
	closedir(d);
	(void) write(rem, "E\n", 2);
	(void) response();
	tp = otp;
	*tp = '\0';
}

/*
 * Remove a file or directory (recursively) and send back an acknowledge
 * or an error message.
 */
static void
removeit(stp)
	struct stat *stp;
{
	DIR *d;
	struct direct *dp;
	char *cp;
	struct stat stb;
	char *otp;
	int len;

	switch (stp->st_mode & S_IFMT) {
	case S_IFREG:
	case S_IFLNK:
		if (unlink(target) < 0)
			goto bad;
		goto removed;

	case S_IFDIR:
		break;

	default:
		error("%s:%s: not a plain file\n", host, target);
		return;
	}

	if ((d = opendir(target)) == NULL)
		goto bad;

	otp = tp;
	len = tp - target;
	while (dp = readdir(d)) {
		if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
			continue;
		if (len + 1 + strlen(dp->d_name) >= BUFSIZ - 1) {
			error("%s:%s/%s: Name too long\n",
				host, target, dp->d_name);
			continue;
		}
		tp = otp;
		*tp++ = '/';
		cp = dp->d_name;;
		while (*tp++ = *cp++)
			;
		tp--;
		if (lstat(target, &stb) < 0) {
			error("%s:%s: %s\n", host, target, strerror(errno));
			continue;
		}
		removeit(&stb);
	}
	closedir(d);
	tp = otp;
	*tp = '\0';
	if (rmdir(target) < 0) {
bad:
		error("%s:%s: %s\n", host, target, strerror(errno));
		return;
	}
removed:
	cp = buf;
	*cp++ = '\0';
	(void) snprintf(cp, sizeof(buf) - 1, "removed %s\n", target);
	(void) write(rem, buf, strlen(cp) + 1);
}

/*
 * Execute a shell command to handle special cases.
 */
static void
dospecial(cmd)
	char *cmd;
{
	int fd[2], status;
	char *cp, *s;
	char sbuf[BUFSIZ];
	pid_t pid, i;
	extern uid_t userid;
	extern gid_t groupid;

	if (pipe(fd) < 0) {
		error("%s\n", strerror(errno));
		return;
	}
	if ((pid = fork()) == 0) {
		/*
		 * Return everything the shell commands print.
		 */
		(void) close(0);
		(void) close(1);
		(void) close(2);
		(void) open(_PATH_DEVNULL, O_RDONLY);
		(void) dup(fd[1]);
		(void) dup(fd[1]);
		(void) close(fd[0]);
		(void) close(fd[1]);
#if	defined(DIRECT_RCMD)
		setegid(groupid);
		setgid(groupid);
		seteuid(userid);
		setuid(userid);
#endif	/* DIRECT_RCMD */
		execl(_PATH_BSHELL, "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	(void) close(fd[1]);
	s = sbuf;
	*s++ = '\0';
	while ((i = read(fd[0], buf, sizeof(buf))) > 0) {
		cp = buf;
		do {
			*s++ = *cp++;
			if (cp[-1] != '\n') {
				if (s < &sbuf[sizeof(sbuf)-1])
					continue;
				*s++ = '\n';
			}
			/*
			 * Throw away blank lines.
			 */
			if (s == &sbuf[2]) {
				s--;
				continue;
			}
			(void) write(rem, sbuf, s - sbuf);
			s = &sbuf[1];
		} while (--i);
	}
	if (s > &sbuf[1]) {
		*s++ = '\n';
		(void) write(rem, sbuf, s - sbuf);
	}
	while ((i = wait(&status)) != pid && i != -1)
		;
	if (i == -1)
		status = -1;
	(void) close(fd[0]);
	if (status)
		error("shell returned %d\n", status);
	else
		ack();
}

void
log(FILE *fp, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	/* Print changes locally if not quiet mode */
	if (!qflag)
		(void) vprintf(fmt, ap);

	/* Save changes (for mailing) if really updating files */
	if (!(options & VERIFY) && fp != NULL)
		(void) vfprintf(fp, fmt, ap);
	va_end(ap);
}

void
error(const char *fmt, ...)
{
	static FILE *fp;
	va_list ap;

	va_start(ap, fmt);
	++nerrs;
	if (iamremote) {
		if (!fp && (rem < 0 || !(fp = fdopen(rem, "w"))))
			return;
		(void) fprintf(fp, "%crdist: ", 0x01);
		(void) vfprintf(fp, fmt, ap);
		fflush(fp);
	}
	else {
		fflush(stdout);
		(void) fprintf(stderr, "rdist: ");
		(void) vfprintf(stderr, fmt, ap);
		fflush(stderr);
	}
	if (lfp != NULL) {
		(void) fprintf(lfp, "rdist: ");
		(void) vfprintf(lfp, fmt, ap);
		fflush(lfp);
	}
	va_end(ap);
}

void
fatal(const char *fmt, ...)
{
	static FILE *fp;
	va_list ap;

	va_start(ap, fmt);
	++nerrs;
	if (!fp && !(fp = fdopen(rem, "w")))
		return;
	if (iamremote) {
		(void) fprintf(fp, "%crdist: ", 0x02);
		(void) vfprintf(fp, fmt, ap);
		fflush(fp);
	}
	else {
		fflush(stdout);
		(void) fprintf(stderr, "rdist: ");
		(void) vfprintf(stderr, fmt, ap);
		fflush(stderr);
	}
	if (lfp != NULL) {
		(void) fprintf(lfp, "rdist: ");
		(void) vfprintf(lfp, fmt, ap);
		fflush(lfp);
	}
	va_end(ap);
	cleanup(0);
}

static int
response()
{
	char *cp, *s;
	char resp[BUFSIZ];

	if (debug)
		printf("response()\n");

	cp = s = resp;
	do {
		if (read(rem, cp, 1) != 1)
			lostconn(0);
	} while (*cp++ != '\n' && cp < &resp[BUFSIZ]);

	switch (*s++) {
	case '\0':
		*--cp = '\0';
		if (*s != '\0') {
			log(lfp, "%s\n", s);
			return(1);
		}
		return(0);
	case '\3':
		*--cp = '\0';
		log(lfp, "Note: %s\n",s);
		return(response());

	default:
		s--;
		/* fall into... */
	case '\1':
	case '\2':
		nerrs++;
		if (*s != '\n') {
			if (!iamremote) {
				fflush(stdout);
				(void) write(2, s, cp - s);
			}
			if (lfp != NULL)
				(void) fwrite(s, 1, cp - s, lfp);
		}
		if (resp[0] == '\2')
			lostconn(0);
		return(-1);
	}
}

/*
 * Remove temporary files and do any cleanup operations before exiting.
 */
void
cleanup(signo)
	int signo;
{
	(void) unlink(tempfile);
	exit(1);
}

static void
note(const char *fmt, ...)
{
	static char buf[BUFSIZ];
	va_list ap;

	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	comment(buf);
}

static void
comment(s)
	char *s;
{
	char c;

	c = '\3';
	write(rem, &c, 1);
	write(rem, s, strlen(s));
	c = '\n';
	write(rem, &c, 1);
}
