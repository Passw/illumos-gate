#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

# Copyright 2015, Richard Lowe.

SUBDIRS=		\
	configd		\
	libscf		\
	libsqlite	\
	libuutil	\
	svccfg

all	:=	TARGET = all
install :=	TARGET = install
clean	:=	TARGET = clean
clobber	:=	TARGET = clobber

.KEEP_STATE:

svccfg: libscf libuutil
configd: libsqlite libuutil
libscf: libuutil


all clean clobber install: $(SUBDIRS)

$(SUBDIRS):	FRC
	@cd $@; pwd; $(MAKE) $(TARGET)

FRC:
