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
 * Copyright 2017 Nexenta Systems, Inc.
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2018 by Delphix. All rights reserved.
 * Copyright 2015 RackTop Systems.
 * Copyright (c) 2016, Intel Corporation.
 * Copyright 2020 Joyent, Inc.
 * Copyright 2023 Oxide Computer Company
 */

/*
 * Pool import support functions.
 *
 * Used by zpool, ztest, zdb, and zhack to locate importable configs. Since
 * these commands are expected to run in the global zone, we can assume
 * that the devices are all readable when called.
 *
 * To import a pool, we rely on reading the configuration information from the
 * ZFS label of each device.  If we successfully read the label, then we
 * organize the configuration information in the following hierarchy:
 *
 *	pool guid -> toplevel vdev guid -> label txg
 *
 * Duplicate entries matching this same tuple will be discarded.  Once we have
 * examined every device, we pick the best label txg config for each toplevel
 * vdev.  We then arrange these toplevel vdevs into a complete pool config, and
 * update any paths that have changed.  Finally, we attempt to import the pool
 * using our derived config, and record the results.
 */

#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <ctype.h>
#include <devid.h>
#include <dirent.h>
#include <errno.h>
#include <libintl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/vtoc.h>
#include <sys/dktp/fdisk.h>
#include <sys/efi_partition.h>
#include <sys/vdev_impl.h>
#include <sys/fs/zfs.h>

#include <thread_pool.h>
#include <libzutil.h>
#include <libnvpair.h>

#include "zutil_import.h"

#ifdef NDEBUG
#define	verify(EX)	((void)(EX))
#else
#define	verify(EX)	assert(EX)
#endif

/*PRINTFLIKE2*/
static void
zutil_error_aux(libpc_handle_t *hdl, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	(void) vsnprintf(hdl->lpc_desc, sizeof (hdl->lpc_desc), fmt, ap);
	hdl->lpc_desc_active = B_TRUE;

	va_end(ap);
}

static void
zutil_verror(libpc_handle_t *hdl, const char *error, const char *fmt,
    va_list ap)
{
	char action[1024];

	(void) vsnprintf(action, sizeof (action), fmt, ap);

	if (hdl->lpc_desc_active)
		hdl->lpc_desc_active = B_FALSE;
	else
		hdl->lpc_desc[0] = '\0';

	if (hdl->lpc_printerr) {
		if (hdl->lpc_desc[0] != '\0')
			error = hdl->lpc_desc;

		(void) fprintf(stderr, "%s: %s\n", action, error);
	}
}

/*PRINTFLIKE3*/
static int
zutil_error_fmt(libpc_handle_t *hdl, const char *error, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	zutil_verror(hdl, error, fmt, ap);

	va_end(ap);

	return (-1);
}

static int
zutil_error(libpc_handle_t *hdl, const char *error, const char *msg)
{
	return (zutil_error_fmt(hdl, error, "%s", msg));
}

static int
zutil_no_memory(libpc_handle_t *hdl)
{
	(void) zutil_error(hdl, EZFS_NOMEM, "internal error");
	exit(1);
}

void *
zutil_alloc(libpc_handle_t *hdl, size_t size)
{
	void *data;

	if ((data = calloc(1, size)) == NULL)
		(void) zutil_no_memory(hdl);

	return (data);
}

char *
zutil_strdup(libpc_handle_t *hdl, const char *str)
{
	char *ret;

	if ((ret = strdup(str)) == NULL)
		(void) zutil_no_memory(hdl);

	return (ret);
}

/*
 * Intermediate structures used to gather configuration information.
 */
typedef struct config_entry {
	uint64_t		ce_txg;
	nvlist_t		*ce_config;
	struct config_entry	*ce_next;
} config_entry_t;

typedef struct vdev_entry {
	uint64_t		ve_guid;
	config_entry_t		*ve_configs;
	struct vdev_entry	*ve_next;
} vdev_entry_t;

typedef struct pool_entry {
	uint64_t		pe_guid;
	vdev_entry_t		*pe_vdevs;
	struct pool_entry	*pe_next;
} pool_entry_t;

typedef struct name_entry {
	char			*ne_name;
	uint64_t		ne_guid;
	struct name_entry	*ne_next;
} name_entry_t;

typedef struct pool_list {
	pool_entry_t		*pools;
	name_entry_t		*names;
} pool_list_t;

/*
 * Go through and fix up any path and/or devid information for the given vdev
 * configuration.
 */
static int
fix_paths(nvlist_t *nv, name_entry_t *names)
{
	nvlist_t **child;
	uint_t c, children;
	uint64_t guid;
	name_entry_t *ne, *best;
	char *path, *devid;
	int matched;

	if (nvlist_lookup_nvlist_array(nv, ZPOOL_CONFIG_CHILDREN,
	    &child, &children) == 0) {
		for (c = 0; c < children; c++)
			if (fix_paths(child[c], names) != 0)
				return (-1);
		return (0);
	}

	/*
	 * This is a leaf (file or disk) vdev.  In either case, go through
	 * the name list and see if we find a matching guid.  If so, replace
	 * the path and see if we can calculate a new devid.
	 *
	 * There may be multiple names associated with a particular guid, in
	 * which case we have overlapping slices or multiple paths to the same
	 * disk.  If this is the case, then we want to pick the path that is
	 * the most similar to the original, where "most similar" is the number
	 * of matching characters starting from the end of the path.  This will
	 * preserve slice numbers even if the disks have been reorganized, and
	 * will also catch preferred disk names if multiple paths exist.
	 */
	verify(nvlist_lookup_uint64(nv, ZPOOL_CONFIG_GUID, &guid) == 0);
	if (nvlist_lookup_string(nv, ZPOOL_CONFIG_PATH, &path) != 0)
		path = NULL;

	matched = 0;
	best = NULL;
	for (ne = names; ne != NULL; ne = ne->ne_next) {
		if (ne->ne_guid == guid) {
			const char *src, *dst;
			int count;

			if (path == NULL) {
				best = ne;
				break;
			}

			src = ne->ne_name + strlen(ne->ne_name) - 1;
			dst = path + strlen(path) - 1;
			for (count = 0; src >= ne->ne_name && dst >= path;
			    src--, dst--, count++)
				if (*src != *dst)
					break;

			/*
			 * At this point, 'count' is the number of characters
			 * matched from the end.
			 */
			if (count > matched || best == NULL) {
				best = ne;
				matched = count;
			}
		}
	}

	if (best == NULL)
		return (0);

	if (nvlist_add_string(nv, ZPOOL_CONFIG_PATH, best->ne_name) != 0)
		return (-1);

	if ((devid = devid_str_from_path(best->ne_name)) == NULL) {
		(void) nvlist_remove_all(nv, ZPOOL_CONFIG_DEVID);
	} else {
		if (nvlist_add_string(nv, ZPOOL_CONFIG_DEVID, devid) != 0) {
			devid_str_free(devid);
			return (-1);
		}
		devid_str_free(devid);
	}

	return (0);
}

