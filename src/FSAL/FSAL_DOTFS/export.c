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

#include "fsal_convert.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_config.h"
#include "FSAL/fsal_localfs.h"

#include "dotfs.h"

void dotfs_export_ops_init(struct export_ops *ops);
static void dotfs_export_release(struct fsal_export *exp_hdl);
fsal_status_t dotfs_export_lookup_path(struct fsal_export *exp_hdl, const char *path,
			      struct fsal_obj_handle **obj_hdl,
			      struct fsal_attrlist *attrs_out);
static fsal_status_t dotfs_export_wire_to_host(struct fsal_export *exp_hdl,
					fsal_digesttype_t in_type,
					struct gsh_buffdesc *fh_desc, int flags);
fsal_status_t dotfs_export_create_handle(struct fsal_export *exp_hdl,
				  struct gsh_buffdesc *hdl_desc,
				  struct fsal_obj_handle **obj_hdl,
				  struct fsal_attrlist *attrs_out);
static fsal_status_t dotfs_export_get_fs_dynamic_info(struct fsal_export *exp_hdl,
				      struct fsal_obj_handle *obj_hdl,
				      fsal_dynamicfsinfo_t *infop);
static attrmask_t dotfs_export_fs_supported_attrs(struct fsal_export *exp_hdl);
static struct state_t *dotfs_export_alloc_state(struct fsal_export *exp_hdl,
					 enum state_type state_type,
					 struct state_t *related_state);
void dotfs_export_get_fsal_obj_hdl(struct fsal_export *exp_hdl, struct fsal_fd *fd,
		      struct fsal_obj_handle **handle);
static void dotfs_close_ctx(dotfs_context_t *ctx);


static struct config_item export_params[] = {
    CONF_ITEM_NOOP("name"),
	CONFIG_EOL
};

static struct config_block export_param = {
	.dbus_interface_name = "org.ganesha.nfsd.config.fsal.dotfs-export",
	.blk_desc.name = "FSAL",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = export_params,
	.blk_desc.u.blk.commit = noop_conf_commit
};


/**
 * @brief Create and register a new DOTFS export.
 *
 * Invoked by Ganesha's daemon engine during initialization or reloads. This function allocates 
 * the memory block for the DOTFS export, parses parameters, and assigns the structural 
 * operational vectors.
 *
 * Steps:
 *   1. Parse DOTFS-specific config from @p parse_node.
 *   2. Allocate and zero a dotfs_fsal_export_t.
 *   3. Open the dotfs VFS context for the export path.
 *   4. Register the export ops table with Ganesha.
 *   5. Attach to the FSAL module's export list.
 *
 * @param[in]  fsal_hdl   Pointer to the master FSAL module parent structure.
 * @param[in]  parse_node Configuration parsing node handle representing the current block.
 * @param[out] err_type   Buffer utilized to report configuration compilation faults.
 * @param[in]  up_ops     Upper layer up-call function vector table mappings.
 *
 * @return fsal_status_t  Returns ERR_FSAL_NO_ERROR on a clean setup or mapped POSIX errors.
 */
fsal_status_t dotfs_create_export(struct fsal_module *fsal_hdl,
				  void *parse_node,
				  struct config_error_type *err_type,
				  const struct fsal_up_vector *up_ops)
{
    dotfs_fsal_export_t *myself = NULL;
    int rc = 0;
    fsal_status_t status = { ERR_FSAL_NO_ERROR, 0 };

    LogInfoMsg("Creating new DOTFS export");

    // TODO: Do we need AVL tree to maintain the list of exports?
    // vfs_state_init();

    myself = gsh_calloc(1, sizeof(dotfs_fsal_export_t));
    myself->root_handle = NULL;

    fsal_export_init(&myself->export);
    dotfs_export_ops_init(&myself->export.exp_ops);
    myself->export.up_ops = up_ops;

    dotfs_fsal_module_t *dotfs_module =
		container_of(fsal_hdl, dotfs_fsal_module_t, module);
    myself->shared_sock_ctx = &dotfs_module->sock_ctx;

    rc = load_config_from_node(parse_node, &export_param, myself,
				       true, err_type);
	if (rc != 0) {
		status = posix2fsal_status(EINVAL);
		goto err_free;
	}

    myself->export_id = op_ctx->ctx_export->export_id;
    if (CTX_FULLPATH(op_ctx)) {
        if (myself->export_path != NULL) {
            gsh_free(myself->export_path);
            myself->export_path = NULL;
        }
        myself->export_path = gsh_strdup(CTX_FULLPATH(op_ctx));
    }

    LogInfoMsg("DOTFS export parameters: export_id=%" PRIu64 ", path=%s",
                myself->export_id, myself->export_path);

    myself->export.fsal = fsal_hdl;

    rc = fsal_attach_export(fsal_hdl, &myself->export.exports);
	if (rc != 0) {
		status = posix2fsal_status(rc);
		goto err_cleanup;
	}

    op_ctx->fsal_export = &myself->export;

    LogEventMsg("Export %lu created and attached successfully for path: %s", 
                myself->export_id, myself->export_path);
    return fsalstat(ERR_FSAL_NO_ERROR, 0);

err_cleanup:
	unclaim_all_export_maps(&myself->export);
	fsal_detach_export(fsal_hdl, &myself->export.exports);
err_free:
	free_export_ops(&myself->export);
	gsh_free(myself); /* elvis has left the building */
    return status;
}

