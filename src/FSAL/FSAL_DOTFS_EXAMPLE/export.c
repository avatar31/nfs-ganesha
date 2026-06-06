// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * export.c
 * FSAL_DOTFS — export lifecycle and export-level operations.
 *
 * Responsibilities:
 *   - Allocate and initialise a dotfs_fsal_export on Ganesha export creation.
 *   - Open the dotfs VFS context that backs the exported namespace path.
 *   - Implement fsal_export_ops: dynamic filesystem info, quota, path/handle
 *     lookup, and clean export teardown.
 *
 * One dotfs_fsal_export is created per NFS export entry that names DOTFS
 * as its FSAL.  The export holds an opaque dotfs_ctx handle which represents
 * a live connection to the dotfs metadata store (omashu) and storage engine.
 */

#include "config.h"

#include <pthread.h>
#include <string.h>
#include <sys/statvfs.h>

#include "fsal.h"
#include "fsal_convert.h"
#include "config_parsing.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_config.h"
#include "export_mgr.h"
#include "nfs_exports.h"
#include "gsh_list.h"

#include "dotfs_methods.h"

/* =========================================================================
 * Config block
 * ========================================================================= */

/**
 * Per-export configuration items parsed from the Ganesha config file block:
 *
 *   Export {
 *       FSAL { name = DOTFS; dotfs_meta_addr = "localhost:2379"; }
 *   }
 */
static struct config_item dotfs_export_params[] = {
	CONF_ITEM_NOOP("name"),
	/* TODO: add dotfs-specific export params, e.g.:
	 *   CONF_ITEM_STR("dotfs_meta_addr", 1, MAXPATHLEN,
	 *                 "localhost:2379", dotfs_fsal_export, meta_addr),
	 *   CONF_ITEM_UI32("data_shards", 1, 16, 4,
	 *                  dotfs_fsal_export, data_shards),
	 *   CONF_ITEM_UI32("parity_shards", 1, 8, 2,
	 *                  dotfs_fsal_export, parity_shards),
	 */
	CONFIG_EOL
};

static struct config_block dotfs_export_param_block = {
	.dbus_interface_name = "org.ganesha.nfsd.config.fsal.dotfs-export%d",
	.blk_desc.name = "FSAL",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = dotfs_export_params,
	.blk_desc.u.blk.commit = noop_conf_commit,
};

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * dotfs_open_vfs_ctx — open a dotfs VFS context for the given export path.
 *
 * @param[in]  export_path  Logical dotfs root path for this export.
 * @param[out] ctx_out      Populated with the new opaque VFS context handle.
 *
 * @return 0 on success, negative errno on failure.
 *
 * TODO: implement via the dotfs C-binding (CGo / cgo-generated header).
 *       Expected call: dotfs_open(cfg, &ctx);  where cfg includes the
 *       omashu cluster address, data/parity shard counts, and drive paths.
 */
static int dotfs_open_vfs_ctx(const char *export_path, void **ctx_out)
{
	/* TODO: construct dotfs.Config from ganesha config parameters and
	 *       call the dotfs C binding to create a VFS context.
	 *       Example:
	 *         struct DotfsConfig cfg = { .base_path = export_path, ... };
	 *         return dotfs_new_vfs(&cfg, ctx_out);
	 */
	(void)export_path;
	*ctx_out = NULL;
	return -ENOSYS;
}

/**
 * dotfs_close_vfs_ctx — release a previously opened dotfs VFS context.
 *
 * @param[in] ctx  VFS context returned by dotfs_open_vfs_ctx().
 *
 * TODO: call the dotfs C-binding close/shutdown function.
 */
static void dotfs_close_vfs_ctx(void *ctx)
{
	/* TODO: dotfs_close_vfs(ctx); */
	(void)ctx;
}

/* =========================================================================
 * Export ops
 * ========================================================================= */

/**
 * dotfs_release_export — tear down a dotfs export and free all resources.
 *
 * Called by Ganesha when the export is being removed (e.g. on SIGHUP with
 * the export removed, or during graceful shutdown).  Must:
 *   1. Close the dotfs VFS context.
 *   2. Release any filesystem claim maps.
 *   3. Detach from the Ganesha FSAL export list.
 *   4. Free the dotfs_fsal_export allocation.
 *
 * @param[in] exp_hdl  Generic Ganesha export handle (owned by us).
 */
void dotfs_release_export(struct fsal_export *exp_hdl)
{
	struct dotfs_fsal_export *myself = DOTFS_EXPORT(exp_hdl);

	LogDebug(COMPONENT_FSAL,
		 "Releasing DOTFS export %" PRIu16 " path=%s",
		 exp_hdl->export_id, myself->root_path ? myself->root_path : "(null)");

	/* TODO: flush in-flight I/O before closing the VFS context. */
	dotfs_close_vfs_ctx(myself->dotfs_ctx);
	myself->dotfs_ctx = NULL;

	gsh_free(myself->root_path);
	myself->root_path = NULL;

	/* Detach from Ganesha's export registry and free export ops. */
	fsal_detach_export(exp_hdl->fsal, &exp_hdl->exports);
	free_export_ops(exp_hdl);

	gsh_free(myself);
}