/*
 * Add the given configuration to the list of known devices.
 */
static int
add_config(libpc_handle_t *hdl, pool_list_t *pl, const char *path,
    int order, int num_labels, nvlist_t *config)
{
	uint64_t pool_guid, vdev_guid, top_guid, txg, state;
	pool_entry_t *pe;
	vdev_entry_t *ve;
	config_entry_t *ce;
	name_entry_t *ne;

	/*
	 * If this is a hot spare not currently in use or level 2 cache
	 * device, add it to the list of names to translate, but don't do
	 * anything else.
	 */
	if (nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_STATE,
	    &state) == 0 &&
	    (state == POOL_STATE_SPARE || state == POOL_STATE_L2CACHE) &&
	    nvlist_lookup_uint64(config, ZPOOL_CONFIG_GUID, &vdev_guid) == 0) {
		if ((ne = zutil_alloc(hdl, sizeof (name_entry_t))) == NULL)
			return (-1);

		if ((ne->ne_name = zutil_strdup(hdl, path)) == NULL) {
			free(ne);
			return (-1);
		}

		ne->ne_guid = vdev_guid;
		ne->ne_next = pl->names;
		pl->names = ne;

		return (0);
	}

	/*
	 * If we have a valid config but cannot read any of these fields, then
	 * it means we have a half-initialized label.  In vdev_label_init()
	 * we write a label with txg == 0 so that we can identify the device
	 * in case the user refers to the same disk later on.  If we fail to
	 * create the pool, we'll be left with a label in this state
	 * which should not be considered part of a valid pool.
	 */
	if (nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID,
	    &pool_guid) != 0 ||
	    nvlist_lookup_uint64(config, ZPOOL_CONFIG_GUID,
	    &vdev_guid) != 0 ||
	    nvlist_lookup_uint64(config, ZPOOL_CONFIG_TOP_GUID,
	    &top_guid) != 0 ||
	    nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_TXG,
	    &txg) != 0 || txg == 0) {
		return (0);
	}

	/*
	 * First, see if we know about this pool.  If not, then add it to the
	 * list of known pools.
	 */
	for (pe = pl->pools; pe != NULL; pe = pe->pe_next) {
		if (pe->pe_guid == pool_guid)
			break;
	}

	if (pe == NULL) {
		if ((pe = zutil_alloc(hdl, sizeof (pool_entry_t))) == NULL) {
			return (-1);
		}
		pe->pe_guid = pool_guid;
		pe->pe_next = pl->pools;
		pl->pools = pe;
	}

	/*
	 * Second, see if we know about this toplevel vdev.  Add it if its
	 * missing.
	 */
	for (ve = pe->pe_vdevs; ve != NULL; ve = ve->ve_next) {
		if (ve->ve_guid == top_guid)
			break;
	}

	if (ve == NULL) {
		if ((ve = zutil_alloc(hdl, sizeof (vdev_entry_t))) == NULL) {
			return (-1);
		}
		ve->ve_guid = top_guid;
		ve->ve_next = pe->pe_vdevs;
		pe->pe_vdevs = ve;
	}

	/*
	 * Third, see if we have a config with a matching transaction group.  If
	 * so, then we do nothing.  Otherwise, add it to the list of known
	 * configs.
	 */
	for (ce = ve->ve_configs; ce != NULL; ce = ce->ce_next) {
		if (ce->ce_txg == txg)
			break;
	}

	if (ce == NULL) {
		if ((ce = zutil_alloc(hdl, sizeof (config_entry_t))) == NULL) {
			return (-1);
		}
		ce->ce_txg = txg;
		ce->ce_config = fnvlist_dup(config);
		ce->ce_next = ve->ve_configs;
		ve->ve_configs = ce;
	}

	/*
	 * At this point we've successfully added our config to the list of
	 * known configs.  The last thing to do is add the vdev guid -> path
	 * mappings so that we can fix up the configuration as necessary before
	 * doing the import.
	 */
	if ((ne = zutil_alloc(hdl, sizeof (name_entry_t))) == NULL)
		return (-1);

	if ((ne->ne_name = zutil_strdup(hdl, path)) == NULL) {
		free(ne);
		return (-1);
	}

	ne->ne_guid = vdev_guid;
	ne->ne_next = pl->names;
	pl->names = ne;

	return (0);
}

/*
 * Returns true if the named pool matches the given GUID.
 */
static int
zutil_pool_active(libpc_handle_t *hdl, const char *name, uint64_t guid,
    boolean_t *isactive)
{
	ASSERT(hdl->lpc_ops->pco_pool_active != NULL);

	int error = hdl->lpc_ops->pco_pool_active(hdl->lpc_lib_handle, name,
	    guid, isactive);

	return (error);
}

