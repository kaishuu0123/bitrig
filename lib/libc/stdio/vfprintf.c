/*	$OpenBSD: vfprintf.c,v 1.65 2014/03/19 05:17:01 guenther Exp $	*/
/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Chris Torek.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
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

/*
 * Actual printf innards.
 *
 * This code is large and complicated...
 */

#include <sys/types.h>

#include <errno.h>
#include <langinfo.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "locale/xlocale_private.h"
#include "local.h"
#include "fvwrite.h"
#include "printflocal.h"

static int	__sprint(FILE *, struct __suio *, locale_t);

#define	CHAR	char
#include "printfcommon.h"

/*
 * Flush out all the vectors defined by the given uio,
 * then reset it so that it can be reused.
 */
static int
__sprint(FILE *fp, struct __suio *uio, locale_t locale)
{
	int err;

	if (uio->uio_resid == 0) {
		uio->uio_iovcnt = 0;
		return (0);
	}
	err = __sfvwrite(fp, uio);
	uio->uio_resid = 0;
	uio->uio_iovcnt = 0;
	return (err);
}

/*
 * Helper function for `fprintf to unbuffered unix file': creates a
 * temporary buffer.  We only work on write-only files; this avoids
 * worries about ungetc buffers and so forth.
 */
static int
__sbprintf(FILE *fp, locale_t locale, const char *fmt, va_list ap)
{
	int ret;
	FILE fake;
	struct __sfileext fakeext;
	unsigned char buf[BUFSIZ];

	_FILEEXT_SETUP(&fake, &fakeext);
	/* copy the important variables */
	fake._flags = fp->_flags & ~__SNBF;
	fake._file = fp->_file;
	fake._cookie = fp->_cookie;
	fake._write = fp->_write;
#ifdef notyet
	fake._orientation = fp->_orientation;
	fake._mbstate = fp->_mbstate;
#endif

	/* set up the buffer */
	fake._bf._base = fake._p = buf;
	fake._bf._size = fake._w = sizeof(buf);
	fake._lbfsize = 0;	/* not actually used, but Just In Case */

	/* do the work, then copy any error status */
	ret = __vfprintf(&fake, locale, fmt, ap);
	if (ret >= 0 && __sflush(&fake))
		ret = EOF;
	if (fake._flags & __SERR)
		fp->_flags |= __SERR;
	return (ret);
}

#ifdef PRINTF_WIDE_CHAR
/*
 * Convert a wide character string argument for the %ls format to a multibyte
 * string representation. If not -1, prec specifies the maximum number of
 * bytes to output, and also means that we can't assume that the wide char
 * string is null-terminated.
 */
static char *
__wcsconv(wchar_t *wcsarg, int prec)
{
	mbstate_t mbs;
	char buf[MB_LEN_MAX];
	wchar_t *p;
	char *convbuf;
	size_t clen, nbytes;

	/* Allocate space for the maximum number of bytes we could output. */
	if (prec < 0) {
		memset(&mbs, 0, sizeof(mbs));
		p = wcsarg;
		nbytes = wcsrtombs(NULL, (const wchar_t **)&p, 0, &mbs);
		if (nbytes == (size_t)-1) {
			errno = EILSEQ;
			return (NULL);
		}
	} else {
		/*
		 * Optimisation: if the output precision is small enough,
		 * just allocate enough memory for the maximum instead of
		 * scanning the string.
		 */
		if (prec < 128)
			nbytes = prec;
		else {
			nbytes = 0;
			p = wcsarg;
			memset(&mbs, 0, sizeof(mbs));
			for (;;) {
				clen = wcrtomb(buf, *p++, &mbs);
				if (clen == 0 || clen == (size_t)-1 ||
				    nbytes + clen > (size_t)prec)
					break;
				nbytes += clen;
			}
			if (clen == (size_t)-1) {
				errno = EILSEQ;
				return (NULL);
			}
		}
	}
	if ((convbuf = malloc(nbytes + 1)) == NULL)
		return (NULL);

	/* Fill the output buffer. */
	p = wcsarg;
	memset(&mbs, 0, sizeof(mbs));
	if ((nbytes = wcsrtombs(convbuf, (const wchar_t **)&p,
	    nbytes, &mbs)) == (size_t)-1) {
		free(convbuf);
		errno = EILSEQ;
		return (NULL);
	}
	convbuf[nbytes] = '\0';
	return (convbuf);
}
#endif

