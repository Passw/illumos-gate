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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2018 by Delphix. All rights reserved.
 * Copyright 2016 Nexenta Systems, Inc.  All rights reserved.
 * Copyright 2020 Joyent, Inc.
 * Copyright 2020 Joshua M. Clulow <josh@sysmgr.org>
 * Copyright 2022 Tintri by DDN, Inc. All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/spa_impl.h>
#include <sys/refcount.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_trim.h>
#include <sys/abd.h>
#include <sys/fs/zfs.h>
#include <sys/zio.h>
#include <sys/sunldi.h>
#include <sys/efi_partition.h>
#include <sys/fm/fs/zfs.h>
#include <sys/ddi.h>

/*
 * Tunable to disable TRIM in case we're using a problematic SSD.
 */
uint_t zfs_no_trim = 0;

/*
 * Tunable parameter for debugging or performance analysis. Setting this
 * will cause pool corruption on power loss if a volatile out-of-order
 * write cache is enabled.
 */
boolean_t zfs_nocacheflush = B_FALSE;

/*
 * Virtual device vector for disks.
 */

extern ldi_ident_t zfs_li;

static void vdev_disk_close(vdev_t *);

typedef struct vdev_disk {
	ddi_devid_t	vd_devid;
	char		*vd_minor;
	ldi_handle_t	vd_lh;
	list_t		vd_ldi_cbs;
	boolean_t	vd_ldi_offline;
} vdev_disk_t;

typedef struct vdev_disk_buf {
	buf_t	vdb_buf;
	zio_t	*vdb_io;
} vdev_disk_buf_t;

typedef struct vdev_disk_ldi_cb {
	list_node_t		lcb_next;
	ldi_callback_id_t	lcb_id;
} vdev_disk_ldi_cb_t;

/*
 * Bypass the devid when opening a disk vdev.
 * There have been issues where the devids of several devices were shuffled,
 * causing pool open failures. Note, that this flag is intended to be used
 * for pool recovery only.
 *
 * Note that if a pool is imported with the devids bypassed, all its vdevs will
 * cease storing devid information permanently. In practice, the devid is rarely
 * useful as vdev paths do not tend to change unless the hardware is
 * reconfigured. That said, if the paths do change and a pool fails to open
 * automatically at boot, a simple zpool import should re-scan the paths and fix
 * the issue.
 */
boolean_t vdev_disk_bypass_devid = B_FALSE;

static void
vdev_disk_alloc(vdev_t *vd)
{
	vdev_disk_t *dvd;

	dvd = vd->vdev_tsd = kmem_zalloc(sizeof (vdev_disk_t), KM_SLEEP);
	/*
	 * Create the LDI event callback list.
	 */
	list_create(&dvd->vd_ldi_cbs, sizeof (vdev_disk_ldi_cb_t),
	    offsetof(vdev_disk_ldi_cb_t, lcb_next));
}

static void
vdev_disk_free(vdev_t *vd)
{
	vdev_disk_t *dvd = vd->vdev_tsd;
	vdev_disk_ldi_cb_t *lcb;

	if (dvd == NULL)
		return;

	/*
	 * We have already closed the LDI handle. Clean up the LDI event
	 * callbacks and free vd->vdev_tsd.
	 */
	while ((lcb = list_head(&dvd->vd_ldi_cbs)) != NULL) {
		list_remove(&dvd->vd_ldi_cbs, lcb);
		(void) ldi_ev_remove_callbacks(lcb->lcb_id);
		kmem_free(lcb, sizeof (vdev_disk_ldi_cb_t));
	}
	list_destroy(&dvd->vd_ldi_cbs);
	kmem_free(dvd, sizeof (vdev_disk_t));
	vd->vdev_tsd = NULL;
}

static int
vdev_disk_off_notify(ldi_handle_t lh __unused, ldi_ev_cookie_t ecookie,
    void *arg, void *ev_data __unused)
{
	vdev_t *vd = (vdev_t *)arg;
	vdev_disk_t *dvd = vd->vdev_tsd;

	/*
	 * Ignore events other than offline.
	 */
	if (strcmp(ldi_ev_get_type(ecookie), LDI_EV_OFFLINE) != 0)
		return (LDI_EV_SUCCESS);

	/*
	 * Tell any new threads that stumble upon this vdev that they should not
	 * try to do I/O.
	 */
	dvd->vd_ldi_offline = B_TRUE;

	/*
	 * Request that the spa_async_thread mark the device as REMOVED and
	 * notify FMA of the removal.  This should also trigger a vdev_close()
	 * in the async thread.
	 */
	zfs_post_remove(vd->vdev_spa, vd);
	vd->vdev_remove_wanted = B_TRUE;
	spa_async_request(vd->vdev_spa, SPA_ASYNC_REMOVE);

	return (LDI_EV_SUCCESS);
}

static void
vdev_disk_off_finalize(ldi_handle_t lh __unused, ldi_ev_cookie_t ecookie,
    int ldi_result, void *arg, void *ev_data __unused)
{
	vdev_t *vd = (vdev_t *)arg;

	/*
	 * Ignore events other than offline.
	 */
	if (strcmp(ldi_ev_get_type(ecookie), LDI_EV_OFFLINE) != 0)
		return;

	/*
	 * Request that the vdev be reopened if the offline state change was
	 * unsuccessful.
	 */
	if (ldi_result != LDI_EV_SUCCESS) {
		vd->vdev_probe_wanted = B_TRUE;
		spa_async_request(vd->vdev_spa, SPA_ASYNC_PROBE);
	}
}