/**
 * dotfs_get_dynamic_info — report live filesystem capacity metrics.
 *
 * Fills in @p infop with the total/available storage space and inode
 * counts as seen by the dotfs StorageEngine.  Called by Ganesha when an
 * NFS FSSTAT or FSINFO RPC is received.
 *
 * @param[in]  exp_hdl  The export whose filesystem is being queried.
 * @param[in]  obj_hdl  Any object handle within the filesystem (may be NULL).
 * @param[out] infop    Output structure for capacity figures.
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: implement by querying the dotfs StorageEngine for aggregate drive
 *       utilisation figures.  Until then, returns ERR_FSAL_NOTSUPP.
 */
fsal_status_t dotfs_get_dynamic_info(struct fsal_export *exp_hdl,
				     struct fsal_obj_handle *obj_hdl,
				     fsal_dynamicfsinfo_t *infop)
{
	/* TODO: query dotfs VFS context for capacity stats:
	 *   struct DotfsFSStats stats;
	 *   int rc = dotfs_statvfs(myself->dotfs_ctx, &stats);
	 *   if (rc != 0) return fsalstat(posix2fsal_error(-rc), -rc);
	 *   infop->total_bytes = stats.total_bytes;
	 *   infop->free_bytes  = stats.free_bytes;
	 *   infop->avail_bytes = stats.available_bytes;
	 *   infop->total_files = stats.total_inodes;
	 *   infop->free_files  = stats.free_inodes;
	 *   infop->avail_files = stats.free_inodes;
	 *   infop->time_delta  = (struct timespec){ .tv_sec = 1, .tv_nsec = 0 };
	 */
	(void)exp_hdl;
	(void)obj_hdl;
	(void)infop;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_lookup_path — resolve a server-side path to an object handle.
 *
 * Used during export mount and by Ganesha's pseudo-FS when it needs to
 * turn the export root path into a FSAL object handle.
 *
 * @param[in]  exp_hdl    The export whose namespace to search.
 * @param[in]  path       Absolute path inside the dotfs namespace.
 * @param[out] handle     Returns a new object handle on success.
 * @param[out] attrs_out  Optional: attributes of the located object.
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_NOENT if not found.
 *
 * TODO: resolve via the dotfs metadata store (omashu GetByPrefix/Get).
 *       Allocate a dotfs_fsal_obj_handle with dotfs_alloc_handle().
 */
fsal_status_t dotfs_lookup_path(struct fsal_export *exp_hdl, const char *path,
				struct fsal_obj_handle **handle,
				struct fsal_attrlist *attrs_out)
{
	/* TODO: look up 'path' in the dotfs namespace:
	 *   1. Strip the export root_path prefix to get a relative dotfs path.
	 *   2. Call dotfs metadata store to fetch ObjectStorageMeta for path.
	 *   3. Build a dotfs_obj_handle from the object ID and path.
	 *   4. Allocate via dotfs_alloc_handle() and populate *handle.
	 *   5. If attrs_out != NULL, fill POSIX attrs from ObjectStorageMeta.
	 */
	(void)exp_hdl;
	(void)path;
	(void)handle;
	(void)attrs_out;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_create_handle — reconstruct an object handle from its wire encoding.
 *
 * Called when an NFS client presents an opaque filehandle (FH) that was
 * previously issued by this server.  Must rebuild a live dotfs_fsal_obj_handle
 * from the serialised bytes in @p hdl_desc.
 *
 * @param[in]  exp_hdl    The export the client is accessing.
 * @param[in]  hdl_desc   Wire-encoded handle bytes (from dotfs_handle_to_wire).
 * @param[out] handle     Returns the reconstructed object handle.
 * @param[out] attrs_out  Optional: attributes of the reconstructed object.
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_STALE if the handle is no
 *         longer valid (object was deleted), ERR_FSAL_BADHANDLE if malformed.
 *
 * TODO: deserialise hdl_desc.addr bytes into a dotfs_obj_handle, verify the
 *       object still exists in the metadata store, and return a fresh handle.
 */
fsal_status_t dotfs_create_handle(struct fsal_export *exp_hdl,
				  struct gsh_buffdesc *hdl_desc,
				  struct fsal_obj_handle **handle,
				  struct fsal_attrlist *attrs_out)
{
	/* TODO:
	 *   1. Validate hdl_desc->len <= DOTFS_HANDLE_MAX_LEN.
	 *   2. Copy bytes into a local dotfs_obj_handle.
	 *   3. Verify the encoded object_id still exists in the metadata store.
	 *   4. Allocate and return a new dotfs_fsal_obj_handle.
	 */
	(void)exp_hdl;
	(void)hdl_desc;
	(void)handle;
	(void)attrs_out;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_get_quota — query disk quota for a user/group on this export.
 *
 * @param[in]  exp_hdl     Export handle.
 * @param[in]  filepath    Path used to identify the filesystem.
 * @param[in]  quota_type  FSAL_QUOTA_BLOCKS or FSAL_QUOTA_INODES.
 * @param[out] pquota      Quota values to populate.
 *
 * @return ERR_FSAL_NOTSUPP until quota tracking is implemented in dotfs.
 *
 * TODO: dotfs does not yet model per-user quotas.  Implement after the
 *       POSIX namespace layer (P2 roadmap item) is in place.
 */
fsal_status_t dotfs_get_quota(struct fsal_export *exp_hdl,
			      const char *filepath, int quota_type,
			      fsal_quota_t *pquota)
{
	/* TODO: implement quota queries via the dotfs metadata store. */
	(void)exp_hdl;
	(void)filepath;
	(void)quota_type;
	(void)pquota;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_set_quota — apply disk quota limits for a user/group.
 *
 * @param[in]  exp_hdl     Export handle.
 * @param[in]  filepath    Path identifying the filesystem.
 * @param[in]  quota_type  FSAL_QUOTA_BLOCKS or FSAL_QUOTA_INODES.
 * @param[in]  pquota      Quota limits to set.
 * @param[out] presquota   Resulting quota after the operation.
 *
 * @return ERR_FSAL_NOTSUPP until quota enforcement is implemented.
 *
 * TODO: implement after the POSIX namespace and per-user TTL/lifecycle
 *       policies (P2 roadmap) are in place.
 */
fsal_status_t dotfs_set_quota(struct fsal_export *exp_hdl,
			      const char *filepath, int quota_type,
			      fsal_quota_t *pquota, fsal_quota_t *presquota)
{
	/* TODO: implement quota enforcement via the dotfs metadata store. */
	(void)exp_hdl;
	(void)filepath;
	(void)quota_type;
	(void)pquota;
	(void)presquota;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/* =========================================================================
 * Export creation (called by Ganesha on export activation)
 * ========================================================================= */

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
	struct dotfs_fsal_export *myself;
	fsal_status_t status = { ERR_FSAL_NO_ERROR, 0 };
	int rc;

	myself = gsh_calloc(1, sizeof(*myself));

	/* Initialise Ganesha's base export struct and default ops. */
	fsal_export_init(&myself->export);
	export_ops_init(&myself->export.exp_ops);

	/* Override with DOTFS-specific export ops. */
	myself->export.exp_ops.release = dotfs_release_export;
	myself->export.exp_ops.get_fs_dynamic_info = dotfs_get_dynamic_info;
	myself->export.exp_ops.lookup_path = dotfs_lookup_path;
	myself->export.exp_ops.create_handle = dotfs_create_handle;
	myself->export.exp_ops.get_quota = dotfs_get_quota;
	myself->export.exp_ops.set_quota = dotfs_set_quota;

	/* Parse the per-export DOTFS config block. */
	rc = load_config_from_node(parse_node, &dotfs_export_param_block,
				   myself, true, err_type);
	if (rc != 0 && !config_error_is_harmless(err_type)) {
		status = fsalstat(ERR_FSAL_INVAL, 0);
		goto err_free;
	}

	/* TODO: extract export root path from op_ctx or parse_node.
	 *       Currently set to "/" as a placeholder.
	 */
	myself->root_path = gsh_strdup("/");

	/* Open the underlying dotfs VFS context. */
	rc = dotfs_open_vfs_ctx(myself->root_path, &myself->dotfs_ctx);
	if (rc != 0) {
		LogCrit(COMPONENT_FSAL,
			"DOTFS: failed to open VFS context for path %s: %s",
			myself->root_path, strerror(-rc));
		/* TODO: downgrade to a warning and continue once the dotfs
		 *       C-binding is implemented.  Currently an error so the
		 *       export init surface is correct. */
	}

	/* Attach to the FSAL module. */
	rc = fsal_attach_export(fsal_hdl, &myself->export.exports);
	if (rc != 0) {
		status = fsalstat(posix2fsal_error(rc), rc);
		goto err_ctx;
	}

	myself->export.fsal = fsal_hdl;

	/* Initialise the up_ops for cache-invalidation callbacks. */
	myself->export.up_ops = up_ops;

	LogInfo(COMPONENT_FSAL, "DOTFS export created: path=%s",
		myself->root_path);
	return status;

err_ctx:
	dotfs_close_vfs_ctx(myself->dotfs_ctx);
	gsh_free(myself->root_path);
err_free:
	gsh_free(myself);
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
	/* TODO: load new config, compare against current export state,
	 *       and apply changed fields atomically without disrupting
	 *       in-flight NFS operations.
	 */
	(void)fsal_hdl;
	(void)parse_node;
	(void)err_type;
	(void)original;
	(void)updated_super;
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}
