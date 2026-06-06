// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * main.c
 * FSAL_DOTFS — module entry point.
 *
 * This file is the shared-library entry point for the DOTFS FSAL plugin.
 * When NFS-Ganesha calls dlopen() on libfsal_dotfs.so, the linker invokes
 * dotfs_init() (MODULE_INIT).  On dlclose() / server shutdown, dotfs_unload()
 * (MODULE_FINI) is called.
 *
 * Responsibilities of this file:
 *   - Define the singleton dotfs_fsal_module instance (DOTFS).
 *   - Declare the module-level config parameter block.
 *   - Implement init_config() to parse module-level ganesha.conf settings.
 *   - Wire create_export / update_export / init_config into the module's
 *     m_ops table.
 *   - Initialise the shared handle ops table (dotfs_handle_ops_init).
 *   - Register / unregister with Ganesha's FSAL registry.
 *
 * Only one instance of DOTFS is loaded per Ganesha process; all exports
 * share the same fsal_module singleton and the same handle_ops table.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>

#include "fsal.h"
#include "FSAL/fsal_init.h"
#include "gsh_list.h"

#include "dotfs_methods.h"

/* =========================================================================
 * Supported attribute mask
 *
 * ATTRS_POSIX covers the mandatory POSIX attribute set (mode, uid, gid,
 * size, atime, mtime, ctime, nlink, …).  Extend this once dotfs adds
 * ACL support (ATTR_ACL) or xattr support (ATTR4_XATTR).
 * ========================================================================= */

#define DOTFS_SUPPORTED_ATTRIBUTES ((const attrmask_t)(ATTRS_POSIX))

/* =========================================================================
 * Module singleton
 *
 * One statically-allocated instance; its address is passed to
 * register_fsal() and serves as the fsal_module* throughout Ganesha.
 * ========================================================================= */

static const char myname[] = "DOTFS";

/**
 * DOTFS — the single module instance.
 *
 * fs_info values below are conservative defaults matching a distributed
 * object store that does not yet have full POSIX semantics:
 *   - lock_support = false  (distributed locks not yet implemented — P1)
 *   - named_attr   = false  (xattr support is P2)
 *   - unique_handles = true (UUIDs guarantee per-object uniqueness)
 *   - expire_time_parent = -1 (directory entries don't expire independently)
 */
static struct dotfs_fsal_module DOTFS = {
	.module = {
		.fs_info = {
			.maxfilesize             = INT64_MAX,
			.maxlink                 = _POSIX_LINK_MAX,
			.maxnamelen              = 1024,
			.maxpathlen              = 1024,
			.no_trunc                = true,
			.chown_restricted        = true,
			.case_insensitive        = false,
			.case_preserving         = true,
			.lock_support            = false,  /* TODO: P1 */
			.lock_support_async_block = false,
			.named_attr              = false,  /* TODO: P2 xattr */
			.unique_handles          = true,
			.acl_support             = FSAL_ACLSUPPORT_ALLOW,
			.homogenous              = true,
			.supported_attrs         = DOTFS_SUPPORTED_ATTRIBUTES,
			.maxread                 = FSAL_MAXIOSIZE,
			.maxwrite                = FSAL_MAXIOSIZE,
			.link_supports_permission_checks = false,
			.expire_time_parent      = -1,
			.xattr_support           = false,  /* TODO: P2 */
		}
	}
};

/* =========================================================================
 * Module-level config parameters
 *
 * These appear in the global FSAL block of ganesha.conf:
 *
 *   FSAL {
 *       DOTFS {
 *           maxread  = 1048576;
 *           maxwrite = 1048576;
 *       }
 *   }
 * ========================================================================= */

static struct config_item dotfs_params[] = {
	CONF_ITEM_BOOL("link_support", true, dotfs_fsal_module,
		       module.fs_info.link_support),
	CONF_ITEM_BOOL("symlink_support", true, dotfs_fsal_module,
		       module.fs_info.symlink_support),
	CONF_ITEM_BOOL("cansettime", true, dotfs_fsal_module,
		       module.fs_info.cansettime),
	CONF_ITEM_UI64("maxread", 512, FSAL_MAXIOSIZE, FSAL_MAXIOSIZE,
		       dotfs_fsal_module, module.fs_info.maxread),
	CONF_ITEM_UI64("maxwrite", 512, FSAL_MAXIOSIZE, FSAL_MAXIOSIZE,
		       dotfs_fsal_module, module.fs_info.maxwrite),
	CONF_ITEM_MODE("umask", 0, dotfs_fsal_module, module.fs_info.umask),
	CONF_ITEM_BOOL("auth_xdev_export", false, dotfs_fsal_module,
		       module.fs_info.auth_exportpath_xdev),
	/* TODO: add dotfs-specific module params, e.g.:
	 *   CONF_ITEM_STR("default_meta_addr", 1, 256,
	 *                 "localhost:2379", dotfs_fsal_module, default_meta_addr),
	 */
	CONFIG_EOL
};

static struct config_block dotfs_param_block = {
	.dbus_interface_name = "org.ganesha.nfsd.config.fsal.dotfs",
	.blk_desc.name       = "DOTFS",
	.blk_desc.type       = CONFIG_BLOCK,
	.blk_desc.flags      = CONFIG_UNIQUE, /* only one DOTFS module block */
	.blk_desc.u.blk.init   = noop_conf_init,
	.blk_desc.u.blk.params = dotfs_params,
	.blk_desc.u.blk.commit = noop_conf_commit,
};

/* =========================================================================
 * Module-level ops
 * ========================================================================= */

/**
 * init_config — parse module-level DOTFS config and validate the environment.
 *
 * Called by Ganesha once after the module is registered.  Loads the DOTFS
 * config block from @p config_struct, applies values to the DOTFS module
 * singleton, and performs any one-time environment checks (e.g. verifying
 * that the dotfs C-binding is available).
 *
 * @param[in] fsal_module_hdl  The DOTFS fsal_module (== &DOTFS.module).
 * @param[in] config_struct    Parsed Ganesha config file handle.
 * @param[in] err_type         Error accumulator; use config_error_is_harmless().
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_INVAL on config parse error.
 *
 * TODO: add a runtime check that the dotfs shared library / CGo bridge is
 *       available and at a compatible version before returning success.
 */
static fsal_status_t init_config(struct fsal_module *fsal_module_hdl,
				 config_file_t config_struct,
				 struct config_error_type *err_type)
{
	struct dotfs_fsal_module *dotfs_module =
		container_of(fsal_module_hdl, struct dotfs_fsal_module, module);

	LogInfo(COMPONENT_FSAL, "DOTFS init_config called");

	(void)load_config_from_parse(config_struct, &dotfs_param_block,
				     dotfs_module, true, err_type);
	if (!config_error_is_harmless(err_type))
		return fsalstat(ERR_FSAL_INVAL, 0);

	display_fsinfo(&dotfs_module->module);

	LogDebug(COMPONENT_FSAL,
		 "DOTFS: supported_attrs=0x%" PRIx64 " maxread=%" PRIu64
		 " maxwrite=%" PRIu64,
		 dotfs_module->module.fs_info.supported_attrs,
		 dotfs_module->module.fs_info.maxread,
		 dotfs_module->module.fs_info.maxwrite);

	/* TODO: probe the dotfs C-binding here.  Example:
	 *   if (dotfs_runtime_version() < DOTFS_MIN_VERSION) {
	 *       LogCrit(COMPONENT_FSAL, "DOTFS: incompatible runtime version");
	 *       return fsalstat(ERR_FSAL_INVAL, 0);
	 *   }
	 */

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* =========================================================================
 * Module load / unload  (called by dlopen / dlclose via linker magic)
 * ========================================================================= */

/**
 * dotfs_init — register FSAL_DOTFS with Ganesha at shared-library load time.
 *
 * Invoked automatically by the dynamic linker (via MODULE_INIT attribute)
 * when Ganesha loads libfsal_dotfs.so.  Performs:
 *   1. register_fsal()    — inserts DOTFS into Ganesha's FSAL registry.
 *   2. Sets m_ops         — wires module-level callbacks.
 *   3. dotfs_handle_ops_init() — populates the shared fsal_obj_ops table.
 */
MODULE_INIT void dotfs_init(void)
{
	int retval;
	struct fsal_module *myself = &DOTFS.module;

	retval = register_fsal(myself, myname,
			       FSAL_MAJOR_VERSION, FSAL_MINOR_VERSION,
			       FSAL_ID_NO_PNFS);
	if (retval != 0) {
		fprintf(stderr, "DOTFS: module failed to register (rc=%d)\n",
			retval);
		return;
	}

	/* Wire module-level ops. */
	myself->m_ops.create_export = dotfs_create_export;
	myself->m_ops.update_export = dotfs_update_export;
	myself->m_ops.init_config   = init_config;

	/* Populate the shared handle ops table used by all object handles
	 * created under any DOTFS export. */
	dotfs_handle_ops_init(&DOTFS.handle_ops);

	LogInfo(COMPONENT_FSAL, "DOTFS module registered as \"%s\"", myname);
}

/**
 * dotfs_unload — deregister FSAL_DOTFS on shared-library unload.
 *
 * Invoked by the dynamic linker (MODULE_FINI) during dlclose() or process
 * shutdown.  At this point all exports must already have been released by
 * Ganesha.  Any remaining VFS context would be a leak — log a critical
 * error so it shows up in post-mortem analysis.
 *
 * TODO: walk the exports list and assert it is empty; log leaked contexts.
 */
MODULE_FINI void dotfs_unload(void)
{
	int retval;

	retval = unregister_fsal(&DOTFS.module);
	if (retval != 0) {
		fprintf(stderr,
			"DOTFS: module failed to unregister (rc=%d) — "
			"possible resource leak\n",
			retval);
		return;
	}

	LogInfo(COMPONENT_FSAL, "DOTFS module unregistered");
}


