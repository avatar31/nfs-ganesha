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

#include <limits.h>
#include "config.h"
#include "fsal.h"
#include "FSAL/fsal_init.h"

#include "dotfs.h"


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
	CONF_ITEM_STR("inbound_sock_path", 1, SOCK_PATH_MAX, "", dotfs_fsal_module,
		      sock_ctx.inbound_socket_path),
	CONF_ITEM_STR("outbound_sock_path", 1, SOCK_PATH_MAX, "", dotfs_fsal_module,
		      sock_ctx.outbound_socket_path),
	CONFIG_EOL
};

struct config_block dotfs_param_block = {
	.dbus_interface_name = "org.ganesha.nfsd.config.fsal.dotfs",
	.blk_desc.name = "DOTFS",			// Block name in ganesha.conf
	.blk_desc.type = CONFIG_BLOCK, 		// Standalone block with key/value pairs
	.blk_desc.flags = CONFIG_UNIQUE,	// Only one DOTFS block allowed in ganesha.conf
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = dotfs_params,
	.blk_desc.u.blk.commit = noop_conf_commit
};

/**
 * init_sock — initialize the DOTFS socket context and connect to the daemon.
 * @param[in,out] dotfs_module The DOTFS module singleton containing the socket context to initialize.
 * 
 * @return DFS_PASS on success, DFS_FAIL on failure.
 */
static dfs_status_t init_sock(dotfs_fsal_module_t *dotfs_module)
{
	pthread_t in_tid, out_tid;

	if (initialize_socket_ctx(&dotfs_module->sock_ctx) != DFS_PASS) {
		LogErrorMsg("Failed to initialize socket context");
		return (DFS_FAIL);
	}

	if (init_inbound_server(&dotfs_module->sock_ctx) != DFS_PASS) {
        LogErrorMsg("Failed to initialize inbound socket interface. Aborting startup.");
        return (DFS_FAIL);
    }

	if (pthread_create(&in_tid, NULL, inbound_reader_thread, &dotfs_module->sock_ctx) != 0) {
		LogSysError("Failed to create inbound reader thread", errno);
		close(dotfs_module->sock_ctx.inbound_sock_fd);
        unlink(dotfs_module->sock_ctx.inbound_socket_path);
		return (DFS_FAIL);
	}
	pthread_detach(in_tid);

    if (pthread_create(&out_tid, NULL, outbound_dialer_thread, &dotfs_module->sock_ctx) != 0) {
		LogSysError("Failed to create inbound reader thread", errno);
		close(dotfs_module->sock_ctx.inbound_sock_fd);
        unlink(dotfs_module->sock_ctx.inbound_socket_path);
		return (DFS_FAIL);
	}
    pthread_detach(out_tid);

	return (DFS_PASS);
}

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
	dotfs_fsal_module_t *dotfs_module =
		container_of(fsal_module_hdl, dotfs_fsal_module_t, module);

	(void)load_config_from_parse(config_struct, &dotfs_param_block,
				     dotfs_module, true, err_type);
	if (!config_error_is_harmless(err_type)) {
		return fsalstat(ERR_FSAL_INVAL, 0);
	}

	display_fsinfo(&dotfs_module->module);

	LogDebugMsg("DOTFS: supported_attrs=0x%" PRIx64 " maxread=%" PRIu64
		    " maxwrite=%" PRIu64,
		    dotfs_module->module.fs_info.supported_attrs,
		    dotfs_module->module.fs_info.maxread,
		    dotfs_module->module.fs_info.maxwrite);

	dfs_status_t status = init_sock(dotfs_module);
	if (status != DFS_PASS) {
		return fsalstat(ERR_FSAL_FAULT, 0);
	}

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

	retval = register_fsal(myself, FS_NAME, FSAL_MAJOR_VERSION,
			       FSAL_MINOR_VERSION, FSAL_ID_NO_PNFS);
	if (retval != 0) {
		LogErrorMsg("DOTFS: module failed to register (rc=%d)\n", retval);
		return;
	}

	/* Wire module-level ops. */
	myself->m_ops.create_export = dotfs_create_export;
	myself->m_ops.update_export = dotfs_update_export;
	myself->m_ops.init_config = init_config;

	/* Populate the shared handle ops table used by all object handles
	 * created under any DOTFS export. */
	dotfs_handle_ops_init(&DOTFS.handle_ops);

	LogInfoMsg("DOTFS module registered as \"%s\"", FS_NAME);
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
	socket_close(&DOTFS.sock_ctx);

	int retval = unregister_fsal(&DOTFS.module);
	if (retval != 0) {
		LogErrorMsg("failed to unregister (rc=%d)\n", retval);
		return;
	}

	// TODO: walk the exports list and assert it is empty; log leaked contexts.

	LogInfoMsg("DOTFS module unregistered");
}