static nvlist_t *
zutil_refresh_config(libpc_handle_t *hdl, nvlist_t *tryconfig)
{
	ASSERT(hdl->lpc_ops->pco_refresh_config != NULL);

	return (hdl->lpc_ops->pco_refresh_config(hdl->lpc_lib_handle,
	    tryconfig));
}

/*
 * Determine if the vdev id is a hole in the namespace.
 */
static boolean_t
vdev_is_hole(uint64_t *hole_array, uint_t holes, uint_t id)
{
	for (int c = 0; c < holes; c++) {

		/* Top-level is a hole */
		if (hole_array[c] == id)
			return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * Convert our list of pools into the definitive set of configurations.  We
 * start by picking the best config for each toplevel vdev.  Once that's done,
 * we assemble the toplevel vdevs into a full config for the pool.  We make a
 * pass to fix up any incorrect paths, and then add it to the main list to
 * return to the user.
 */
static nvlist_t *
get_configs(libpc_handle_t *hdl, pool_list_t *pl, boolean_t active_ok,
    nvlist_t *policy)
{
	pool_entry_t *pe;
	vdev_entry_t *ve;
	config_entry_t *ce;
	nvlist_t *ret = NULL, *config = NULL, *tmp = NULL, *nvtop, *nvroot;
	nvlist_t **spares, **l2cache;
	uint_t i, nspares, nl2cache;
	boolean_t config_seen;
	uint64_t best_txg;
	char *name, *hostname = NULL;
	uint64_t guid;
	uint_t children = 0;
	nvlist_t **child = NULL;
	uint_t holes;
	uint64_t *hole_array, max_id;
	uint_t c;
	boolean_t isactive;
	uint64_t hostid;
	nvlist_t *nvl;
	boolean_t found_one = B_FALSE;
	boolean_t valid_top_config = B_FALSE;

	if (nvlist_alloc(&ret, 0, 0) != 0)
		goto nomem;

	for (pe = pl->pools; pe != NULL; pe = pe->pe_next) {
		uint64_t id, max_txg = 0;

		if (nvlist_alloc(&config, NV_UNIQUE_NAME, 0) != 0)
			goto nomem;
		config_seen = B_FALSE;

		/*
		 * Iterate over all toplevel vdevs.  Grab the pool configuration
		 * from the first one we find, and then go through the rest and
		 * add them as necessary to the 'vdevs' member of the config.
		 */
		for (ve = pe->pe_vdevs; ve != NULL; ve = ve->ve_next) {

			/*
			 * Determine the best configuration for this vdev by
			 * selecting the config with the latest transaction
			 * group.
			 */
			best_txg = 0;
			for (ce = ve->ve_configs; ce != NULL;
			    ce = ce->ce_next) {

				if (ce->ce_txg > best_txg) {
					tmp = ce->ce_config;
					best_txg = ce->ce_txg;
				}
			}

			/*
			 * We rely on the fact that the max txg for the
			 * pool will contain the most up-to-date information
			 * about the valid top-levels in the vdev namespace.
			 */
			if (best_txg > max_txg) {
				(void) nvlist_remove(config,
				    ZPOOL_CONFIG_VDEV_CHILDREN,
				    DATA_TYPE_UINT64);
				(void) nvlist_remove(config,
				    ZPOOL_CONFIG_HOLE_ARRAY,
				    DATA_TYPE_UINT64_ARRAY);

				max_txg = best_txg;
				hole_array = NULL;
				holes = 0;
				max_id = 0;
				valid_top_config = B_FALSE;

				if (nvlist_lookup_uint64(tmp,
				    ZPOOL_CONFIG_VDEV_CHILDREN, &max_id) == 0) {
					verify(nvlist_add_uint64(config,
					    ZPOOL_CONFIG_VDEV_CHILDREN,
					    max_id) == 0);
					valid_top_config = B_TRUE;
				}

				if (nvlist_lookup_uint64_array(tmp,
				    ZPOOL_CONFIG_HOLE_ARRAY, &hole_array,
				    &holes) == 0) {
					verify(nvlist_add_uint64_array(config,
					    ZPOOL_CONFIG_HOLE_ARRAY,
					    hole_array, holes) == 0);
				}
			}

			if (!config_seen) {
				/*
				 * Copy the relevant pieces of data to the pool
				 * configuration:
				 *
				 *	version
				 *	pool guid
				 *	name
				 *	comment (if available)
				 *	pool state
				 *	hostid (if available)
				 *	hostname (if available)
				 */
				uint64_t state, version;
				char *comment = NULL;

				version = fnvlist_lookup_uint64(tmp,
				    ZPOOL_CONFIG_VERSION);
				fnvlist_add_uint64(config,
				    ZPOOL_CONFIG_VERSION, version);
				guid = fnvlist_lookup_uint64(tmp,
				    ZPOOL_CONFIG_POOL_GUID);
				fnvlist_add_uint64(config,
				    ZPOOL_CONFIG_POOL_GUID, guid);
				name = fnvlist_lookup_string(tmp,
				    ZPOOL_CONFIG_POOL_NAME);
				fnvlist_add_string(config,
				    ZPOOL_CONFIG_POOL_NAME, name);

				if (nvlist_lookup_string(tmp,
				    ZPOOL_CONFIG_COMMENT, &comment) == 0)
					fnvlist_add_string(config,
					    ZPOOL_CONFIG_COMMENT, comment);

				state = fnvlist_lookup_uint64(tmp,
				    ZPOOL_CONFIG_POOL_STATE);
				fnvlist_add_uint64(config,
				    ZPOOL_CONFIG_POOL_STATE, state);

				hostid = 0;
				if (nvlist_lookup_uint64(tmp,
				    ZPOOL_CONFIG_HOSTID, &hostid) == 0) {
					fnvlist_add_uint64(config,
					    ZPOOL_CONFIG_HOSTID, hostid);
					hostname = fnvlist_lookup_string(tmp,
					    ZPOOL_CONFIG_HOSTNAME);
					fnvlist_add_string(config,
					    ZPOOL_CONFIG_HOSTNAME, hostname);
				}

				config_seen = B_TRUE;
			}

			/*
			 * Add this top-level vdev to the child array.
			 */
			verify(nvlist_lookup_nvlist(tmp,
			    ZPOOL_CONFIG_VDEV_TREE, &nvtop) == 0);
			verify(nvlist_lookup_uint64(nvtop, ZPOOL_CONFIG_ID,
			    &id) == 0);

			if (id >= children) {
				nvlist_t **newchild;

				newchild = zutil_alloc(hdl, (id + 1) *
				    sizeof (nvlist_t *));
				if (newchild == NULL)
					goto nomem;

				for (c = 0; c < children; c++)
					newchild[c] = child[c];

				free(child);
				child = newchild;
				children = id + 1;
			}
			if (nvlist_dup(nvtop, &child[id], 0) != 0)
				goto nomem;

		}

		/*
		 * If we have information about all the top-levels then
		 * clean up the nvlist which we've constructed. This
		 * means removing any extraneous devices that are
		 * beyond the valid range or adding devices to the end
		 * of our array which appear to be missing.
		 */
		if (valid_top_config) {
			if (max_id < children) {
				for (c = max_id; c < children; c++)
					nvlist_free(child[c]);
				children = max_id;
			} else if (max_id > children) {
				nvlist_t **newchild;

				newchild = zutil_alloc(hdl, (max_id) *
				    sizeof (nvlist_t *));
				if (newchild == NULL)
					goto nomem;

				for (c = 0; c < children; c++)
					newchild[c] = child[c];

				free(child);
				child = newchild;
				children = max_id;
			}
		}

		verify(nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID,
		    &guid) == 0);

		/*
		 * The vdev namespace may contain holes as a result of
		 * device removal. We must add them back into the vdev
		 * tree before we process any missing devices.
		 */
		if (holes > 0) {
			ASSERT(valid_top_config);

			for (c = 0; c < children; c++) {
				nvlist_t *holey;

				if (child[c] != NULL ||
				    !vdev_is_hole(hole_array, holes, c))
					continue;

				if (nvlist_alloc(&holey, NV_UNIQUE_NAME,
				    0) != 0)
					goto nomem;

				/*
				 * Holes in the namespace are treated as
				 * "hole" top-level vdevs and have a
				 * special flag set on them.
				 */
				if (nvlist_add_string(holey,
				    ZPOOL_CONFIG_TYPE,
				    VDEV_TYPE_HOLE) != 0 ||
				    nvlist_add_uint64(holey,
				    ZPOOL_CONFIG_ID, c) != 0 ||
				    nvlist_add_uint64(holey,
				    ZPOOL_CONFIG_GUID, 0ULL) != 0) {
					nvlist_free(holey);
					goto nomem;
				}
				child[c] = holey;
			}
		}

		/*
		 * Look for any missing top-level vdevs.  If this is the case,
		 * create a faked up 'missing' vdev as a placeholder.  We cannot
		 * simply compress the child array, because the kernel performs
		 * certain checks to make sure the vdev IDs match their location
		 * in the configuration.
		 */
		for (c = 0; c < children; c++) {
			if (child[c] == NULL) {
				nvlist_t *missing;
				if (nvlist_alloc(&missing, NV_UNIQUE_NAME,
				    0) != 0)
					goto nomem;
				if (nvlist_add_string(missing,
				    ZPOOL_CONFIG_TYPE,
				    VDEV_TYPE_MISSING) != 0 ||
				    nvlist_add_uint64(missing,
				    ZPOOL_CONFIG_ID, c) != 0 ||
				    nvlist_add_uint64(missing,
				    ZPOOL_CONFIG_GUID, 0ULL) != 0) {
					nvlist_free(missing);
					goto nomem;
				}
				child[c] = missing;
			}
		}

		/*
		 * Put all of this pool's top-level vdevs into a root vdev.
		 */
		if (nvlist_alloc(&nvroot, NV_UNIQUE_NAME, 0) != 0)
			goto nomem;
		if (nvlist_add_string(nvroot, ZPOOL_CONFIG_TYPE,
		    VDEV_TYPE_ROOT) != 0 ||
		    nvlist_add_uint64(nvroot, ZPOOL_CONFIG_ID, 0ULL) != 0 ||
		    nvlist_add_uint64(nvroot, ZPOOL_CONFIG_GUID, guid) != 0 ||
		    nvlist_add_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
		    child, children) != 0) {
			nvlist_free(nvroot);
			goto nomem;
		}

		for (c = 0; c < children; c++)
			nvlist_free(child[c]);
		free(child);
		children = 0;
		child = NULL;

		/*
		 * Go through and fix up any paths and/or devids based on our
		 * known list of vdev GUID -> path mappings.
		 */
		if (fix_paths(nvroot, pl->names) != 0) {
			nvlist_free(nvroot);
			goto nomem;
		}

		/*
		 * Add the root vdev to this pool's configuration.
		 */
		if (nvlist_add_nvlist(config, ZPOOL_CONFIG_VDEV_TREE,
		    nvroot) != 0) {
			nvlist_free(nvroot);
			goto nomem;
		}
		nvlist_free(nvroot);

		/*
		 * zdb uses this path to report on active pools that were
		 * imported or created using -R.
		 */
		if (active_ok)
			goto add_pool;

		/*
		 * Determine if this pool is currently active, in which case we
		 * can't actually import it.
		 */
		verify(nvlist_lookup_string(config, ZPOOL_CONFIG_POOL_NAME,
		    &name) == 0);
		verify(nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID,
		    &guid) == 0);

		if (zutil_pool_active(hdl, name, guid, &isactive) != 0)
			goto error;

		if (isactive) {
			nvlist_free(config);
			config = NULL;
			continue;
		}

		if (policy != NULL) {
			if (nvlist_add_nvlist(config, ZPOOL_LOAD_POLICY,
			    policy) != 0)
				goto nomem;
		}

		if ((nvl = zutil_refresh_config(hdl, config)) == NULL) {
			nvlist_free(config);
			config = NULL;
			continue;
		}

		nvlist_free(config);
		config = nvl;

		/*
		 * Go through and update the paths for spares, now that we have
		 * them.
		 */
		verify(nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE,
		    &nvroot) == 0);
		if (nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_SPARES,
		    &spares, &nspares) == 0) {
			for (i = 0; i < nspares; i++) {
				if (fix_paths(spares[i], pl->names) != 0)
					goto nomem;
			}
		}

		/*
		 * Update the paths for l2cache devices.
		 */
		if (nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_L2CACHE,
		    &l2cache, &nl2cache) == 0) {
			for (i = 0; i < nl2cache; i++) {
				if (fix_paths(l2cache[i], pl->names) != 0)
					goto nomem;
			}
		}

		/*
		 * Restore the original information read from the actual label.
		 */
		(void) nvlist_remove(config, ZPOOL_CONFIG_HOSTID,
		    DATA_TYPE_UINT64);
		(void) nvlist_remove(config, ZPOOL_CONFIG_HOSTNAME,
		    DATA_TYPE_STRING);
		if (hostid != 0) {
			verify(nvlist_add_uint64(config, ZPOOL_CONFIG_HOSTID,
			    hostid) == 0);
			verify(nvlist_add_string(config, ZPOOL_CONFIG_HOSTNAME,
			    hostname) == 0);
		}

