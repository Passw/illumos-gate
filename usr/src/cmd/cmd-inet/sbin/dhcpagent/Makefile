#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
# Copyright (c) 2016, Chris Fraire <cfraire@me.com>.
# Copyright 2020 Joyent, Inc.
# Copyright 2019 Joshua M. Clulow <josh@sysmgr.org>
#

PROG    = dhcpagent
ROOTFS_PROG = $(PROG)
DEFAULTFILES = dhcpagent.dfl

OBJS =	adopt.o agent.o async.o bound.o class_id.o defaults.o inform.o \
	init_reboot.o interface.o ipc_action.o packet.o release.o renew.o \
	request.o script_handler.o select.o states.o util.o

include ../../../Makefile.cmd
include ../../../Makefile.ctf

POFILES = $(OBJS:%.o=%.po)
XGETFLAGS += -a -x dhcpagent.xcl

CERRWARN += -_gcc=-Wno-switch
CERRWARN += -_gcc=-Wno-parentheses

CSTD = $(CSTD_GNU99)

# not linted
SMATCH=off
CPPFLAGS  += -D_XOPEN_SOURCE=500 -D__EXTENSIONS__
LDLIBS    += -lxnet -lnvpair -ldhcpagent -ldhcputil -linetutil -ldevinfo \
	-ldlpi -lresolv -lipadm

.KEEP_STATE:

all:		$(ROOTFS_PROG)

install:	all $(ROOTSBINPROG) $(ROOTETCDEFAULTFILES)

$(PROG):	$(OBJS)
		$(LINK.c) -o $@ $(OBJS) $(LDLIBS)
		$(POST_PROCESS)

$(POFILE):	$(POFILES)
		$(RM) $@; $(CAT) $(POFILES) > $@; $(RM) $(POFILES)

clean:
		$(RM) $(OBJS)

include ../../../Makefile.targ
