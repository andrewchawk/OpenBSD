#	$OpenBSD: Makefile,v 1.10 2026/04/19 19:54:02 millert Exp $

PROG=	login_chpass
SRCS=	login_chpass.c
MAN=	login_chpass.8
CFLAGS+=-Wall

BINOWN=	root
BINGRP=	auth
BINMODE=555
BINDIR=	/usr/libexec/auth

.include <bsd.prog.mk>