add_pool:
		/*
		 * Add this pool to the list of configs.
		 */
		verify(nvlist_lookup_string(config, ZPOOL_CONFIG_POOL_NAME,
		    &name) == 0);
		if (nvlist_add_nvlist(ret, name, config) != 0)
			goto nomem;

		found_one = B_TRUE;
		nvlist_free(config);
		config = NULL;
	}

	if (!found_one) {
		nvlist_free(ret);
		ret = NULL;
	}

	return (ret);

nomem:
	(void) zutil_no_memory(hdl);
error:
	nvlist_free(config);
	nvlist_free(ret);
	for (c = 0; c < children; c++)
		nvlist_free(child[c]);
	free(child);

	return (NULL);
}

/*
 * Return the offset of the given label.
 */
static uint64_t
label_offset(uint64_t size, int l)
{
	ASSERT(P2PHASE_TYPED(size, sizeof (vdev_label_t), uint64_t) == 0);
	return (l * sizeof (vdev_label_t) + (l < VDEV_LABELS / 2 ?
	    0 : size - VDEV_LABELS * sizeof (vdev_label_t)));
}

/*
 * Given a file descriptor, read the label information and return an nvlist
 * describing the configuration, if there is one. The number of valid
 * labels found will be returned in num_labels when non-NULL.
 */