/*
 * MT-safe version
 */
int
vfprintf_l(FILE *fp, locale_t locale, const char *fmt0, __va_list ap)
{
	int ret;
	FIX_LOCALE(locale);

	FLOCKFILE(fp);
	/* optimise fprintf(stderr) (and other unbuffered Unix files) */
	if ((fp->_flags & (__SNBF|__SWR|__SRW)) == (__SNBF|__SWR) &&
	    fp->_file >= 0)
		ret = __sbprintf(fp, locale, fmt0, ap);
	else
		ret = __vfprintf(fp, locale, fmt0, ap);
	FUNLOCKFILE(fp);
	return (ret);
}

int
vfprintf(FILE *fp, const char *fmt0, __va_list ap)
{
	return vfprintf_l(fp, __get_locale(), fmt0, ap);
}

/*
 * The size of the buffer we use as scratch space for integer
 * conversions, among other things.  We need enough space to
 * write a uintmax_t in octal (plus one byte).
 */
#if UINTMAX_MAX <= UINT64_MAX
#define	BUF	32
#else
#error "BUF must be large enough to format a uintmax_t"
#endif

/*
 * Non-MT-safe version
 */
int
__vfprintf(FILE *fp, locale_t locale, const char *fmt0, __va_list ap)
{
	char *fmt;		/* format string */
	int ch;			/* character from fmt */
	int n, n2;		/* handy integers (short term usage) */
	char *cp;		/* handy char pointer (short term usage) */
	int flags;		/* flags as above */
	int ret;		/* return value accumulator */
	int width;		/* width from format (%8d), or 0 */
	int prec;		/* precision from format; <0 for N/A */
	char sign;		/* sign prefix (' ', '+', '-', or \0) */
	wchar_t wc;
	mbstate_t ps;
#ifdef FLOATING_POINT
	/*
	 * We can decompose the printed representation of floating
	 * point numbers into several parts, some of which may be empty:
	 *
	 * [+|-| ] [0x|0X] MMM . NNN [e|E|p|P] [+|-] ZZ
	 *    A       B     ---C---      D       E   F
	 *
	 * A:	'sign' holds this value if present; '\0' otherwise
	 * B:	ox[1] holds the 'x' or 'X'; '\0' if not hexadecimal
	 * C:	cp points to the string MMMNNN.  Leading and trailing
	 *	zeros are not in the string and must be added.
	 * D:	expchar holds this character; '\0' if no exponent, e.g. %f
	 * F:	at least two digits for decimal, at least one digit for hex
	 */
	char *decimal_point = NULL;
	int signflag;		/* true if float is negative */
	union {			/* floating point arguments %[aAeEfFgG] */
		double dbl;
		long double ldbl;
	} fparg;
	int expt;		/* integer value of exponent */
	char expchar;		/* exponent character: [eEpP\0] */
	char *dtoaend;		/* pointer to end of converted digits */
	int expsize;		/* character count for expstr */
	int lead;		/* sig figs before decimal or group sep */
	int ndig;		/* actual number of digits returned by dtoa */
	char expstr[MAXEXPDIG+2];	/* buffer for exponent string: e+ZZZ */
	char *dtoaresult = NULL;
#endif

	uintmax_t _umax;	/* integer arguments %[diouxX] */
	enum { OCT, DEC, HEX } base;	/* base for %[diouxX] conversion */
	int dprec;		/* a copy of prec if %[diouxX], 0 otherwise */
	int realsz;		/* field size expanded by dprec */
	int size;		/* size of converted field or string */
	const char *xdigs;	/* digits for %[xX] conversion */
	struct io_state io;	/* I/O buffering state */
	char buf[BUF];		/* buffer with space for digits of uintmax_t */
	char ox[2];		/* space for 0x; ox[1] is either x, X, or \0 */
	union arg *argtable;	/* args, built due to positional arg */
	union arg statargtable[STATIC_ARG_TBL_SIZE];
	int nextarg;		/* 1-based argument index */
	va_list orgap;		/* original argument pointer */
#ifdef PRINTF_WIDE_CHAR
	char *convbuf;		/* buffer for wide to multi-byte conversion */
#endif

	static const char xdigs_lower[16] = "0123456789abcdef";
	static const char xdigs_upper[16] = "0123456789ABCDEF";

	/* BEWARE, these `goto error' on error. */
#define	PRINT(ptr, len) { \
	if (io_print(&io, (ptr), (len), locale))	\
		goto error; \
}
#define	PAD(howmany, with) { \
	if (io_pad(&io, (howmany), (with), locale)) \
		goto error; \
}
#define	PRINTANDPAD(p, ep, len, with) {	\
	if (io_printandpad(&io, (p), (ep), (len), (with), locale)) \
		goto error; \
}
#define	FLUSH() { \
	if (io_flush(&io, locale)) \
		goto error; \
}
	/*
	 * Get the argument indexed by nextarg.   If the argument table is
	 * built, use it to get the argument.  If its not, get the next
	 * argument (and arguments must be gotten sequentially).
	 */
#define GETARG(type) \
	((argtable != NULL) ? *((type*)(&argtable[nextarg++])) : \
	    (nextarg++, va_arg(ap, type)))

	/*
	 * To extend shorts properly, we need both signed and unsigned
	 * argument extraction methods.
	 */
#define	SARG() \
	((intmax_t)(flags&INTMAXT ? GETARG(intmax_t) : \
	    flags&LLONGINT ? GETARG(long long) : \
	    flags&LONGINT ? GETARG(long) : \
	    flags&PTRDIFFT ? GETARG(ptrdiff_t) : \
	    flags&SIZET ? GETARG(ssize_t) : \
	    flags&SHORTINT ? (short)GETARG(int) : \
	    flags&CHARINT ? (signed char)GETARG(int) : \
	    GETARG(int)))
#define	UARG() \
	((uintmax_t)(flags&INTMAXT ? GETARG(uintmax_t) : \
	    flags&LLONGINT ? GETARG(unsigned long long) : \
	    flags&LONGINT ? GETARG(unsigned long) : \
	    flags&PTRDIFFT ? (uintptr_t)GETARG(ptrdiff_t) : /* XXX */ \
	    flags&SIZET ? GETARG(size_t) : \
	    flags&SHORTINT ? (unsigned short)GETARG(int) : \
	    flags&CHARINT ? (unsigned char)GETARG(int) : \
	    GETARG(unsigned int)))

	/*
	 * Append a digit to a value and check for overflow.
	 */
#define APPEND_DIGIT(val, dig) do { \
	if ((val) > INT_MAX / 10) \
		goto overflow; \
	(val) *= 10; \
	if ((val) > INT_MAX - to_digit((dig))) \
		goto overflow; \
	(val) += to_digit((dig)); \
} while (0)

	 /*
	  * Get * arguments, including the form *nn$.  Preserve the nextarg
	  * that the argument can be gotten once the type is determined.
	  */
#define GETASTER(val) \
	n2 = 0; \
	cp = fmt; \
	while (is_digit(*cp)) { \
		APPEND_DIGIT(n2, *cp); \
		cp++; \
	} \
	if (*cp == '$') { \
		int hold = nextarg; \
		if (argtable == NULL) { \
			argtable = statargtable; \
			if (__find_arguments (fmt0, orgap, &argtable)) { \
				ret = EOF; \
				goto error; \
			} \
		} \
		nextarg = n2; \
		val = GETARG(int); \
		nextarg = hold; \
		fmt = ++cp; \
	} else { \
		val = GETARG(int); \
	}

	_SET_ORIENTATION(fp, -1);
	/* sorry, fprintf(read_only_file, "") returns EOF, not 0 */
	if (cantwrite(fp)) {
		errno = EBADF;
		return (EOF);
	}

	fmt = (char *)fmt0;
	argtable = NULL;
	nextarg = 1;
	va_copy(orgap, ap);
	io_init(&io, fp);
	ret = 0;
#ifdef PRINTF_WIDE_CHAR
	convbuf = NULL;