/**
 * dotfs_export_ops_init — initialize the DOTFS export ops table.
 * 
 * @param[in,out] ops Pointer to the export ops table to initialize.
 */
void dotfs_export_ops_init(struct export_ops *ops)
{
    ops->release                = dotfs_export_release;
    ops->lookup_path            = dotfs_export_lookup_path;
    ops->wire_to_host           = dotfs_export_wire_to_host;
    ops->create_handle          = dotfs_export_create_handle;
    ops->get_fs_dynamic_info    = dotfs_export_get_fs_dynamic_info;
    ops->fs_supported_attrs     = dotfs_export_fs_supported_attrs;
    ops->alloc_state            = dotfs_export_alloc_state;
    ops->get_fsal_obj_hdl       = dotfs_export_get_fsal_obj_hdl;
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

    fsal_status_t status = { ERR_FSAL_NOTSUPP, 0 };

    LogInfoMsg("dotfs_update_export: Not implemented yet. Returning ENOTSUPP.");

    return status;
}

/**
 * @brief Destructor for the custom FSAL export instance.
 *
 * Invoked when Ganesha is done with an export and wants to free its resources.
 * It performs complete cleanup of backend storage links, local structures,
 * and memory resources.
 *
 * @param[in] exp_hdl Pointer to the generic Ganesha fsal_export structure.
 */
static void dotfs_export_release(struct fsal_export *exp_hdl)
{
    dotfs_fsal_export_t *myself = container_of(exp_hdl, dotfs_fsal_export_t, export);

    // TODO: Change this to debug log
    LogInfoMsg("Releasing DOTFS export id=%u, path=%s",
		 exp_hdl->export_id, myself->export_path);

	/* TODO: flush in-flight I/O before closing the VFS context. */
    
    dotfs_close_ctx(myself->dotfs_ctx);
    myself->dotfs_ctx = NULL;
    
    gsh_free(myself->export_path);
	myself->export_path = NULL;

    gsh_free(myself->root_handle);
    myself->root_handle = NULL;

	/* Detach from Ganesha's export registry and free export ops. */
    fsal_detach_export(exp_hdl->fsal, &exp_hdl->exports);
	free_export_ops(exp_hdl);

    gsh_free(myself); /* elvis has left the building */
    myself = NULL;
}

/**
 * @brief Resolves a configuration path into a root object handle and fetches its attributes.
 *
 * Invoked during export initialization. This function translates the human-readable 
 * configuration path into an internal FSAL object handle representing the export's root, 
 * while simultaneously retrieving its initial metadata attributes.
 *
 * @param[in]  exp_hdl    Pointer to the generic Ganesha export structure.
 * @param[in]  path       The configuration path string to be resolved.
 * @param[out] handle     Pointer to store the newly allocated root object handle.
 * @param[out] attrs_out  Pointer to store the root object's initial metadata attributes.
 * 
 * @return fsal_status_t  Returns success status or an appropriate FSAL error code.
 */
fsal_status_t dotfs_export_lookup_path(struct fsal_export *exp_hdl, const char *path,
			      struct fsal_obj_handle **obj_hdl,
			      struct fsal_attrlist *attrs_out)
{
    LogInfoMsg("Resolving path '%s' for export", path);