static ldi_ev_callback_t vdev_disk_off_callb = {
	.cb_vers = LDI_EV_CB_VERS,
	.cb_notify = vdev_disk_off_notify,
	.cb_finalize = vdev_disk_off_finalize
};

static void
vdev_disk_dgrd_finalize(ldi_handle_t lh __unused, ldi_ev_cookie_t ecookie,
    int ldi_result, void *arg, void *ev_data __unused)
{
	vdev_t *vd = (vdev_t *)arg;

	/*
	 * Ignore events other than degrade.
	 */
	if (strcmp(ldi_ev_get_type(ecookie), LDI_EV_DEGRADE) != 0)
		return;

	/*
	 * Degrade events always succeed. Mark the vdev as degraded.
	 * This status is purely informative for the user.
	 */
	(void) vdev_degrade(vd->vdev_spa, vd->vdev_guid, 0);
}

static ldi_ev_callback_t vdev_disk_dgrd_callb = {
	.cb_vers = LDI_EV_CB_VERS,
	.cb_notify = NULL,
	.cb_finalize = vdev_disk_dgrd_finalize
};

static void
vdev_disk_hold(vdev_t *vd)
{
	ddi_devid_t devid;
	char *minor;

	ASSERT(spa_config_held(vd->vdev_spa, SCL_STATE, RW_WRITER));

	/*
	 * We must have a pathname, and it must be absolute.
	 */
	if (vd->vdev_path == NULL || vd->vdev_path[0] != '/')
		return;

	/*
	 * Only prefetch path and devid info if the device has
	 * never been opened.
	 */
	if (vd->vdev_tsd != NULL)
		return;

	if (vd->vdev_wholedisk == -1ULL) {
		size_t len = strlen(vd->vdev_path) + 3;
		char *buf = kmem_alloc(len, KM_SLEEP);

		(void) snprintf(buf, len, "%ss0", vd->vdev_path);

		(void) ldi_vp_from_name(buf, &vd->vdev_name_vp);
		kmem_free(buf, len);
	}

	if (vd->vdev_name_vp == NULL)
		(void) ldi_vp_from_name(vd->vdev_path, &vd->vdev_name_vp);

	if (vd->vdev_devid != NULL &&
	    ddi_devid_str_decode(vd->vdev_devid, &devid, &minor) == 0) {
		(void) ldi_vp_from_devid(devid, minor, &vd->vdev_devid_vp);
		ddi_devid_str_free(minor);
		ddi_devid_free(devid);
	}
}

static void
vdev_disk_rele(vdev_t *vd)
{
	ASSERT(spa_config_held(vd->vdev_spa, SCL_STATE, RW_WRITER));

	if (vd->vdev_name_vp) {
		VN_RELE_ASYNC(vd->vdev_name_vp,
		    dsl_pool_vnrele_taskq(vd->vdev_spa->spa_dsl_pool));
		vd->vdev_name_vp = NULL;
	}
	if (vd->vdev_devid_vp) {
		VN_RELE_ASYNC(vd->vdev_devid_vp,
		    dsl_pool_vnrele_taskq(vd->vdev_spa->spa_dsl_pool));
		vd->vdev_devid_vp = NULL;
	}
}

/*
 * We want to be loud in DEBUG kernels when DKIOCGMEDIAINFOEXT fails, or when
 * even a fallback to DKIOCGMEDIAINFO fails.
 */
#ifdef DEBUG
#define	VDEV_DEBUG(...)	cmn_err(CE_NOTE, __VA_ARGS__)
#else
#define	VDEV_DEBUG(...)	/* Nothing... */
#endif

