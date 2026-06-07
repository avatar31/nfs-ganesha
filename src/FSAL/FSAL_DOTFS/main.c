// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Author: Sachin S <sarodhesachin96@gmail.com>
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * -------------
 */

/* =========================================================================
 * Supported attribute mask
 *
 * ATTRS_POSIX covers the mandatory POSIX attribute set (mode, uid, gid,
 * size, atime, mtime, ctime, nlink, …).  Extend this once dotfs adds
 * ACL support (ATTR_ACL) or xattr support (ATTR4_XATTR).
 * ========================================================================= */

#include <limits.h>

#include "FSAL/fsal_init.h"

#include "dotfs.h"

#define DOTFS_SUPPORTED_ATTRIBUTES ((const attrmask_t)(ATTRS_POSIX))

static const char myname[] = "DOTFS";

static dotfs_fsal_module_t DOTFS = {
    .module = {
        .fs_info = {
			.maxfilesize = INT64_MAX,
			.maxlink = _POSIX_LINK_MAX,
			.maxnamelen = 1024,
			.maxpathlen = 1024,
			.no_trunc = true,
			.chown_restricted = true,                       // TODO: What is this field for?
			.case_insensitive = false,                      // TODO: What is this field for?
			.case_preserving = true,                        // TODO: What is this field for?
			.lock_support = false,                          // TODO: What is this field for?
			.lock_support_async_block = false,              // TODO: What is this field for?
			.named_attr = true,                             // TODO: What is this field for?
			.unique_handles = true,                         // TODO: What is this field for?
			.acl_support = FSAL_ACLSUPPORT_ALLOW,
			.homogenous = true,                             // TODO: What is this field for?
			.supported_attrs = DOTFS_SUPPORTED_ATTRIBUTES,    // TODO: What is this field for?
			.maxread = FSAL_MAXIOSIZE,                      // TODO: What is this field for?
			.maxwrite = FSAL_MAXIOSIZE,                     // TODO: What is this field for?
			.link_supports_permission_checks = false,       // TODO: What is this field for?
			.expire_time_parent = -1,                       // TODO: What is this field for?
			.symlink_support = true,
            .link_support = true,
			.cansettime = true,                             // TODO: What is this field for?
            .readdir_plus = true,                           // TODO: What is this field for?
			.xattr_support = true,                          // TODO: What is this field for?
            .readdir_mode = FSAL_RDDIR_CHUNK_NEVER,         // TODO: What is this field for?
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
    // Everything is hard coded for now, 
    // but we can add config options here in the future if needed
    CONFIG_EOL
};

struct config_block dotfs_param_block = {
    .dbus_interface_name = "org.ganesha.nfsd.config.fsal.dotfs",
	.blk_desc.name = "DOTFS",
	.blk_desc.type = CONFIG_BLOCK,              // TODO: What is this field for?
	.blk_desc.flags = CONFIG_UNIQUE,            // TODO: What is this field for?
	.blk_desc.u.blk.init = noop_conf_init,      // TODO: What is this field for?
	.blk_desc.u.blk.params = dotfs_params,      // TODO: What is this field for?
	.blk_desc.u.blk.commit = noop_conf_commit   // TODO: What is this field for?
};

/**
 * init_config — parse module-level DOTFS config and validate the environment.
 *
 * Called by Ganesha once after the module is registered.  Loads the DOTFS
 * config block from @p config_struct, applies values to the DOTFS module
 * singleton, and performs any one-time environment checks (e.g. verifying
 * that the dotfs C-binding is available).
 *
 * @param[in] fsal_module_hdl  The DOTFS fsal_module handle (i.e. &DOTFS.module).
 * @param[in] config_struct    Parsed Ganesha config file handle.
 * @param[in] err_type         Error accumulator; use config_error_is_harmless().
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_INVAL on config parse error.
 */
static fsal_status_t init_config(struct fsal_module *fsal_module_hdl,
				 config_file_t config_struct,
				 struct config_error_type *err_type)
{
    // TODO: Check Unix socket path is exist and writable

    dotfs_fsal_module_t *dotfs_module =
		container_of(fsal_module_hdl, dotfs_fsal_module_t, module);

	(void)load_config_from_parse(config_struct, &dotfs_param_block,
				     dotfs_module, true, err_type);
	if (!config_error_is_harmless(err_type)) {
		return fsalstat(ERR_FSAL_INVAL, 0);
    }

	display_fsinfo(&dotfs_module->module);

    LogDebug(COMPONENT_FSAL,
        "DOTFS: supported_attrs=0x%" PRIx64 " maxread=%" PRIu64
		" maxwrite=%" PRIu64,
		dotfs_module->module.fs_info.supported_attrs,
		dotfs_module->module.fs_info.maxread,
		dotfs_module->module.fs_info.maxwrite);

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

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

    retval = register_fsal(myself, myname, FSAL_MAJOR_VERSION,
        FSAL_MINOR_VERSION, FSAL_ID_NO_PNFS);
	if (retval != 0) {
        LogCrit(COMPONENT_FSAL, "DOTFS: module failed to register (rc=%d)\n",
            retval);
		return;
	}

	/* Wire module-level ops. */
	myself->m_ops.create_export = dotfs_create_export;
	myself->m_ops.update_export = dotfs_update_export;
	myself->m_ops.init_config = init_config;

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
 */
MODULE_FINI void dotfs_unload(void)
{
    int retval = unregister_fsal(&DOTFS.module);
	if (retval != 0) {
        LogCrit(COMPONENT_FSAL, "DOTFS: module failed to unregister (rc=%d)\n",
            retval);
		return;
	}

    // TODO: walk the exports list and assert it is empty; log leaked contexts.

	LogInfo(COMPONENT_FSAL, "DOTFS module unregistered");
}
