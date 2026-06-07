// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Author: Sachin S <sarodhesachin96@gmail.com>
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

#include "dotfs.h"

/**
 * dotfs_create_export — create and register a new DOTFS export.
 *
 * Entry point invoked by Ganesha's export manager when it processes an
 * export configuration block whose FSAL is DOTFS.  Steps:
 *   1. Parse DOTFS-specific config from @p parse_node.
 *   2. Allocate and zero a dotfs_fsal_export.
 *   3. Open the dotfs VFS context for the export path.
 *   4. Register the export ops table with Ganesha.
 *   5. Attach to the FSAL module's export list.
 *
 * @param[in] fsal_hdl   The DOTFS fsal_module singleton.
 * @param[in] parse_node Opaque config parse node for this export block.
 * @param[in] err_type   Error accumulator for config parse errors.
 * @param[in] up_ops     Upcall vector (cache invalidation, layout recalls).
 *
 * @return FSAL_NO_ERROR on success; ERR_FSAL_NOMEM / ERR_FSAL_INVAL on error.
 */
fsal_status_t dotfs_create_export(struct fsal_module *fsal_hdl,
				  void *parse_node,
				  struct config_error_type *err_type,
				  const struct fsal_up_vector *up_ops)
{
    // TODO: implement this function.  Example steps:
    //   1. Parse the per-export DOTFS config block (e.g. export path, ACL policy, etc.) from @p parse_node.
    //   2. Allocate and zero a dotfs_fsal_export structure.
    //   3. Open the dotfs VFS context for the export path (e.g. by calling a dotfs_open_vfs_ctx() function in the dotfs C-binding).
    //   4. Register the export ops table with Ganesha (e.g. by setting myself->export.exp_ops.release = dotfs_release_export, etc.).
    //   5. Attach to the FSAL module's export list (e.g. by calling fsal_attach_export()).
    //   6. Return FSAL_NO_ERROR on success, or an appropriate error code on failure.

    fsal_status_t status = { ERR_FSAL_NO_ERROR, 0 };
    return status;
}

/**
 * dotfs_update_export — hot-update an existing DOTFS export on config reload.
 *
 * Called on SIGHUP when Ganesha re-reads its config and finds a changed
 * export block.  Should update mutable parameters (ACL policy, shard counts,
 * etc.) without disrupting active NFS sessions.
 *
 * @param[in] fsal_hdl      DOTFS module handle.
 * @param[in] parse_node    New config parse node.
 * @param[in] err_type      Error accumulator.
 * @param[in] original      The currently-active fsal_export to update.
 * @param[in] updated_super Updated parent FSAL module (for stackable FSALs).
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: implement parameter diffing and live reconfiguration.
 */
fsal_status_t dotfs_update_export(struct fsal_module *fsal_hdl,
				  void *parse_node,
				  struct config_error_type *err_type,
				  struct fsal_export *original,
				  struct fsal_module *updated_super)
{
    // TODO: implement this function.  Example steps:
    //   1. Parse the new config parameters from @p parse_node.
    //   2. Compare the new parameters with the currently active export configuration in @p original to determine what has changed.
    //   3. For any mutable parameters that have changed (e.g. ACL policy
    //      or shard counts), apply the new settings to the active export without disrupting existing NFS sessions.  This may involve calling into the dotfs C-binding to update the VFS context.
    //   4. For any immutable parameters that have changed (e.g. export path), log a warning and ignore the change, since we cannot update those without tearing down the export.
    //   5. Return FSAL_NO_ERROR on success, or an appropriate error code on failure.

    fsal_status_t status = { ERR_FSAL_NO_ERROR, 0 };
    return status;
}