static int
vdev_disk_open(vdev_t *vd, uint64_t *psize, uint64_t *max_psize,
    uint64_t *ashift)
{
	spa_t *spa = vd->vdev_spa;
	vdev_disk_t *dvd = vd->vdev_tsd;
	ldi_ev_cookie_t ecookie;
	vdev_disk_ldi_cb_t *lcb;
	union {
		struct dk_minfo_ext ude;
		struct dk_minfo ud;
	} dks;
	struct dk_minfo_ext *dkmext = &dks.ude;
	struct dk_minfo *dkm = &dks.ud;
	int error, can_free;
	dev_t dev;
	int otyp;
	boolean_t validate_devid = B_FALSE;
	uint64_t capacity = 0, blksz = 0, pbsize;
	const char *rdpath = vdev_disk_preroot_force_path();

	/*
	 * We must have a pathname, and it must be absolute.
	 */
	if (vd->vdev_path == NULL || vd->vdev_path[0] != '/') {
		vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Reopen the device if it's not currently open. Otherwise,
	 * just update the physical size of the device.
	 */
	if (dvd != NULL) {
		ASSERT(vd->vdev_reopening);
		goto skip_open;
	}

	/*
	 * Create vd->vdev_tsd.
	 */
	vdev_disk_alloc(vd);
	dvd = vd->vdev_tsd;

	/*
	 * Allow bypassing the devid.
	 */
	if (vd->vdev_devid != NULL &&
	    (vdev_disk_bypass_devid || rdpath != NULL)) {
		vdev_dbgmsg(vd, "vdev_disk_open, devid %s bypassed",
		    vd->vdev_devid);
		spa_strfree(vd->vdev_devid);
		vd->vdev_devid = NULL;
	}

	/*
	 * When opening a disk device, we want to preserve the user's original
	 * intent.  We always want to open the device by the path the user gave
	 * us, even if it is one of multiple paths to the same device.  But we
	 * also want to be able to survive disks being removed/recabled.
	 * Therefore the sequence of opening devices is:
	 *
	 * 1. Try opening the device by path.  For legacy pools without the
	 *    'whole_disk' property, attempt to fix the path by appending 's0'.
	 *
	 * 2. If the devid of the device matches the stored value, return
	 *    success.
	 *
	 * 3. Otherwise, the device may have moved.  Try opening the device
	 *    by the devid instead.
	 */
	if (vd->vdev_devid != NULL) {
		if (ddi_devid_str_decode(vd->vdev_devid, &dvd->vd_devid,
		    &dvd->vd_minor) != 0) {
			vdev_dbgmsg(vd,
			    "vdev_disk_open, invalid devid %s bypassed",
			    vd->vdev_devid);
			spa_strfree(vd->vdev_devid);
			vd->vdev_devid = NULL;
		}
	}

	error = EINVAL;		/* presume failure */

	if (rdpath != NULL) {
		/*
		 * We have been asked to open only a specific root device, and
		 * to fail otherwise.
		 */
		error = ldi_open_by_name((char *)rdpath, spa_mode(spa), kcred,
		    &dvd->vd_lh, zfs_li);
		validate_devid = B_TRUE;
		goto rootdisk_only;
	}

	if (vd->vdev_path != NULL) {
		if (vd->vdev_wholedisk == -1ULL) {
			size_t len = strlen(vd->vdev_path) + 3;
			char *buf = kmem_alloc(len, KM_SLEEP);

			(void) snprintf(buf, len, "%ss0", vd->vdev_path);

			error = ldi_open_by_name(buf, spa_mode(spa), kcred,
			    &dvd->vd_lh, zfs_li);
			if (error == 0) {
				spa_strfree(vd->vdev_path);
				vd->vdev_path = buf;
				vd->vdev_wholedisk = 1ULL;
			} else {
				kmem_free(buf, len);
			}
		}

		/*
		 * If we have not yet opened the device, try to open it by the
		 * specified path.
		 */
		if (error != 0) {
			error = ldi_open_by_name(vd->vdev_path, spa_mode(spa),
			    kcred, &dvd->vd_lh, zfs_li);
		}

		/*
		 * Compare the devid to the stored value.
		 */
		if (error == 0 && vd->vdev_devid != NULL) {
			ddi_devid_t devid = NULL;

			if (ldi_get_devid(dvd->vd_lh, &devid) != 0) {
				/*
				 * We expected a devid on this device but it no
				 * longer appears to have one.  The validation
				 * step may need to remove it from the
				 * configuration.
				 */
				validate_devid = B_TRUE;

			} else if (ddi_devid_compare(devid, dvd->vd_devid) !=
			    0) {
				/*
				 * A mismatch here is unexpected, log it.
				 */
				char *devid_str = ddi_devid_str_encode(devid,
				    dvd->vd_minor);
				vdev_dbgmsg(vd, "vdev_disk_open: devid "
				    "mismatch: %s != %s", vd->vdev_devid,
				    devid_str);
				cmn_err(CE_NOTE, "vdev_disk_open %s: devid "
				    "mismatch: %s != %s", vd->vdev_path,
				    vd->vdev_devid, devid_str);
				ddi_devid_str_free(devid_str);

				error = SET_ERROR(EINVAL);
				(void) ldi_close(dvd->vd_lh, spa_mode(spa),
				    kcred);
				dvd->vd_lh = NULL;
			}

			if (devid != NULL) {
				ddi_devid_free(devid);
			}
		}

		/*
		 * If we succeeded in opening the device, but 'vdev_wholedisk'
		 * is not yet set, then this must be a slice.
		 */
		if (error == 0 && vd->vdev_wholedisk == -1ULL)
			vd->vdev_wholedisk = 0;
	}

	/*
	 * If we were unable to open by path, or the devid check fails, open by
	 * devid instead.
	 */
	if (error != 0 && vd->vdev_devid != NULL) {
		error = ldi_open_by_devid(dvd->vd_devid, dvd->vd_minor,
		    spa_mode(spa), kcred, &dvd->vd_lh, zfs_li);
		if (error != 0) {
			vdev_dbgmsg(vd, "Failed to open by devid (%s)",
			    vd->vdev_devid);
		}
	}

	/*
	 * If all else fails, then try opening by physical path (if available)
	 * or the logical path (if we failed due to the devid check).  While not
	 * as reliable as the devid, this will give us something, and the higher
	 * level vdev validation will prevent us from opening the wrong device.
	 */
	if (error != 0) {
		validate_devid = B_TRUE;

		if (vd->vdev_physpath != NULL &&
		    (dev = ddi_pathname_to_dev_t(vd->vdev_physpath)) != NODEV) {
			error = ldi_open_by_dev(&dev, OTYP_BLK, spa_mode(spa),
			    kcred, &dvd->vd_lh, zfs_li);
		}

		/*
		 * Note that we don't support the legacy auto-wholedisk support
		 * as above.  This hasn't been used in a very long time and we
		 * don't need to propagate its oddities to this edge condition.
		 */
		if (error != 0 && vd->vdev_path != NULL) {
			error = ldi_open_by_name(vd->vdev_path, spa_mode(spa),
			    kcred, &dvd->vd_lh, zfs_li);
		}
	}

	/*
	 * If this is early in boot, a sweep of available block devices may
	 * locate an alternative path that we can try.
	 */
	if (error != 0) {
		const char *altdevpath = vdev_disk_preroot_lookup(
		    spa_guid(spa), vd->vdev_guid);

		if (altdevpath != NULL) {
			vdev_dbgmsg(vd, "Trying alternate preroot path (%s)",
			    altdevpath);

			validate_devid = B_TRUE;

			if ((error = ldi_open_by_name((char *)altdevpath,
			    spa_mode(spa), kcred, &dvd->vd_lh, zfs_li)) != 0) {
				vdev_dbgmsg(vd, "Failed to open by preroot "
				    "path (%s)", altdevpath);
			}
		}
	}

rootdisk_only:
	if (error != 0) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		vdev_dbgmsg(vd, "vdev_disk_open: failed to open [error=%d]",
		    error);
		return (error);
	}

	/*
	 * Now that the device has been successfully opened, update the devid
	 * if necessary.
	 */
	if (validate_devid) {
		ddi_devid_t devid = NULL;
		char *minorname = NULL;
		char *vd_devid = NULL;
		boolean_t remove = B_FALSE, update = B_FALSE;

		/*
		 * Get the current devid and minor name for the device we
		 * opened.
		 */
		if (ldi_get_devid(dvd->vd_lh, &devid) != 0 ||
		    ldi_get_minor_name(dvd->vd_lh, &minorname) != 0) {
			/*
			 * If we are unable to get the devid or the minor name
			 * for the device, we need to remove them from the
			 * configuration to prevent potential inconsistencies.
			 */
			if (dvd->vd_minor != NULL || dvd->vd_devid != NULL ||
			    vd->vdev_devid != NULL) {
				/*
				 * We only need to remove the devid if one
				 * exists.
				 */
				remove = B_TRUE;
			}

		} else if (dvd->vd_devid == NULL || dvd->vd_minor == NULL) {
			/*
			 * There was previously no devid at all so we need to
			 * add one.
			 */
			update = B_TRUE;

		} else if (ddi_devid_compare(devid, dvd->vd_devid) != 0 ||
		    strcmp(minorname, dvd->vd_minor) != 0) {
			/*
			 * The devid or minor name on file does not match the
			 * one from the opened device.
			 */
			update = B_TRUE;
		}

		if (update) {
			/*
			 * Render the new devid and minor name as a string for
			 * logging and to store in the vdev configuration.
			 */
			vd_devid = ddi_devid_str_encode(devid, minorname);
		}

		if (update || remove) {
			vdev_dbgmsg(vd, "vdev_disk_open: update devid from "
			    "'%s' to '%s'",
			    vd->vdev_devid != NULL ? vd->vdev_devid : "<none>",
			    vd_devid != NULL ? vd_devid : "<none>");
			cmn_err(CE_NOTE, "vdev_disk_open %s: update devid "
			    "from '%s' to '%s'",
			    vd->vdev_path != NULL ? vd->vdev_path : "?",
			    vd->vdev_devid != NULL ? vd->vdev_devid : "<none>",
			    vd_devid != NULL ? vd_devid : "<none>");

			/*
			 * Remove and free any existing values.
			 */
			if (dvd->vd_minor != NULL) {
				ddi_devid_str_free(dvd->vd_minor);
				dvd->vd_minor = NULL;
			}
			if (dvd->vd_devid != NULL) {
				ddi_devid_free(dvd->vd_devid);
				dvd->vd_devid = NULL;
			}
			if (vd->vdev_devid != NULL) {
				spa_strfree(vd->vdev_devid);
				vd->vdev_devid = NULL;
			}
		}

		if (update) {
			/*
			 * Install the new values.
			 */
			vd->vdev_devid = vd_devid;
			dvd->vd_minor = minorname;
			dvd->vd_devid = devid;

		} else {
			if (devid != NULL) {
				ddi_devid_free(devid);
			}
			if (minorname != NULL) {
				kmem_free(minorname, strlen(minorname) + 1);
			}
		}
	}

	/*
	 * Once a device is opened, verify that the physical device path (if
	 * available) is up to date.
	 */
	if (ldi_get_dev(dvd->vd_lh, &dev) == 0 &&
	    ldi_get_otyp(dvd->vd_lh, &otyp) == 0) {
		char *physpath, *minorname;

		physpath = kmem_alloc(MAXPATHLEN, KM_SLEEP);
		minorname = NULL;
		if (ddi_dev_pathname(dev, otyp, physpath) == 0 &&
		    ldi_get_minor_name(dvd->vd_lh, &minorname) == 0 &&
		    (vd->vdev_physpath == NULL ||
		    strcmp(vd->vdev_physpath, physpath) != 0)) {
			if (vd->vdev_physpath)
				spa_strfree(vd->vdev_physpath);
			(void) strlcat(physpath, ":", MAXPATHLEN);
			(void) strlcat(physpath, minorname, MAXPATHLEN);
			vd->vdev_physpath = spa_strdup(physpath);
		}
		if (minorname)
			kmem_free(minorname, strlen(minorname) + 1);
		kmem_free(physpath, MAXPATHLEN);
	}

	/*
	 * Register callbacks for the LDI offline event.
	 */
	if (ldi_ev_get_cookie(dvd->vd_lh, LDI_EV_OFFLINE, &ecookie) ==
	    LDI_EV_SUCCESS) {
		lcb = kmem_zalloc(sizeof (vdev_disk_ldi_cb_t), KM_SLEEP);
		list_insert_tail(&dvd->vd_ldi_cbs, lcb);
		(void) ldi_ev_register_callbacks(dvd->vd_lh, ecookie,
		    &vdev_disk_off_callb, (void *) vd, &lcb->lcb_id);
	}

	/*
	 * Register callbacks for the LDI degrade event.
	 */
	if (ldi_ev_get_cookie(dvd->vd_lh, LDI_EV_DEGRADE, &ecookie) ==
	    LDI_EV_SUCCESS) {
		lcb = kmem_zalloc(sizeof (vdev_disk_ldi_cb_t), KM_SLEEP);
		list_insert_tail(&dvd->vd_ldi_cbs, lcb);
		(void) ldi_ev_register_callbacks(dvd->vd_lh, ecookie,
		    &vdev_disk_dgrd_callb, (void *) vd, &lcb->lcb_id);
	}

skip_open:
	/*
	 * Determine the actual size of the device.
	 */
	if (ldi_get_size(dvd->vd_lh, psize) != 0) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		vdev_dbgmsg(vd, "vdev_disk_open: failed to get size");
		return (SET_ERROR(EINVAL));
	}

	*max_psize = *psize;

	/*
	 * Determine the device's minimum transfer size.
	 * If the ioctl isn't supported, assume DEV_BSIZE.
	 */
	if ((error = ldi_ioctl(dvd->vd_lh, DKIOCGMEDIAINFOEXT,
	    (intptr_t)dkmext, FKIOCTL, kcred, NULL)) == 0) {
		capacity = dkmext->dki_capacity - 1;
		blksz = dkmext->dki_lbsize;
		pbsize = dkmext->dki_pbsize;
	} else if ((error = ldi_ioctl(dvd->vd_lh, DKIOCGMEDIAINFO,
	    (intptr_t)dkm, FKIOCTL, kcred, NULL)) == 0) {
		VDEV_DEBUG(
		    "vdev_disk_open(\"%s\"): fallback to DKIOCGMEDIAINFO\n",
		    vd->vdev_path);
		capacity = dkm->dki_capacity - 1;
		blksz = dkm->dki_lbsize;
		pbsize = blksz;
	} else {
		VDEV_DEBUG("vdev_disk_open(\"%s\"): "
		    "both DKIOCGMEDIAINFO{,EXT} calls failed, %d\n",
		    vd->vdev_path, error);
		pbsize = DEV_BSIZE;
	}

	*ashift = highbit64(MAX(pbsize, SPA_MINBLOCKSIZE)) - 1;

	if (vd->vdev_wholedisk == 1) {
		int wce = 1;

		if (error == 0) {
			/*
			 * If we have the capability to expand, we'd have
			 * found out via success from DKIOCGMEDIAINFO{,EXT}.
			 * Adjust max_psize upward accordingly since we know
			 * we own the whole disk now.
			 */
			*max_psize = capacity * blksz;
		}

		/*
		 * Since we own the whole disk, try to enable disk write
		 * caching.  We ignore errors because it's OK if we can't do it.
		 */
		(void) ldi_ioctl(dvd->vd_lh, DKIOCSETWCE, (intptr_t)&wce,
		    FKIOCTL, kcred, NULL);
	}

	/*
	 * Clear the nowritecache bit, so that on a vdev_reopen() we will
	 * try again.
	 */
	vd->vdev_nowritecache = B_FALSE;

	if (ldi_ioctl(dvd->vd_lh, DKIOC_CANFREE, (intptr_t)&can_free, FKIOCTL,
	    kcred, NULL) == 0 && can_free == 1) {
		vd->vdev_has_trim = B_TRUE;
	} else {
		vd->vdev_has_trim = B_FALSE;
	}

	if (zfs_no_trim == 1)
		vd->vdev_has_trim = B_FALSE;

	/* Currently only supported for ZoL. */
	vd->vdev_has_securetrim = B_FALSE;

	/* Inform the ZIO pipeline that we are non-rotational */
	vd->vdev_nonrot = B_FALSE;
	if (ldi_prop_exists(dvd->vd_lh, DDI_PROP_DONTPASS | DDI_PROP_NOTPROM,
	    "device-solid-state")) {
		if (ldi_prop_get_int(dvd->vd_lh,
		    LDI_DEV_T_ANY | DDI_PROP_DONTPASS | DDI_PROP_NOTPROM,
		    "device-solid-state", B_FALSE) != 0)
			vd->vdev_nonrot = B_TRUE;
	}

	return (0);
}

static void
vdev_disk_close(vdev_t *vd)
{
	vdev_disk_t *dvd = vd->vdev_tsd;

	if (vd->vdev_reopening || dvd == NULL)
		return;

	if (dvd->vd_minor != NULL) {
		ddi_devid_str_free(dvd->vd_minor);
		dvd->vd_minor = NULL;
	}

	if (dvd->vd_devid != NULL) {
		ddi_devid_free(dvd->vd_devid);
		dvd->vd_devid = NULL;
	}

	if (dvd->vd_lh != NULL) {
		(void) ldi_close(dvd->vd_lh, spa_mode(vd->vdev_spa), kcred);
		dvd->vd_lh = NULL;
	}

	vd->vdev_delayed_close = B_FALSE;
	vdev_disk_free(vd);
}

static int
vdev_disk_ldi_physio(ldi_handle_t vd_lh, caddr_t data,
    size_t size, uint64_t offset, int flags)
{
	buf_t *bp;
	int error = 0;

	if (vd_lh == NULL)
		return (SET_ERROR(EINVAL));

	ASSERT(flags & B_READ || flags & B_WRITE);

	bp = getrbuf(KM_SLEEP);
	bp->b_flags = flags | B_BUSY | B_NOCACHE | B_FAILFAST;
	bp->b_bcount = size;
	bp->b_un.b_addr = (void *)data;
	bp->b_lblkno = lbtodb(offset);
	bp->b_bufsize = size;

	error = ldi_strategy(vd_lh, bp);
	ASSERT(error == 0);
	if ((error = biowait(bp)) == 0 && bp->b_resid != 0)
		error = SET_ERROR(EIO);
	freerbuf(bp);

	return (error);
}

static int
vdev_disk_dumpio(vdev_t *vd, caddr_t data, size_t size,
    uint64_t offset, uint64_t origoffset __unused, boolean_t doread,
    boolean_t isdump)
{
	vdev_disk_t *dvd = vd->vdev_tsd;
	int flags = doread ? B_READ : B_WRITE;

	/*
	 * If the vdev is closed, it's likely in the REMOVED or FAULTED state.
	 * Nothing to be done here but return failure.
	 */
	if (dvd == NULL || dvd->vd_ldi_offline) {
		return (SET_ERROR(ENXIO));
	}

	ASSERT(vd->vdev_ops == &vdev_disk_ops);

	offset += VDEV_LABEL_START_SIZE;

	/*
	 * If in the context of an active crash dump, use the ldi_dump(9F)
	 * call instead of ldi_strategy(9F) as usual.
	 */
	if (isdump) {
		ASSERT3P(dvd, !=, NULL);
		return (ldi_dump(dvd->vd_lh, data, lbtodb(offset),
		    lbtodb(size)));
	}

	return (vdev_disk_ldi_physio(dvd->vd_lh, data, size, offset, flags));
}

static int
vdev_disk_io_intr(buf_t *bp)
{
	vdev_buf_t *vb = (vdev_buf_t *)bp;
	zio_t *zio = vb->vb_io;

	/*
	 * The rest of the zio stack only deals with EIO, ECKSUM, and ENXIO.
	 * Rather than teach the rest of the stack about other error
	 * possibilities (EFAULT, etc), we normalize the error value here.
	 */
	zio->io_error = (geterror(bp) != 0 ? EIO : 0);

	if (zio->io_error == 0 && bp->b_resid != 0)
		zio->io_error = SET_ERROR(EIO);

	if (zio->io_type == ZIO_TYPE_READ) {
		abd_return_buf_copy(zio->io_abd, bp->b_un.b_addr, zio->io_size);
	} else {
		abd_return_buf(zio->io_abd, bp->b_un.b_addr, zio->io_size);
	}

	kmem_free(vb, sizeof (vdev_buf_t));

	zio_delay_interrupt(zio);
	return (0);
}