    if (!exp_hdl || !path || !obj_hdl || !attrs_out) {
        return fsalstat(ERR_FSAL_FAULT, 0);
    }

    *obj_hdl = NULL;

    /*
     * Enforce that this placeholder export only boots if the config 
     * path matches your expected placeholder root exactly.
     */
    if (strcmp(path, CTX_FULLPATH(op_ctx)) != 0) {
        LogCrit(COMPONENT_FSAL, "DOTFS does not support exporting nested physical paths directly: %s", path);
        return fsalstat(ERR_FSAL_INVAL, 0);
    }

    dotfs_fsal_export_t *export = container_of(exp_hdl, dotfs_fsal_export_t, export);

    // TODO: Revisit here for fsid generation
    fsal_fsid_t fsid = { .major = 0x12345678, .minor = 0x1 };

    struct fsal_attrlist root_attrs;
	set_root_attrs(&root_attrs, export->export_id, fsid);


    export->root_handle = dotfs_alloc_handle(export, &root_attrs,
                HANDLE_TYPE_GLOBAL_ROOT, export->export_path);

    fsal_copy_attrs(&root_attrs, attrs_out, false);
    *obj_hdl = &export->root_handle->fsal_handle;

    LogInfoMsg("Successfully created Global Root Handle for path: %s (%p)", path, *obj_hdl);

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Reconstructs an internal object handle from a raw network file handle.
 *
 * Invoked when an NFS client sends an existing file handle over the network. 
 * This function decodes the raw byte descriptor into a valid, in-memory 
 * FSAL object handle, allowing Ganesha to perform active I/O operations on it.
 *
 * @param[in]  exp_hdl     Pointer to the generic Ganesha export structure.
 * @param[in]  handle_desc Pointer to the raw byte buffer received from the wire.
 * @param[out] obj_hdl     Pointer to store the reconstructed FSAL object handle.
 * @param[out] attrs_out   Pointer to store the object's initial metadata attributes.
 * 
 * @return fsal_status_t   Returns success status or an appropriate FSAL error code.
 */
fsal_status_t dotfs_export_create_handle(struct fsal_export *exp_hdl,
				  struct gsh_buffdesc *hdl_desc,
				  struct fsal_obj_handle **obj_hdl,
				  struct fsal_attrlist *attrs_out)
{
    dotfs_fsal_export_t *myself = container_of(exp_hdl, dotfs_fsal_export_t, export);

    dotfs_file_handle_t *fh = (dotfs_file_handle_t *) hdl_desc->addr;
    fh->handle_len = le16toh(fh->handle_len);
    
    LogInfoMsg("Reconstructing object handle from raw descriptor for '%s'", (char *) fh->handle_data);

    fsal_fsid_t fsid = { .major = 0x12345678, .minor = 0x1 };
    struct fsal_attrlist root_attrs;
	set_root_attrs(&root_attrs, myself->export_id, fsid);
    
    *obj_hdl = NULL;
    dotfs_fsal_obj_handle_t *dotfs_obj_handle = dotfs_alloc_handle(myself, &root_attrs,
                                HANDLE_TYPE_GLOBAL_ROOT, fh->handle_data);

    *obj_hdl = &dotfs_obj_handle->fsal_handle;

    // High-Level Logic: What your implementation must do
    // When Ganesha invokes your myfs_create_handle, your code will typically follow these steps:
    //      1. Extract Custom Data: Cast handle_desc->addr into your FSAL's internal identifier structure (e.g., an inode wrapper or database UUID).
    //      2. Sanity Check: Ensure the handle data is valid and actually belongs to this export.
    //      3. Check Cache / Allocate: Check if your FSAL already has this object active in memory. If not, allocate memory for your custom object handle structure.
    //      4. Reconstruct: Re-link the handle to your storage backend using the unique ID decoded from the bytes.
    //      5. Set Operations: Assign file or directory operations (fsal_obj_ops) to this newly resurrected handle so Ganesha knows how to read/write to it.
    //      6. Return: Assign the output pointer and return ERR_FSAL_NO_ERROR.

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Decode a digested handle
 *
 * This function decodes a previously digested handle.
 *
 * @param[in]  exp_hdl  Handle of the relevant fs export
 * @param[in]  in_type  The type of digest being decoded
 * @param[out] fh_desc  Address and length of key
 * @param[in]  flags    Flags for the operation
 */
static fsal_status_t dotfs_export_wire_to_host(struct fsal_export *exp_hdl,
					fsal_digesttype_t in_type,
					struct gsh_buffdesc *fh_desc, int flags)
{
    dotfs_file_handle_t *fh = (dotfs_file_handle_t *)fh_desc->addr;

