#	$Id: Makefile,v 1.6 2014/01/18 05:54:52 martynas Exp $

.PATH:		${.CURDIR}/../snmpd

PROG=		snmpctl
SRCS=		log.c ber.c smi.c snmpclient.c snmpctl.c parser.c

MAN=		snmpctl.8

LDADD=		-lutil
DPADD=		${LIBUTIL}
CFLAGS+=	-Wall -I${.CURDIR} -I${.CURDIR}/../snmpd
CFLAGS+=	-Wstrict-prototypes -Wmissing-prototypes
CFLAGS+=	-Wmissing-declarations
CFLAGS+=	-Wshadow -Wpointer-arith -Wcast-qual
CFLAGS+=	-Wsign-compare

.include <bsd.prog.mk>