static void
vdev_disk_ioctl_free(zio_t *zio)
{
	kmem_free(zio->io_vsd, sizeof (struct dk_callback));
}

static const zio_vsd_ops_t vdev_disk_vsd_ops = {
	vdev_disk_ioctl_free,
	zio_vsd_default_cksum_report
};

static void
vdev_disk_ioctl_done(void *zio_arg, int error)
{
	zio_t *zio = zio_arg;

	zio->io_error = error;

	zio_interrupt(zio);
}

static void
vdev_disk_io_start(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	vdev_disk_t *dvd = vd->vdev_tsd;
	unsigned long trim_flags = 0;
	vdev_buf_t *vb;
	struct dk_callback *dkc;
	buf_t *bp;
	int error;

	/*
	 * If the vdev is closed, it's likely in the REMOVED or FAULTED state.
	 * Nothing to be done here but return failure.
	 */
	if (dvd == NULL || dvd->vd_ldi_offline) {
		zio->io_error = ENXIO;
		zio_interrupt(zio);
		return;
	}

	switch (zio->io_type) {
	case ZIO_TYPE_IOCTL:
		/* XXPOLICY */
		if (!vdev_readable(vd)) {
			zio->io_error = SET_ERROR(ENXIO);
			zio_interrupt(zio);
			return;
		}

		switch (zio->io_cmd) {

		case DKIOCFLUSHWRITECACHE:

			if (zfs_nocacheflush)
				break;

			if (vd->vdev_nowritecache) {
				zio->io_error = SET_ERROR(ENOTSUP);
				break;
			}

			zio->io_vsd = dkc = kmem_alloc(sizeof (*dkc), KM_SLEEP);
			zio->io_vsd_ops = &vdev_disk_vsd_ops;

			dkc->dkc_callback = vdev_disk_ioctl_done;
			dkc->dkc_flag = FLUSH_VOLATILE;
			dkc->dkc_cookie = zio;

			error = ldi_ioctl(dvd->vd_lh, zio->io_cmd,
			    (uintptr_t)dkc, FKIOCTL, kcred, NULL);

			if (error == 0) {
				/*
				 * The ioctl will be done asychronously,
				 * and will call vdev_disk_ioctl_done()
				 * upon completion.
				 */
				return;
			}

			zio->io_error = error;

			break;

		default:
			zio->io_error = SET_ERROR(ENOTSUP);
		}

		zio_execute(zio);
		return;

	case ZIO_TYPE_TRIM:
		if (zfs_no_trim == 1 || !vd->vdev_has_trim) {
			zio->io_error = SET_ERROR(ENOTSUP);
			zio_execute(zio);
			return;
		}
		/* Currently only supported on ZoL. */
		ASSERT0(zio->io_trim_flags & ZIO_TRIM_SECURE);

		/* dkioc_free_list_t is already declared to hold one entry */
		dkioc_free_list_t dfl;
		dfl.dfl_flags = 0;
		dfl.dfl_num_exts = 1;
		dfl.dfl_offset = 0;
		dfl.dfl_exts[0].dfle_start = zio->io_offset;
		dfl.dfl_exts[0].dfle_length = zio->io_size;

		zio->io_error = ldi_ioctl(dvd->vd_lh, DKIOCFREE,
		    (uintptr_t)&dfl, FKIOCTL, kcred, NULL);

		if (zio->io_error == ENOTSUP || zio->io_error == ENOTTY) {
			/*
			 * The device must have changed and now TRIM is
			 * no longer supported.
			 */
			vd->vdev_has_trim = B_FALSE;
		}

		zio_interrupt(zio);
		return;
	}

	ASSERT(zio->io_type == ZIO_TYPE_READ || zio->io_type == ZIO_TYPE_WRITE);
	zio->io_target_timestamp = zio_handle_io_delay(zio);

	vb = kmem_alloc(sizeof (vdev_buf_t), KM_SLEEP);

	vb->vb_io = zio;
	bp = &vb->vb_buf;

	bioinit(bp);
	bp->b_flags = B_BUSY | B_NOCACHE |
	    (zio->io_type == ZIO_TYPE_READ ? B_READ : B_WRITE);
	if (!(zio->io_flags & (ZIO_FLAG_IO_RETRY | ZIO_FLAG_TRYHARD)))
		bp->b_flags |= B_FAILFAST;
	bp->b_bcount = zio->io_size;

	if (zio->io_type == ZIO_TYPE_READ) {
		bp->b_un.b_addr =
		    abd_borrow_buf(zio->io_abd, zio->io_size);
	} else {
		bp->b_un.b_addr =
		    abd_borrow_buf_copy(zio->io_abd, zio->io_size);
	}

	bp->b_lblkno = lbtodb(zio->io_offset);
	bp->b_bufsize = zio->io_size;
	bp->b_iodone = vdev_disk_io_intr;

	/*
	 * In general we would expect ldi_strategy() to return non-zero only
	 * because of programming errors, but we've also seen this fail shortly
	 * after a disk dies.
	 */
	if (ldi_strategy(dvd->vd_lh, bp) != 0) {
		zio->io_error = ENXIO;
		zio_interrupt(zio);
	}
}

static void
vdev_disk_io_done(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;

	/*
	 * If the device returned EIO, then attempt a DKIOCSTATE ioctl to see if
	 * the device has been removed.  If this is the case, then we trigger an
	 * asynchronous removal of the device. Otherwise, probe the device and
	 * make sure it's still accessible.
	 */
	if (zio->io_error == EIO && !vd->vdev_remove_wanted) {
		vdev_disk_t *dvd = vd->vdev_tsd;
		int state = DKIO_NONE;

		if (ldi_ioctl(dvd->vd_lh, DKIOCSTATE, (intptr_t)&state,
		    FKIOCTL, kcred, NULL) == 0 && state != DKIO_INSERTED) {
			/*
			 * We post the resource as soon as possible, instead of
			 * when the async removal actually happens, because the
			 * DE is using this information to discard previous I/O
			 * errors.
			 */
			zfs_post_remove(zio->io_spa, vd);
			vd->vdev_remove_wanted = B_TRUE;
			spa_async_request(zio->io_spa, SPA_ASYNC_REMOVE);
		} else if (!vd->vdev_delayed_close) {
			vd->vdev_delayed_close = B_TRUE;
		}
	}
}

vdev_ops_t vdev_disk_ops = {
	.vdev_op_open = vdev_disk_open,
	.vdev_op_close = vdev_disk_close,
	.vdev_op_asize = vdev_default_asize,
	.vdev_op_io_start = vdev_disk_io_start,
	.vdev_op_io_done = vdev_disk_io_done,
	.vdev_op_state_change = NULL,
	.vdev_op_need_resilver = NULL,
	.vdev_op_hold = vdev_disk_hold,
	.vdev_op_rele = vdev_disk_rele,
	.vdev_op_remap = NULL,
	.vdev_op_xlate = vdev_default_xlate,
	.vdev_op_dumpio = vdev_disk_dumpio,
	.vdev_op_type = VDEV_TYPE_DISK,		/* name of this vdev type */
	.vdev_op_leaf = B_TRUE			/* leaf vdev */
};

/*
 * Given the root disk device devid or pathname, read the label from
 * the device, and construct a configuration nvlist.
 */
int
vdev_disk_read_rootlabel(const char *devpath, const char *devid,
    nvlist_t **config)
{
	ldi_handle_t vd_lh;
	vdev_label_t *label;
	uint64_t s, size;
	int l;
	ddi_devid_t tmpdevid;
	int error = -1;
	char *minor_name;

	/*
	 * Read the device label and build the nvlist.
	 */
	if (devid != NULL && ddi_devid_str_decode((char *)devid, &tmpdevid,
	    &minor_name) == 0) {
		error = ldi_open_by_devid(tmpdevid, minor_name,
		    FREAD, kcred, &vd_lh, zfs_li);
		ddi_devid_free(tmpdevid);
		ddi_devid_str_free(minor_name);
	}

	if (error != 0 && (error = ldi_open_by_name((char *)devpath, FREAD,
	    kcred, &vd_lh, zfs_li)) != 0) {
		return (error);
	}

	if (ldi_get_size(vd_lh, &s)) {
		(void) ldi_close(vd_lh, FREAD, kcred);
		return (SET_ERROR(EIO));
	}

	size = P2ALIGN_TYPED(s, sizeof (vdev_label_t), uint64_t);
	label = kmem_alloc(sizeof (vdev_label_t), KM_SLEEP);

	*config = NULL;
	for (l = 0; l < VDEV_LABELS; l++) {
		uint64_t offset, state, txg = 0;

		/* read vdev label */
		offset = vdev_label_offset(size, l, 0);
		if (vdev_disk_ldi_physio(vd_lh, (caddr_t)label,
		    VDEV_SKIP_SIZE + VDEV_PHYS_SIZE, offset, B_READ) != 0)
			continue;

		if (nvlist_unpack(label->vl_vdev_phys.vp_nvlist,
		    sizeof (label->vl_vdev_phys.vp_nvlist), config, 0) != 0) {
			*config = NULL;
			continue;
		}

		if (nvlist_lookup_uint64(*config, ZPOOL_CONFIG_POOL_STATE,
		    &state) != 0 || state >= POOL_STATE_DESTROYED) {
			nvlist_free(*config);
			*config = NULL;
			continue;
		}

		if (nvlist_lookup_uint64(*config, ZPOOL_CONFIG_POOL_TXG,
		    &txg) != 0 || txg == 0) {
			nvlist_free(*config);
			*config = NULL;
			continue;
		}

		break;
	}

	kmem_free(label, sizeof (vdev_label_t));
	(void) ldi_close(vd_lh, FREAD, kcred);
	if (*config == NULL)
		error = SET_ERROR(EIDRM);

	return (error);
}