#endif

	memset(&ps, 0, sizeof(ps));
	/*
	 * Scan the format for conversions (`%' character).
	 */
	for (;;) {
		cp = fmt;
		while ((n = mbrtowc(&wc, fmt, MB_CUR_MAX, &ps)) > 0) {
			fmt += n;
			if (wc == '%') {
				fmt--;
				break;
			}
		}
		if (fmt != cp) {
			ptrdiff_t m = fmt - cp;
			if (m < 0 || m > INT_MAX - ret)
				goto overflow;
			PRINT(cp, m);
			ret += m;
		}
		if (n <= 0)
			goto done;
		fmt++;		/* skip over '%' */

		flags = 0;
		dprec = 0;
		width = 0;
		prec = -1;
		sign = '\0';
		ox[1] = '\0';

rflag:		ch = *fmt++;
reswitch:	switch (ch) {
		case ' ':
			/*
			 * ``If the space and + flags both appear, the space
			 * flag will be ignored.''
			 *	-- ANSI X3J11
			 */
			if (!sign)
				sign = ' ';
			goto rflag;
		case '#':
			flags |= ALT;
			goto rflag;
		case '\'':
			/* grouping not implemented */
			goto rflag;
		case '*':
			/*
			 * ``A negative field width argument is taken as a
			 * - flag followed by a positive field width.''
			 *	-- ANSI X3J11
			 * They don't exclude field widths read from args.
			 */
			GETASTER(width);
			if (width >= 0)
				goto rflag;
			if (width == INT_MIN)
				goto overflow;
			width = -width;
			/* FALLTHROUGH */
		case '-':
			flags |= LADJUST;
			goto rflag;
		case '+':
			sign = '+';
			goto rflag;
		case '.':
			if ((ch = *fmt++) == '*') {
				GETASTER(n);
				prec = n < 0 ? -1 : n;
				goto rflag;
			}
			n = 0;
			while (is_digit(ch)) {
				APPEND_DIGIT(n, ch);
				ch = *fmt++;
			}
			if (ch == '$') {
				nextarg = n;
				if (argtable == NULL) {
					argtable = statargtable;
					if (__find_arguments (fmt0, orgap,
							      &argtable)) {
						ret = EOF;
						goto error;
					}
				}
				goto rflag;
			}
			prec = n;
			goto reswitch;
		case '0':
			/*
			 * ``Note that 0 is taken as a flag, not as the
			 * beginning of a field width.''
			 *	-- ANSI X3J11
			 */
			flags |= ZEROPAD;
			goto rflag;
		case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			n = 0;
			do {
				APPEND_DIGIT(n, ch);
				ch = *fmt++;
			} while (is_digit(ch));
			if (ch == '$') {
				nextarg = n;
				if (argtable == NULL) {
					argtable = statargtable;
					if (__find_arguments (fmt0, orgap,
							      &argtable)) {
						ret = EOF;
						goto error;
					}
				}
				goto rflag;
			}
			width = n;
			goto reswitch;
#ifdef FLOATING_POINT
		case 'L':
			flags |= LONGDBL;
			goto rflag;
#endif
		case 'h':
			if (*fmt == 'h') {
				fmt++;
				flags |= CHARINT;
			} else {
				flags |= SHORTINT;
			}
			goto rflag;
		case 'j':
			flags |= INTMAXT;
			goto rflag;
		case 'l':
			if (*fmt == 'l') {
				fmt++;
				flags |= LLONGINT;
			} else {
				flags |= LONGINT;
			}
			goto rflag;
		case 'q':
			flags |= LLONGINT;
			goto rflag;
		case 't':
			flags |= PTRDIFFT;
			goto rflag;
		case 'z':
			flags |= SIZET;
			goto rflag;
		case 'c':
#ifdef PRINTF_WIDE_CHAR
			if (flags & LONGINT) {
				mbstate_t mbs;
				size_t mbseqlen;

				memset(&mbs, 0, sizeof(mbs));
				mbseqlen = wcrtomb(buf,
				    (wchar_t)GETARG(wint_t), &mbs);
				if (mbseqlen == (size_t)-1) {
					fp->_flags |= __SERR;
					errno = EILSEQ;
					goto error;
				}
				cp = buf;
				size = (int)mbseqlen;
			} else {
#endif
				*(cp = buf) = GETARG(int);
				size = 1;
#ifdef PRINTF_WIDE_CHAR
			}
#endif
			sign = '\0';
			break;
		case 'D':
			flags |= LONGINT;
			/*FALLTHROUGH*/
		case 'd':
		case 'i':
			_umax = SARG();
			if ((intmax_t)_umax < 0) {
				_umax = -_umax;
				sign = '-';
			}
			base = DEC;
			goto number;
#ifdef FLOATING_POINT
		case 'a':
		case 'A':
			if (ch == 'a') {
				ox[1] = 'x';
				xdigs = xdigs_lower;
				expchar = 'p';
			} else {
				ox[1] = 'X';
				xdigs = xdigs_upper;
				expchar = 'P';
			}
			if (prec >= 0)
				prec++;
			if (dtoaresult)
				__freedtoa(dtoaresult);
			if (flags & LONGDBL) {
				fparg.ldbl = GETARG(long double);
				dtoaresult = cp =
				    __hldtoa(fparg.ldbl, xdigs, prec,
				    &expt, &signflag, &dtoaend);
				if (dtoaresult == NULL) {
					errno = ENOMEM;
					goto error;
				}
			} else {
				fparg.dbl = GETARG(double);
				dtoaresult = cp =
				    __hdtoa(fparg.dbl, xdigs, prec,
				    &expt, &signflag, &dtoaend);
				if (dtoaresult == NULL) {
					errno = ENOMEM;
					goto error;
				}
			}
			if (prec < 0)
				prec = dtoaend - cp;
			if (expt == INT_MAX)
				ox[1] = '\0';
			goto fp_common;
		case 'e':
		case 'E':
			expchar = ch;
			if (prec < 0)	/* account for digit before decpt */
				prec = DEFPREC + 1;
			else
				prec++;
			goto fp_begin;
		case 'f':
		case 'F':
			expchar = '\0';
			goto fp_begin;
		case 'g':
		case 'G':
			expchar = ch - ('g' - 'e');
 			if (prec == 0)
 				prec = 1;
fp_begin:
			if (prec < 0)
				prec = DEFPREC;
			if (dtoaresult)
				__freedtoa(dtoaresult);
			if (flags & LONGDBL) {
				fparg.ldbl = GETARG(long double);
				dtoaresult = cp =
				    __ldtoa(&fparg.ldbl, expchar ? 2 : 3, prec,
				    &expt, &signflag, &dtoaend);
				if (dtoaresult == NULL) {
					errno = ENOMEM;
					goto error;
				}
			} else {
				fparg.dbl = GETARG(double);
				dtoaresult = cp =
				    __dtoa(fparg.dbl, expchar ? 2 : 3, prec,
				    &expt, &signflag, &dtoaend);
				if (dtoaresult == NULL) {
					errno = ENOMEM;
					goto error;
				}
				if (expt == 9999)
					expt = INT_MAX;
 			}
fp_common:
			if (signflag)
				sign = '-';
			if (expt == INT_MAX) {	/* inf or nan */
				if (*cp == 'N') {
					cp = (ch >= 'a') ? "nan" : "NAN";
					sign = '\0';
				} else
					cp = (ch >= 'a') ? "inf" : "INF";
 				size = 3;
				flags &= ~ZEROPAD;
 				break;
 			}
			flags |= FPT;
			ndig = dtoaend - cp;
 			if (ch == 'g' || ch == 'G') {
				if (expt > -4 && expt <= prec) {
					/* Make %[gG] smell like %[fF] */
					expchar = '\0';
					if (flags & ALT)
						prec -= expt;
					else
						prec = ndig - expt;
					if (prec < 0)
						prec = 0;
				} else {
					/*
					 * Make %[gG] smell like %[eE], but
					 * trim trailing zeroes if no # flag.
					 */
					if (!(flags & ALT))
						prec = ndig;
				}
 			}
			if (expchar) {
				expsize = exponent(expstr, expt - 1, expchar);
				size = expsize + prec;
				if (prec > 1 || flags & ALT)
 					++size;
			} else {
				/* space for digits before decimal point */
				if (expt > 0)
					size = expt;
				else	/* "0" */
					size = 1;
				/* space for decimal pt and following digits */
				if (prec || flags & ALT)
					size += prec + 1;
				lead = expt;
			}
			break;
#endif /* FLOATING_POINT */
		case 'n':
			if (flags & LLONGINT)
				*GETARG(long long *) = ret;
			else if (flags & LONGINT)
				*GETARG(long *) = ret;
			else if (flags & SHORTINT)
				*GETARG(short *) = ret;
			else if (flags & CHARINT)
				*GETARG(signed char *) = ret;
			else if (flags & PTRDIFFT)
				*GETARG(ptrdiff_t *) = ret;
			else if (flags & SIZET)
				*GETARG(ssize_t *) = ret;
			else if (flags & INTMAXT)
				*GETARG(intmax_t *) = ret;
			else
				*GETARG(int *) = ret;
			continue;	/* no output */
		case 'O':
			flags |= LONGINT;
			/*FALLTHROUGH*/
		case 'o':
			_umax = UARG();
			base = OCT;
			goto nosign;
		case 'p':
			/*
			 * ``The argument shall be a pointer to void.  The
			 * value of the pointer is converted to a sequence
			 * of printable characters, in an implementation-
			 * defined manner.''
			 *	-- ANSI X3J11
			 */
			/* NOSTRICT */
			_umax = (u_long)GETARG(void *);
			base = HEX;
			xdigs = xdigs_lower;
			ox[1] = 'x';
			goto nosign;
		case 's':
#ifdef PRINTF_WIDE_CHAR
			if (flags & LONGINT) {
				wchar_t *wcp;

				if (convbuf != NULL) {
					free(convbuf);
					convbuf = NULL;
				}
				if ((wcp = GETARG(wchar_t *)) == NULL) {
					cp = "(null)";
				} else {
					convbuf = __wcsconv(wcp, prec);
					if (convbuf == NULL) {
						fp->_flags |= __SERR;
						goto error;
					}
					cp = convbuf;
				}
			} else
#endif /* PRINTF_WIDE_CHAR */
			if ((cp = GETARG(char *)) == NULL)
				cp = "(null)";
			if (prec >= 0) {
				/*
				 * can't use strlen; can only look for the
				 * NUL in the first `prec' characters, and
				 * strlen() will go further.
				 */
				char *p = memchr(cp, 0, prec);

				size = p ? (p - cp) : prec;
			} else {
				size_t len;

				if ((len = strlen(cp)) > INT_MAX)
					goto overflow;
				size = (int)len;
			}
			sign = '\0';
			break;
		case 'U':
			flags |= LONGINT;
			/*FALLTHROUGH*/
		case 'u':
			_umax = UARG();
			base = DEC;
			goto nosign;
		case 'X':
			xdigs = xdigs_upper;
			goto hex;
		case 'x':
			xdigs = xdigs_lower;
hex:			_umax = UARG();
			base = HEX;
			/* leading 0x/X only if non-zero */
			if (flags & ALT && _umax != 0)
				ox[1] = ch;

			/* unsigned conversions */
nosign:			sign = '\0';
			/*
			 * ``... diouXx conversions ... if a precision is
			 * specified, the 0 flag will be ignored.''
			 *	-- ANSI X3J11
			 */
number:			if ((dprec = prec) >= 0)
				flags &= ~ZEROPAD;

			/*
			 * ``The result of converting a zero value with an
			 * explicit precision of zero is no characters.''
			 *	-- ANSI X3J11
			 */
			cp = buf + BUF;
			if (_umax != 0 || prec != 0) {
				/*
				 * Unsigned mod is hard, and unsigned mod
				 * by a constant is easier than that by
				 * a variable; hence this switch.
				 */
				switch (base) {
				case OCT:
					do {
						*--cp = to_char(_umax & 7);
						_umax >>= 3;
					} while (_umax);
					/* handle octal leading 0 */
					if (flags & ALT && *cp != '0')
						*--cp = '0';
					break;

				case DEC:
					/* many numbers are 1 digit */
					while (_umax >= 10) {
						*--cp = to_char(_umax % 10);
						_umax /= 10;
					}
					*--cp = to_char(_umax);
					break;

				case HEX:
					do {
						*--cp = xdigs[_umax & 15];
						_umax >>= 4;
					} while (_umax);
					break;

				default:
					cp = "bug in vfprintf: bad base";
					size = strlen(cp);
					goto skipsize;
				}
			}
			size = buf + BUF - cp;
			if (size > BUF)	/* should never happen */
				abort();
		skipsize:
			break;
		default:	/* "%?" prints ?, unless ? is NUL */
			if (ch == '\0')
				goto done;
			/* pretend it was %c with argument ch */
			cp = buf;
			*cp = ch;
			size = 1;
			sign = '\0';
			break;
		}

		/*
		 * All reasonable formats wind up here.  At this point, `cp'
		 * points to a string which (if not flags&LADJUST) should be
		 * padded out to `width' places.  If flags&ZEROPAD, it should
		 * first be prefixed by any sign or other prefix; otherwise,
		 * it should be blank padded before the prefix is emitted.
		 * After any left-hand padding and prefixing, emit zeroes
		 * required by a decimal %[diouxX] precision, then print the
		 * string proper, then emit zeroes required by any leftover
		 * floating precision; finally, if LADJUST, pad with blanks.
		 *
		 * Compute actual size, so we know how much to pad.
		 * size excludes decimal prec; realsz includes it.
		 */
		realsz = dprec > size ? dprec : size;
		if (sign)
			realsz++;
		if (ox[1])
			realsz+= 2;

		/* right-adjusting blank padding */
		if ((flags & (LADJUST|ZEROPAD)) == 0)
			PAD(width - realsz, blanks);

		/* prefix */
		if (sign)
			PRINT(&sign, 1);
		if (ox[1]) {	/* ox[1] is either x, X, or \0 */
			ox[0] = '0';
			PRINT(ox, 2);
		}

		/* right-adjusting zero padding */
		if ((flags & (LADJUST|ZEROPAD)) == ZEROPAD)
			PAD(width - realsz, zeroes);

		/* leading zeroes from decimal precision */
		PAD(dprec - size, zeroes);

		/* the string or number proper */
#ifdef FLOATING_POINT
		if ((flags & FPT) == 0) {
			PRINT(cp, size);
		} else {	/* glue together f_p fragments */
			if (decimal_point == NULL)
				decimal_point = nl_langinfo(RADIXCHAR);
			if (!expchar) {	/* %[fF] or sufficiently short %[gG] */
				if (expt <= 0) {
					PRINT(zeroes, 1);
					if (prec || flags & ALT)
						PRINT(decimal_point, 1);
					PAD(-expt, zeroes);
					/* already handled initial 0's */
					prec += expt;
 				} else {
					PRINTANDPAD(cp, dtoaend, lead, zeroes);
					cp += lead;
					if (prec || flags & ALT)
						PRINT(decimal_point, 1);
				}
				PRINTANDPAD(cp, dtoaend, prec, zeroes);
			} else {	/* %[eE] or sufficiently long %[gG] */
				if (prec > 1 || flags & ALT) {
					buf[0] = *cp++;
					buf[1] = *decimal_point;
					PRINT(buf, 2);
					PRINT(cp, ndig-1);
					PAD(prec - ndig, zeroes);
				} else { /* XeYYY */
					PRINT(cp, 1);
				}
				PRINT(expstr, expsize);
			}
		}
#else
		PRINT(cp, size);
#endif
		/* left-adjusting padding (always blank) */
		if (flags & LADJUST)
			PAD(width - realsz, blanks);

		/* finally, adjust ret */
		if (width < realsz)
			width = realsz;
		if (width > INT_MAX - ret)
			goto overflow;
		ret += width;

		FLUSH();	/* copy out the I/O vectors */
	}
done:
	FLUSH();
error:
	va_end(orgap);
	if (__sferror(fp))
		ret = -1;
	goto finish;

overflow:
	errno = ENOMEM;
	ret = -1;

finish:
#ifdef PRINTF_WIDE_CHAR
	if (convbuf)
		free(convbuf);
#endif
#ifdef FLOATING_POINT
	if (dtoaresult)
		__freedtoa(dtoaresult);
#endif
	if (argtable != NULL && argtable != statargtable)
		free(argtable);
	return (ret);
}