int
zpool_read_label(int fd, nvlist_t **config, int *num_labels)
{
	struct stat64 statbuf;
	int l, count = 0;
	vdev_label_t *label;
	nvlist_t *expected_config = NULL;
	uint64_t expected_guid = 0, size;

	*config = NULL;

	if (num_labels != NULL)
		*num_labels = 0;

	if (fstat64(fd, &statbuf) == -1)
		return (0);
	size = P2ALIGN_TYPED(statbuf.st_size, sizeof (vdev_label_t), uint64_t);

	if ((label = malloc(sizeof (vdev_label_t))) == NULL)
		return (-1);

	for (l = 0; l < VDEV_LABELS; l++) {
		uint64_t state, guid, txg;

		if (pread64(fd, label, sizeof (vdev_label_t),
		    label_offset(size, l)) != sizeof (vdev_label_t))
			continue;

		if (nvlist_unpack(label->vl_vdev_phys.vp_nvlist,
		    sizeof (label->vl_vdev_phys.vp_nvlist), config, 0) != 0)
			continue;

		if (nvlist_lookup_uint64(*config, ZPOOL_CONFIG_GUID,
		    &guid) != 0 || guid == 0) {
			nvlist_free(*config);
			continue;
		}

		if (nvlist_lookup_uint64(*config, ZPOOL_CONFIG_POOL_STATE,
		    &state) != 0 || state > POOL_STATE_L2CACHE) {
			nvlist_free(*config);
			continue;
		}

		if (state != POOL_STATE_SPARE && state != POOL_STATE_L2CACHE &&
		    (nvlist_lookup_uint64(*config, ZPOOL_CONFIG_POOL_TXG,
		    &txg) != 0 || txg == 0)) {
			nvlist_free(*config);
			continue;
		}

		if (expected_guid) {
			if (expected_guid == guid)
				count++;

			nvlist_free(*config);
		} else {
			expected_config = *config;
			expected_guid = guid;
			count++;
		}
	}

	if (num_labels != NULL)
		*num_labels = count;

	free(label);
	*config = expected_config;

	return (0);
}

static int
slice_cache_compare(const void *arg1, const void *arg2)
{
	const char  *nm1 = ((rdsk_node_t *)arg1)->rn_name;
	const char  *nm2 = ((rdsk_node_t *)arg2)->rn_name;
	char *nm1slice, *nm2slice;
	int rv;

	/*
	 * slices zero and two are the most likely to provide results,
	 * so put those first
	 */
	nm1slice = strstr(nm1, "s0");
	nm2slice = strstr(nm2, "s0");
	if (nm1slice && !nm2slice) {
		return (-1);
	}
	if (!nm1slice && nm2slice) {
		return (1);
	}
	nm1slice = strstr(nm1, "s2");
	nm2slice = strstr(nm2, "s2");
	if (nm1slice && !nm2slice) {
		return (-1);
	}
	if (!nm1slice && nm2slice) {
		return (1);
	}

	rv = strcmp(nm1, nm2);
	if (rv == 0)
		return (0);
	return (rv > 0 ? 1 : -1);
}

