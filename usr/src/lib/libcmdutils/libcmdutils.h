/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
/*
 * Copyright (c) 2013 RackTop Systems.
 */
/*
 * Copyright 2019 Joyent, Inc.
 */

/*
 * Declarations for the functions in libcmdutils.
 */

#ifndef	_LIBCMDUTILS_H
#define	_LIBCMDUTILS_H

#if !defined(_LP64) && \
	!((_FILE_OFFSET_BITS == 64) || defined(_LARGEFILE64_SOURCE))
#error "libcmdutils.h can only be used in a largefile compilation environment"
#endif

/*
 * This is a private header file.  Applications should not directly include
 * this file.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <libintl.h>
#include <string.h>
#include <dirent.h>
#include <attr.h>
#include <sys/avl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <libnvpair.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* extended system attribute support */
#define	_NOT_SATTR	0
#define	_RO_SATTR	1
#define	_RW_SATTR	2

#define	MAXMAPSIZE	(1024*1024*8)	/* map at most 8MB */
#define	SMALLFILESIZE	(32*1024)	/* don't use mmap on little file */

/* Type used for a node containing a device id and inode number */

#if defined(_LP64) || (_FILE_OFFSET_BITS == 64)
typedef struct tree_node {
	dev_t		node_dev;
	ino_t		node_ino;
	avl_node_t	avl_link;
} tree_node_t;
#else
typedef struct tree_node {
	dev_t		node_dev;
	ino64_t		node_ino;
	avl_node_t	avl_link;
} tree_node_t;
#endif

		/* extended system attribute support */

/* Determine if a file is the name of an extended system attribute file */
extern int sysattr_type(char *);

/* Determine if the underlying file system supports system attributes */
extern int sysattr_support(const char *, int);

/* Copies the content of the source file to the target file */
#if defined(_LP64) || (_FILE_OFFSET_BITS == 64)
extern int writefile(int, int, const char *, const char *, const char *,
	const char *, struct stat *, struct stat *);
#else
extern int writefile(int, int, const char *, const char *, const char *,
	const char *, struct stat64 *, struct stat64 *);
#endif

/* Gets file descriptors of the source and target attribute files */
extern int get_attrdirs(int, int, char *, int *, int *);

/* Move extended attribute and extended system attribute */
extern int mv_xattrs(char *, char *, char *, int, int);

/* Returns non default extended system attribute list */
extern nvlist_t *sysattr_list(const char *, int, const char *);



		/* avltree */

/*
 * Used to compare two nodes.  We are attempting to match the 1st
 * argument (node) against the 2nd argument (a node which
 * is already in the search tree).
 */

extern int tnode_compare(const void *, const void *);

/*
 * Used to add a single node (containing the input device id and
 * inode number) to the specified search tree.  The calling
 * application must set the tree pointer to NULL before calling
 * add_tnode() for the first time.
 */
#if defined(_LP64) || (_FILE_OFFSET_BITS == 64)
extern int add_tnode(avl_tree_t **, dev_t, ino_t);
#else
extern int add_tnode(avl_tree_t **, dev_t, ino64_t);
#endif

/*
 * Used to destroy a whole tree (all nodes) without rebalancing.
 * The calling application is responsible for setting the tree
 * pointer to NULL upon return.
 */
extern void destroy_tree(avl_tree_t *);



		/* user/group id helpers */

/*
 * Used to get the next available user id in given range.
 */
extern int findnextuid(uid_t, uid_t, uid_t *);

/*
 * Used to get the next available group id in given range.
 */
extern int findnextgid(gid_t, gid_t, gid_t *);

#define	NN_DIVISOR_1000		(1U << 0)
#define	NN_UNIT_SPACE		(1U << 1)

/* Minimum size for the output of nicenum, including NULL */
#define	NN_NUMBUF_SZ		(6)

void nicenum(uint64_t, char *, size_t);
void nicenum_scale(uint64_t, size_t, char *, size_t, uint32_t);

#ifdef	__cplusplus
}
#endif

#endif /* _LIBCMDUTILS_H */
