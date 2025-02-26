#!/bin/ksh -p
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
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# Copyright (c) 2013, 2016 by Delphix. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/xattr/xattr_common.kshlib

#
# DESCRIPTION:
#
# Creating files on ufs and tmpfs, and copying those files to ZFS with
# appropriate cp flags, the xattrs will still be readable.
#
# STRATEGY:
#	1. Create files in ufs and tmpfs with xattrs
#       2. Copy those files to zfs
#	3. Ensure the xattrs can be read and written
#	4. Do the same in reverse.
#

# we need to be able to create zvols to hold our test
# ufs filesystem.
verify_runnable "global"

# Make sure we clean up properly
function cleanup {

	if [ $( ismounted /tmp/ufs.$$ ufs ) ]
	then
		log_must umount /tmp/ufs.$$
		log_must rm -rf /tmp/ufs.$$
	fi
}

log_assert "Files from ufs,tmpfs with xattrs copied to zfs retain xattr info."
log_onexit cleanup

# Create a UFS file system that we can work in
log_must zfs create -V128m $TESTPOOL/$TESTFS/zvol
log_must eval "new_fs ${ZVOL_RDEVDIR}/$TESTPOOL/$TESTFS/zvol > /dev/null 2>&1"

log_must mkdir /tmp/ufs.$$
log_must mount ${ZVOL_DEVDIR}/$TESTPOOL/$TESTFS/zvol /tmp/ufs.$$

# Create files in ufs and tmpfs, and set some xattrs on them.
log_must touch /tmp/ufs.$$/ufs-file.$$
log_must touch /tmp/tmpfs-file.$$

log_must runat /tmp/ufs.$$/ufs-file.$$ cp /etc/passwd .
log_must runat /tmp/tmpfs-file.$$ cp /etc/group .

# copy those files to ZFS
log_must cp -@ /tmp/ufs.$$/ufs-file.$$ $TESTDIR
log_must cp -@ /tmp/tmpfs-file.$$ $TESTDIR

# ensure the xattr information has been copied correctly
log_must runat $TESTDIR/ufs-file.$$ diff passwd /etc/passwd
log_must runat $TESTDIR/tmpfs-file.$$ diff group /etc/group

log_must umount /tmp/ufs.$$
log_pass "Files from ufs,tmpfs with xattrs copied to zfs retain xattr info."