static void
check_one_slice(avl_tree_t *r, char *diskname, uint_t partno,
    diskaddr_t size, uint_t blksz)
{
	rdsk_node_t tmpnode;
	rdsk_node_t *node;
	char sname[MAXNAMELEN];

	tmpnode.rn_name = &sname[0];
	(void) snprintf(tmpnode.rn_name, MAXNAMELEN, "%s%u",
	    diskname, partno);
	/*
	 * protect against division by zero for disk labels that
	 * contain a bogus sector size
	 */
	if (blksz == 0)
		blksz = DEV_BSIZE;
	/* too small to contain a zpool? */
	if ((size < (SPA_MINDEVSIZE / blksz)) &&
	    (node = avl_find(r, &tmpnode, NULL)))
		node->rn_nozpool = B_TRUE;
}

static void
nozpool_all_slices(avl_tree_t *r, const char *sname)
{
	char diskname[MAXNAMELEN];
	char *ptr;
	int i;

	(void) strncpy(diskname, sname, MAXNAMELEN);
	if (((ptr = strrchr(diskname, 's')) == NULL) &&
	    ((ptr = strrchr(diskname, 'p')) == NULL))
		return;
	ptr[0] = 's';
	ptr[1] = '\0';
	for (i = 0; i < NDKMAP; i++)
		check_one_slice(r, diskname, i, 0, 1);
	ptr[0] = 'p';
	for (i = 0; i <= FD_NUMPART; i++)
		check_one_slice(r, diskname, i, 0, 1);
}

static void
check_slices(avl_tree_t *r, int fd, const char *sname)
{
	struct extvtoc vtoc;
	struct dk_gpt *gpt;
	char diskname[MAXNAMELEN];
	char *ptr;
	int i;

	(void) strncpy(diskname, sname, MAXNAMELEN);
	if ((ptr = strrchr(diskname, 's')) == NULL || !isdigit(ptr[1]))
		return;
	ptr[1] = '\0';

	if (read_extvtoc(fd, &vtoc) >= 0) {
		for (i = 0; i < NDKMAP; i++)
			check_one_slice(r, diskname, i,
			    vtoc.v_part[i].p_size, vtoc.v_sectorsz);
	} else if (efi_alloc_and_read(fd, &gpt) >= 0) {
		/*
		 * on x86 we'll still have leftover links that point
		 * to slices s[9-15], so use NDKMAP instead
		 */
		for (i = 0; i < NDKMAP; i++)
			check_one_slice(r, diskname, i,
			    gpt->efi_parts[i].p_size, gpt->efi_lbasize);
		/* nodes p[1-4] are never used with EFI labels */
		ptr[0] = 'p';
		for (i = 1; i <= FD_NUMPART; i++)
			check_one_slice(r, diskname, i, 0, 1);
		efi_free(gpt);
	}
}

void
zpool_open_func(void *arg)
{
	rdsk_node_t *rn = arg;
	struct stat64 statbuf;
	nvlist_t *config;
	int error;
	int num_labels = 0;
	int fd;

	if (rn->rn_nozpool)
		return;
	if ((fd = openat64(rn->rn_dfd, rn->rn_name, O_RDONLY)) < 0) {
		/* symlink to a device that's no longer there */
		if (errno == ENOENT)
			nozpool_all_slices(rn->rn_avl, rn->rn_name);
		return;
	}
	/*
	 * Ignore failed stats.  We only want regular
	 * files, character devs and block devs.
	 */
	if (fstat64(fd, &statbuf) != 0 ||
	    (!S_ISREG(statbuf.st_mode) &&
	    !S_ISCHR(statbuf.st_mode) &&
	    !S_ISBLK(statbuf.st_mode))) {
		(void) close(fd);
		return;
	}
	/* this file is too small to hold a zpool */
	if (S_ISREG(statbuf.st_mode) &&
	    statbuf.st_size < SPA_MINDEVSIZE) {
		(void) close(fd);
		return;
	} else if (!S_ISREG(statbuf.st_mode)) {
		/*
		 * Try to read the disk label first so we don't have to
		 * open a bunch of minor nodes that can't have a zpool.
		 */
		check_slices(rn->rn_avl, fd, rn->rn_name);
	}

	error = zpool_read_label(fd, &config, &num_labels);
	if (error != 0) {
		(void) close(fd);
		return;
	}

	if (num_labels == 0) {
		(void) close(fd);
		nvlist_free(config);
		return;
	}

	(void) close(fd);

	rn->rn_config = config;
	rn->rn_num_labels = num_labels;
}

/*
 * Given a list of directories to search, find all pools stored on disk.  This
 * includes partial pools which are not available to import.  If no args are
 * given (argc is 0), then the default directory (/dev/dsk) is searched.
 * poolname or guid (but not both) are provided by the caller when trying
 * to import a specific pool.
 */