    LogInfoMsg("Decoding digested handle of type %d of len=%lu", in_type, fh_desc->len);

    // if (fh_desc->len < sizeof(uint16_t)) {
    //     LogMajor(COMPONENT_FSAL, "Incoming file handle buffer is too small");
    //     return fsalstat(ERR_FSAL_SERVERFAULT, 0);
    // }

    switch (in_type) {
    case FSAL_DIGEST_NFSV3:
    case FSAL_DIGEST_NFSV4:
        /* 
         * Convert the 16-bit handle_len from network format (Little-Endian / Wire format)
         * to Host native format.
         */
        fh->handle_len = le16toh(fh->handle_len);

        if (fh->handle_len > DOTFS_HANDLE_MAX_LEN) {
            LogMajor(COMPONENT_FSAL, "Malformed handle: handle_len (%u) exceeds maximum (%d)", 
                     fh->handle_len, DOTFS_HANDLE_MAX_LEN);
            return fsalstat(ERR_FSAL_SERVERFAULT, 0);
        }

        fh_desc->len = sizeof(uint16_t) + fh->handle_len;
        break;

    default:
        return fsalstat(ERR_FSAL_SERVERFAULT, 0);
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Retrieves dynamic file system statistics like total, free, and available space.
 *
 * Invoked when an NFS client requests disk usage statistics (e.g., via the 'df' command).
 * This function queries the custom storage backend for its current capacity, free space, 
 * and inode utilization, filling out Ganesha's dynamic info structure.
 *
 * @param[in]  exp_hdl    Pointer to the generic Ganesha export structure.
 * @param[in]  obj_hdl    Pointer to the object handle used to anchor the query.
 * @param[out] info       Pointer to the structure where storage space stats must be saved.
 * 
 * @return fsal_status_t  Returns success status or an appropriate FSAL error code.
 */
static fsal_status_t dotfs_export_get_fs_dynamic_info(struct fsal_export *exp_hdl,
				      struct fsal_obj_handle *obj_hdl,
				      fsal_dynamicfsinfo_t *info)
{
    dotfs_fsal_export_t *myself = container_of(exp_hdl, dotfs_fsal_export_t, export);
    // struct myfs_backend_stats stats;
    // int rc;

    if (!exp_hdl || !info || !obj_hdl) {
        return fsalstat(ERR_FSAL_FAULT, EINVAL);
    }

    LogInfoMsg("Returning dynamic file system statistics for export=%s", myself->export_path);

    // /* Step 2: Query your backend storage for its raw numbers */
    // rc = myfs_backend_query_space(myself->dotfs_ctx, &stats);
    // if (rc != 0) {
    //     LogCrit(COMPONENT_FSAL, "Failed to retrieve backend storage statistics");
    //     return fsalstat(ERR_FSAL_IO, EIO);
    // }

    // /* 
    //  * Step 3: Populate the Ganesha structure.
    //  * Translate your storage block architecture into raw bytes.
    //  */
    // info->total_bytes = (uint64_t)stats.total_blocks * stats.block_size;
    // info->free_bytes  = (uint64_t)stats.free_blocks  * stats.block_size;
    
    // /* 
    //  * avail_bytes represents space usable by unprivileged users. 
    //  * If your backend doesn't have reserved root blocks, set it equal to free_bytes.
    //  */
    // info->avail_bytes = (uint64_t)stats.available_blocks * stats.block_size;

    // /* Map backend inode tracking metrics */
    // info->total_files = stats.total_inodes;
    // info->free_files  = stats.free_inodes;

	memset(info, 0, sizeof(fsal_dynamicfsinfo_t));

    // TODO: Fetch it from storage backend instead of hardcoding

    info->total_bytes = 214748364800;
    info->free_bytes = 210453397504;
    info->avail_bytes = 210453397504;

    info->total_files = 1000000;
    info->free_files = 999000;
    info->avail_files = 999000;

    // Set file timestamp granularity
    info->time_delta.tv_sec = 0;
	info->time_delta.tv_nsec = FSAL_DEFAULT_TIME_DELTA_NSEC;


    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Reports the total metadata attribute capabilities supported by this export.
 *
 * Invoked during export initialization or capability inspection. This function returns 
 * an optimized attrmask_t structure outlining every file attribute your backend 
 * storage volume can successfully read or modify.
 *
 * @param[in]  exp_hdl    Pointer to the generic Ganesha export structure.
 * 
 * @return attrmask_t     The finalized mask of all active attribute bits.
 */
static attrmask_t dotfs_export_fs_supported_attrs(struct fsal_export *exp_hdl)
{
    attrmask_t supported_mask;
    dotfs_fsal_export_t *myself = container_of(exp_hdl, dotfs_fsal_export_t, export);

    LogInfoMsg("Reporting supported metadata attributes for export %s", myself->export_path);

    supported_mask = fsal_supported_attrs(&exp_hdl->fsal->fs_info);
	supported_mask &= ~ATTR_ACL;

    return supported_mask;
}

void dotfs_export_free_state(struct state_t *state)
{
	dotfs_fd_t *my_fd;
	my_fd = &container_of(state, dotfs_state_fd_t, state)->dotfs_fd;

	destroy_fsal_fd(&my_fd->fsal_fd);
	gsh_free(state);
}

/**
 * @brief Allocates the memory wrapper used to track active NFSv4 file opens and locks.
 *
 * Invoked whenever an NFS client performs a stateful operation (like opening a file or 
 * requesting a file lock). This function allocates your FSAL's custom state structure, 
 * helping the server remember and enforce open/lock ownership.
 *
 * @param[in]  exp_hdl      Pointer to the generic Ganesha export structure.
 * @param[in]  state_type   The nature of the state (e.g., File Open, Record Lock).
 * @param[in]  parent_state Link to the parent state block (useful for mapping locks to opens).
 * 
 * @return struct fsal_state*  Pointer to Ganesha's core tracking state object inside your wrapper.
 */
static struct state_t *dotfs_export_alloc_state(struct fsal_export *exp_hdl,
					 enum state_type state_type,
					 struct state_t *related_state)
{
    struct state_t *state;
	dotfs_fd_t *my_fd;

    LogInfoMsg("Allocating state for type %d", state_type);

	state = init_state(gsh_calloc(1, sizeof(dotfs_state_fd_t)),
			   dotfs_export_free_state, state_type, related_state);

	my_fd = &container_of(state, dotfs_state_fd_t, state)->dotfs_fd;

    // TODO: What is op_ctx->fsal_export here?
	init_fsal_fd(&my_fd->fsal_fd, FSAL_FD_STATE, op_ctx->fsal_export);

	return state;
}

/**
 * @brief Resolves an active file descriptor back into its corresponding object handle.
 *
 * Invoked internally by Ganesha's execution layers. Given an open file descriptor 
 * structure (fsal_fd), this function maps it back to its underlying tracking 
 * object handle structure, updating the provided handle pointer directly.
 *
 * @param[in]  exp_hdl  Pointer to the generic Ganesha export structure.
 * @param[in]  fd       Pointer to the active file descriptor structure being mapped.
 * @param[out] handle   Pointer to store the resolved object handle destination.
 */
void dotfs_export_get_fsal_obj_hdl(struct fsal_export *exp_hdl, struct fsal_fd *fd,
		      struct fsal_obj_handle **handle)
{
    LogInfoMsg("Retrieving FSAL object handle from file descriptor");

    dotfs_fd_t *my_fd = NULL;
    dotfs_fsal_obj_handle_t *myself = NULL;

    my_fd = container_of(fd, dotfs_fd_t, fsal_fd);
    myself = container_of(my_fd, dotfs_fsal_obj_handle_t, u.file.fd);

    *handle = &myself->fsal_handle;
}

/**
 * dotfs_close_ctx — release a previously created dotfs context.
 *
 * @param[in] ctx  context returned by dotfs_open_ctx().
 */
static void dotfs_close_ctx(dotfs_context_t *ctx)
{
    // TODO: Implement the actual context cleanup logic here
    // such as closing file descriptors, freeing memory, 
    // and releasing any other resources associated with the context.
	(void)ctx;
}
