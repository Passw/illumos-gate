#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet
# at http://www.illumos.org/license/CDDL.
#

#
# Copyright 2011, Richard Lowe
# Copyright 2013 Nexenta Systems, Inc.  All rights reserved.
#

include		$(SRC)/Makefile.master

MANSECT=	3kvm

MANFILES=	kvm_getu.3kvm		\
		kvm_kread.3kvm		\
		kvm_nextproc.3kvm	\
		kvm_nlist.3kvm		\
		kvm_open.3kvm		\
		kvm_read.3kvm

.KEEP_STATE:

include		$(SRC)/man/Makefile.man

install:	$(ROOTMANFILES) $(MANLINK_SRCS)
	$(MANLINK_INSTALL)