static nvlist_t *
zpool_find_import_impl(libpc_handle_t *hdl, importargs_t *iarg)
{
	int i, dirs = iarg->paths;
	struct dirent64 *dp;
	char path[MAXPATHLEN];
	char *end, **dir = iarg->path;
	size_t pathleft;
	nvlist_t *ret = NULL;
	static char *default_dir = ZFS_DISK_ROOT;
	pool_list_t pools = { 0 };
	pool_entry_t *pe, *penext;
	vdev_entry_t *ve, *venext;
	config_entry_t *ce, *cenext;
	name_entry_t *ne, *nenext;
	avl_tree_t slice_cache;
	rdsk_node_t *slice;
	void *cookie;

	if (dirs == 0) {
		dirs = 1;
		dir = &default_dir;
	}

	/*
	 * Go through and read the label configuration information from every
	 * possible device, organizing the information according to pool GUID
	 * and toplevel GUID.
	 */
	for (i = 0; i < dirs; i++) {
		tpool_t *t;
		char rdsk[MAXPATHLEN];
		int dfd;
		boolean_t config_failed = B_FALSE;
		DIR *dirp;

		/* use realpath to normalize the path */
		if (realpath(dir[i], path) == 0) {
			(void) zutil_error_fmt(hdl, EZFS_BADPATH,
			    dgettext(TEXT_DOMAIN, "cannot open '%s'"), dir[i]);
			goto error;
		}
		end = &path[strlen(path)];
		*end++ = '/';
		*end = 0;
		pathleft = &path[sizeof (path)] - end;

		/*
		 * Using raw devices instead of block devices when we're
		 * reading the labels skips a bunch of slow operations during
		 * close(2) processing, so we replace /dev/dsk with /dev/rdsk.
		 */
		if (strcmp(path, ZFS_DISK_ROOTD) == 0)
			(void) strlcpy(rdsk, ZFS_RDISK_ROOTD, sizeof (rdsk));
		else
			(void) strlcpy(rdsk, path, sizeof (rdsk));

		if ((dfd = open64(rdsk, O_RDONLY)) < 0 ||
		    (dirp = fdopendir(dfd)) == NULL) {
			if (dfd >= 0)
				(void) close(dfd);
			zutil_error_aux(hdl, strerror(errno));
			(void) zutil_error_fmt(hdl, EZFS_BADPATH,
			    dgettext(TEXT_DOMAIN, "cannot open '%s'"),
			    rdsk);
			goto error;
		}

		avl_create(&slice_cache, slice_cache_compare,
		    sizeof (rdsk_node_t), offsetof(rdsk_node_t, rn_node));
		/*
		 * This is not MT-safe, but we have no MT consumers of libzutil
		 */
		while ((dp = readdir64(dirp)) != NULL) {
			char *name = dp->d_name;
			avl_index_t where;

			if (name[0] == '.' &&
			    (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
				continue;

			slice = zutil_alloc(hdl, sizeof (rdsk_node_t));
			slice->rn_name = name;
			slice->rn_avl = &slice_cache;
			slice->rn_dfd = dfd;
			slice->rn_hdl = hdl;
			slice->rn_nozpool = B_FALSE;
			/*
			 * readdir(3C) buffers several directory entries per
			 * actual read operation. It is possible that if
			 * directory entries appear between reading one chunk
			 * and the next, that the same entry name can end up
			 * being returned more than once. We need to check that
			 * this entry doesn't already exist before adding it to
			 * the tree.
			 */
			if (avl_find(&slice_cache, slice, &where) == NULL) {
				slice->rn_name = zutil_strdup(hdl, name);
				avl_insert(&slice_cache, slice, where);
			} else {
				free(slice);
			}
		}
		/*
		 * create a thread pool to do all of this in parallel;
		 * rn_nozpool is not protected, so this is racy in that
		 * multiple tasks could decide that the same slice can
		 * not hold a zpool, which is benign.  Also choose
		 * double the number of processors; we hold a lot of
		 * locks in the kernel, so going beyond this doesn't
		 * buy us much.
		 */
		t = tpool_create(1, 2 * sysconf(_SC_NPROCESSORS_ONLN),
		    0, NULL);
		for (slice = avl_first(&slice_cache); slice;
		    (slice = avl_walk(&slice_cache, slice,
		    AVL_AFTER)))
			(void) tpool_dispatch(t, zpool_open_func, slice);
		tpool_wait(t);
		tpool_destroy(t);

		cookie = NULL;
		while ((slice = avl_destroy_nodes(&slice_cache,
		    &cookie)) != NULL) {
			if (slice->rn_config != NULL && !config_failed) {
				nvlist_t *config = slice->rn_config;
				boolean_t matched = B_TRUE;

				if (iarg->poolname != NULL) {
					char *pname;

					matched = nvlist_lookup_string(config,
					    ZPOOL_CONFIG_POOL_NAME,
					    &pname) == 0 &&
					    strcmp(iarg->poolname, pname) == 0;
				} else if (iarg->guid != 0) {
					uint64_t this_guid;

					matched = nvlist_lookup_uint64(config,
					    ZPOOL_CONFIG_POOL_GUID,
					    &this_guid) == 0 &&
					    iarg->guid == this_guid;
				}
				if (matched) {
					/*
					 * use the non-raw path for the config
					 */
					(void) strlcpy(end, slice->rn_name,
					    pathleft);
					(void) add_config(hdl, &pools,
					    path, slice->rn_order,
					    slice->rn_num_labels, config);
				}
				nvlist_free(config);
			}
			free(slice->rn_name);
			free(slice);
		}
		avl_destroy(&slice_cache);

		(void) closedir(dirp);

		if (config_failed)
			goto error;
	}

	ret = get_configs(hdl, &pools, iarg->can_be_active, iarg->policy);

error:
	for (pe = pools.pools; pe != NULL; pe = penext) {
		penext = pe->pe_next;
		for (ve = pe->pe_vdevs; ve != NULL; ve = venext) {
			venext = ve->ve_next;
			for (ce = ve->ve_configs; ce != NULL; ce = cenext) {
				cenext = ce->ce_next;
				nvlist_free(ce->ce_config);
				free(ce);
			}
			free(ve);
		}
		free(pe);
	}

	for (ne = pools.names; ne != NULL; ne = nenext) {
		nenext = ne->ne_next;
		free(ne->ne_name);
		free(ne);
	}

	return (ret);
}

/*
 * Given a cache file, return the contents as a list of importable pools.
 * poolname or guid (but not both) are provided by the caller when trying
 * to import a specific pool.
 */
static nvlist_t *
zpool_find_import_cached(libpc_handle_t *hdl, const char *cachefile,
    const char *poolname, uint64_t guid)
{
	char *buf;
	int fd;
	struct stat64 statbuf;
	nvlist_t *raw, *src, *dst;
	nvlist_t *pools;
	nvpair_t *elem;
	char *name;
	uint64_t this_guid;
	boolean_t active;

	verify(poolname == NULL || guid == 0);

	if ((fd = open(cachefile, O_RDONLY)) < 0) {
		zutil_error_aux(hdl, "%s", strerror(errno));
		(void) zutil_error(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN, "failed to open cache file"));
		return (NULL);
	}

	if (fstat64(fd, &statbuf) != 0) {
		zutil_error_aux(hdl, "%s", strerror(errno));
		(void) close(fd);
		(void) zutil_error(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN, "failed to get size of cache file"));
		return (NULL);
	}

	if ((buf = zutil_alloc(hdl, statbuf.st_size)) == NULL) {
		(void) close(fd);
		return (NULL);
	}

	if (read(fd, buf, statbuf.st_size) != statbuf.st_size) {
		(void) close(fd);
		free(buf);
		(void) zutil_error(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN,
		    "failed to read cache file contents"));
		return (NULL);
	}

	(void) close(fd);

	if (nvlist_unpack(buf, statbuf.st_size, &raw, 0) != 0) {
		free(buf);
		(void) zutil_error(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN,
		    "invalid or corrupt cache file contents"));
		return (NULL);
	}

	free(buf);

	/*
	 * Go through and get the current state of the pools and refresh their
	 * state.
	 */
	if (nvlist_alloc(&pools, 0, 0) != 0) {
		(void) zutil_no_memory(hdl);
		nvlist_free(raw);
		return (NULL);
	}

	elem = NULL;
	while ((elem = nvlist_next_nvpair(raw, elem)) != NULL) {
		src = fnvpair_value_nvlist(elem);

		name = fnvlist_lookup_string(src, ZPOOL_CONFIG_POOL_NAME);
		if (poolname != NULL && strcmp(poolname, name) != 0)
			continue;

		this_guid = fnvlist_lookup_uint64(src, ZPOOL_CONFIG_POOL_GUID);
		if (guid != 0 && guid != this_guid)
			continue;

		if (zutil_pool_active(hdl, name, this_guid, &active) != 0) {
			nvlist_free(raw);
			nvlist_free(pools);
			return (NULL);
		}

		if (active)
			continue;

		if (nvlist_add_string(src, ZPOOL_CONFIG_CACHEFILE,
		    cachefile) != 0) {
			(void) zutil_no_memory(hdl);
			nvlist_free(raw);
			nvlist_free(pools);
			return (NULL);
		}

		if ((dst = zutil_refresh_config(hdl, src)) == NULL) {
			nvlist_free(raw);
			nvlist_free(pools);
			return (NULL);
		}

		if (nvlist_add_nvlist(pools, nvpair_name(elem), dst) != 0) {
			(void) zutil_no_memory(hdl);
			nvlist_free(dst);
			nvlist_free(raw);
			nvlist_free(pools);
			return (NULL);
		}
		nvlist_free(dst);
	}

	nvlist_free(raw);
	return (pools);
}

