#	$OpenBSD: Makefile,v 1.9 2026/04/28 13:25:04 millert Exp $

PROG=	rdate
SRCS=	rdate.c ntp.c
CFLAGS+=-Wall
DPADD+= ${LIBUTIL}
LDADD+= -lutil

MAN=	rdate.8

.include <bsd.prog.mk>