struct veb {
	list_t veb_ents;
	boolean_t veb_scanned;
	char *veb_force_path;
};

struct veb_ent {
	uint64_t vebe_pool_guid;
	uint64_t vebe_vdev_guid;

	char *vebe_devpath;

	list_node_t vebe_link;
};

static kmutex_t veb_lock;
static struct veb *veb;

static int
vdev_disk_preroot_scan_walk(const char *devpath, void *arg)
{
	int r;
	nvlist_t *cfg = NULL;
	uint64_t pguid = 0, vguid = 0;

	/*
	 * Attempt to read the label from this block device.
	 */
	if ((r = vdev_disk_read_rootlabel(devpath, NULL, &cfg)) != 0) {
		/*
		 * Many of the available block devices will represent slices or
		 * partitions of disks, or may represent disks that are not at
		 * all initialised with ZFS.  As this is a best effort
		 * mechanism to locate an alternate path to a particular vdev,
		 * we will ignore any failures and keep scanning.
		 */
		return (PREROOT_WALK_BLOCK_DEVICES_NEXT);
	}

	/*
	 * Determine the pool and vdev GUID read from the label for this
	 * device.  Both values must be present and have a non-zero value.
	 */
	if (nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_POOL_GUID, &pguid) != 0 ||
	    nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_GUID, &vguid) != 0 ||
	    pguid == 0 || vguid == 0) {
		/*
		 * This label was not complete.
		 */
		goto out;
	}

	/*
	 * Keep track of all of the GUID-to-devpath mappings we find so that
	 * vdev_disk_preroot_lookup() can search them.
	 */
	struct veb_ent *vebe = kmem_zalloc(sizeof (*vebe), KM_SLEEP);
	vebe->vebe_pool_guid = pguid;
	vebe->vebe_vdev_guid = vguid;
	vebe->vebe_devpath = spa_strdup(devpath);

	list_insert_tail(&veb->veb_ents, vebe);

out:
	nvlist_free(cfg);
	return (PREROOT_WALK_BLOCK_DEVICES_NEXT);
}

const char *
vdev_disk_preroot_lookup(uint64_t pool_guid, uint64_t vdev_guid)
{
	if (pool_guid == 0 || vdev_guid == 0) {
		/*
		 * If we aren't provided both a pool and a vdev GUID, we cannot
		 * perform a lookup.
		 */
		return (NULL);
	}

	mutex_enter(&veb_lock);
	if (veb == NULL) {
		/*
		 * If vdev_disk_preroot_fini() has been called already, there
		 * is nothing we can do.
		 */
		mutex_exit(&veb_lock);
		return (NULL);
	}

	/*
	 * We want to perform at most one scan of all block devices per boot.
	 */
	if (!veb->veb_scanned) {
		cmn_err(CE_NOTE, "Performing full ZFS device scan!");

		preroot_walk_block_devices(vdev_disk_preroot_scan_walk, NULL);

		veb->veb_scanned = B_TRUE;
	}

	const char *path = NULL;
	for (struct veb_ent *vebe = list_head(&veb->veb_ents); vebe != NULL;
	    vebe = list_next(&veb->veb_ents, vebe)) {
		if (vebe->vebe_pool_guid == pool_guid &&
		    vebe->vebe_vdev_guid == vdev_guid) {
			path = vebe->vebe_devpath;
			break;
		}
	}

	mutex_exit(&veb_lock);

	return (path);
}

const char *
vdev_disk_preroot_force_path(void)
{
	const char *force_path = NULL;

	mutex_enter(&veb_lock);
	if (veb != NULL) {
		force_path = veb->veb_force_path;
	}
	mutex_exit(&veb_lock);

	return (force_path);
}

void
vdev_disk_preroot_init(const char *force_path)
{
	mutex_init(&veb_lock, NULL, MUTEX_DEFAULT, NULL);

	VERIFY3P(veb, ==, NULL);
	veb = kmem_zalloc(sizeof (*veb), KM_SLEEP);
	list_create(&veb->veb_ents, sizeof (struct veb_ent),
	    offsetof(struct veb_ent, vebe_link));
	veb->veb_scanned = B_FALSE;
	if (force_path != NULL) {
		veb->veb_force_path = spa_strdup(force_path);
	}
}

void
vdev_disk_preroot_fini(void)
{
	mutex_enter(&veb_lock);

	if (veb != NULL) {
		while (!list_is_empty(&veb->veb_ents)) {
			struct veb_ent *vebe = list_remove_head(&veb->veb_ents);

			spa_strfree(vebe->vebe_devpath);

			kmem_free(vebe, sizeof (*vebe));
		}

		if (veb->veb_force_path != NULL) {
			spa_strfree(veb->veb_force_path);
		}

		kmem_free(veb, sizeof (*veb));
		veb = NULL;
	}

	mutex_exit(&veb_lock);
}