nvlist_t *
zpool_search_import(void *hdl, importargs_t *import,
    const pool_config_ops_t *pco)
{
	libpc_handle_t handle = { 0 };
	nvlist_t *pools = NULL;

	handle.lpc_lib_handle = hdl;
	handle.lpc_ops = pco;
	handle.lpc_printerr = B_TRUE;

	verify(import->poolname == NULL || import->guid == 0);

	if (import->cachefile != NULL)
		pools = zpool_find_import_cached(&handle, import->cachefile,
		    import->poolname, import->guid);
	else
		pools = zpool_find_import_impl(&handle, import);

	if ((pools == NULL || nvlist_empty(pools)) &&
	    handle.lpc_open_access_error && geteuid() != 0) {
		(void) zutil_error(&handle, EZFS_EACESS, dgettext(TEXT_DOMAIN,
		    "no pools found"));
	}

	return (pools);
}

static boolean_t
pool_match(nvlist_t *cfg, char *tgt)
{
	uint64_t v, guid = strtoull(tgt, NULL, 0);
	char *s;

	if (guid != 0) {
		if (nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_POOL_GUID, &v) == 0)
			return (v == guid);
	} else {
		if (nvlist_lookup_string(cfg, ZPOOL_CONFIG_POOL_NAME, &s) == 0)
			return (strcmp(s, tgt) == 0);
	}
	return (B_FALSE);
}

int
zpool_find_config(void *hdl, const char *target, nvlist_t **configp,
    importargs_t *args, const pool_config_ops_t *pco)
{
	nvlist_t *pools;
	nvlist_t *match = NULL;
	nvlist_t *config = NULL;
	char *sepp = NULL;
	int count = 0;
	char *targetdup = strdup(target);

	*configp = NULL;

	if ((sepp = strpbrk(targetdup, "/@")) != NULL) {
		*sepp = '\0';
	}

	pools = zpool_search_import(hdl, args, pco);

	if (pools != NULL) {
		nvpair_t *elem = NULL;
		while ((elem = nvlist_next_nvpair(pools, elem)) != NULL) {
			VERIFY0(nvpair_value_nvlist(elem, &config));
			if (pool_match(config, targetdup)) {
				count++;
				if (match != NULL) {
					/* multiple matches found */
					continue;
				} else {
					match = config;
				}
			}
		}
	}

	if (count == 0) {
		free(targetdup);
		return (ENOENT);
	}

	if (count > 1) {
		free(targetdup);
		return (EINVAL);
	}

	*configp = match;
	free(targetdup);

	return (0);
}
