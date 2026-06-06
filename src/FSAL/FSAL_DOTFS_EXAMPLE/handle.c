// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * handle.c
 * FSAL_DOTFS — filesystem object (file, directory, symlink, …) handle ops.
 *
 * This file implements the fsal_obj_ops function table for DOTFS objects.
 * Each function corresponds to one VFS operation that Ganesha's protocol
 * layer (NFSv3/v4, 9P, …) may invoke on a filesystem object.
 *
 * Object handles (dotfs_fsal_obj_handle) carry a dotfs_obj_handle that
 * encodes the object's stable identity as:
 *
 *   "<36-char UUID>:<logical-path>"
 *
 * This encoding survives renames (UUID is immutable) and gives a human-
 * readable path component for debugging.  The UUID maps to an
 * ObjectStorageMeta entry in the omashu metadata store.
 *
 * Thread-safety: Ganesha serialises per-handle ops with its own MDCACHE
 * locks; individual functions here need not take additional per-handle locks
 * unless they update shared state (e.g. the symlink cache).
 */

#include "config.h"

#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_commonlib.h"
#include "gsh_list.h"

#include "dotfs_methods.h"

/* =========================================================================
 * Handle allocation and teardown
 * ========================================================================= */

/**
 * dotfs_alloc_handle — allocate and partially initialise a new object handle.
 *
 * Allocates a dotfs_fsal_obj_handle, copies the serialised dotfs handle
 * bytes into it, and performs the generic Ganesha fsal_obj_handle_init().
 * The caller is responsible for filling in type-specific union fields
 * (file.fd / symlink.link_content / unopenable.dir+name) after this returns.
 *
 * Ownership: the returned handle is owned by Ganesha's MDCACHE once
 * inserted.  Free via dotfs_free_obj_handle() before insertion if an
 * error occurs.
 *
 * @param[in] exp    The export that owns this handle.
 * @param[in] fh     Serialised dotfs object handle to copy in.
 * @param[in] type   Ganesha object type (REGULAR_FILE, DIRECTORY, …).
 * @param[in] sb     POSIX stat buffer used to populate initial attributes.
 *                   May be NULL if attributes are not yet known.
 * @param[in] path   Logical dotfs path (informational; stored in handle).
 *
 * @return Newly allocated handle on success; NULL on allocation failure.
 */
struct dotfs_fsal_obj_handle *
dotfs_alloc_handle(struct dotfs_fsal_export *exp,
		   const struct dotfs_obj_handle *fh, object_file_type_t type,
		   const struct stat *sb, const char *path)
{
	struct dotfs_fsal_obj_handle *hdl;

	hdl = gsh_calloc(1, sizeof(*hdl));

	/* Copy the serialised handle payload. */
	memcpy(&hdl->handle, fh, sizeof(*fh));

	/* Initialise the Ganesha generic object handle base. */
	fsal_obj_handle_init(&hdl->obj_handle, &exp->export, type);

	/* Attach our pre-built handle ops table. */
	hdl->obj_handle.obj_ops =
		&container_of(exp->export.fsal, struct dotfs_fsal_module,
			      module)->handle_ops;

	if (type == REGULAR_FILE) {
		hdl->u.file.fd.dotfs_stream = NULL;
		hdl->u.file.fd.fsal_fd.openflags = FSAL_O_CLOSED;
		/* TODO: init fsal_share once share-reservation support is added. */
	}

	/* TODO: if sb != NULL, populate hdl->obj_handle cached attributes
	 *       from the stat buffer to avoid an extra metadata round-trip.
	 */
	(void)sb;
	(void)path;

	return hdl;
}

/**
 * dotfs_free_obj_handle — release a dotfs object handle and all owned memory.
 *
 * Called from dotfs_release_obj() (the fsal_obj_ops release callback) and
 * from error paths in allocation code before the handle is inserted into
 * Ganesha's MDCACHE.
 *
 * Nulls *hdl_ref after freeing so callers can safely detect double-free.
 *
 * @param[in,out] hdl_ref  Pointer to the handle pointer.  Set to NULL.
 */
void dotfs_free_obj_handle(struct dotfs_fsal_obj_handle **hdl_ref)
{
	struct dotfs_fsal_obj_handle *myself = *hdl_ref;
	object_file_type_t type = myself->obj_handle.type;

	if (type == SYMBOLIC_LINK) {
		gsh_free(myself->u.symlink.link_content);
	} else if (type == REGULAR_FILE) {
		/* TODO: assert fd is closed; forcibly close if not? */
		destroy_fsal_fd(&myself->u.file.fd.fsal_fd);
	} else if (dotfs_unopenable_type(type)) {
		gsh_free(myself->u.unopenable.dir);
		gsh_free(myself->u.unopenable.name);
	}

	fsal_obj_handle_fini(&myself->obj_handle);

	LogDebug(COMPONENT_FSAL,
		 "DOTFS: releasing obj_hdl=%p myself=%p",
		 &myself->obj_handle, myself);

	gsh_free(myself);
	*hdl_ref = NULL;
}

/* =========================================================================
 * fsal_obj_ops callbacks
 * ========================================================================= */

/**
 * dotfs_release_obj — decrement handle refcount; free when it reaches zero.
 *
 * Ganesha calls this when it evicts a handle from MDCACHE.  Must not be
 * called while any state (NFSv4 open, lock, delegation) still references
 * the handle.
 *
 * @param[in] obj_hdl  The Ganesha object handle to release.
 */
void dotfs_release_obj(struct fsal_obj_handle *obj_hdl)
{
	struct dotfs_fsal_obj_handle *myself = DOTFS_OBJ(obj_hdl);

	/* TODO: assert no active open states remain on this handle. */
	dotfs_free_obj_handle(&myself);
}

/**
 * dotfs_merge — reconcile a duplicate handle from the MDCACHE lookup path.
 *
 * When Ganesha's MDCACHE finds that two lookup paths resolve to the same
 * underlying object (same handle key), it calls merge() on the original and
 * the newly-created duplicate.  FSAL must transfer any ephemeral state from
 * the duplicate to the original, then the duplicate is discarded.
 *
 * @param[in] orig_hdl  The handle already in MDCACHE (canonical copy).
 * @param[in] dupe_hdl  The newly-created, not-yet-inserted duplicate.
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: transfer open-state / share-reservation data from dupe to orig
 *       once NFSv4 state is implemented.
 */
fsal_status_t dotfs_merge(struct fsal_obj_handle *orig_hdl,
			  struct fsal_obj_handle *dupe_hdl)
{
	fsal_status_t status = { ERR_FSAL_NO_ERROR, 0 };

	if (orig_hdl->type == REGULAR_FILE && dupe_hdl->type == REGULAR_FILE) {
		/* TODO: merge share reservations.
		 *   struct dotfs_fsal_obj_handle *orig = DOTFS_OBJ(orig_hdl);
		 *   struct dotfs_fsal_obj_handle *dupe = DOTFS_OBJ(dupe_hdl);
		 *   status = merge_share(&orig->u.file.share, &dupe->u.file.share);
		 */
	}

	return status;
}

/**
 * dotfs_lookup — look up a directory entry by name.
 *
 * Resolves @p name within the directory @p parent and returns a new
 * object handle for the found entry.  This is the core "namei" operation;
 * every path traversal bottoms out here.
 *
 * @param[in]  parent     Directory handle to search in.
 * @param[in]  name       Entry name (NUL-terminated, single component).
 * @param[out] handle     New handle for the found object on success.
 * @param[out] attrs_out  Optional: attributes of the found object.
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_NOENT if not found.
 *
 * TODO: issue a Get/GetByPrefix to the omashu metadata store keyed by
 *       parent_path + "/" + name, deserialise ObjectStorageMeta, build
 *       handle with dotfs_alloc_handle(), populate attrs_out.
 */
fsal_status_t dotfs_lookup(struct fsal_obj_handle *parent, const char *name,
			   struct fsal_obj_handle **handle,
			   struct fsal_attrlist *attrs_out)
{
	/* TODO:
	 *   struct dotfs_fsal_obj_handle *parent_hdl = DOTFS_OBJ(parent);
	 *   struct dotfs_fsal_export *exp = DOTFS_EXPORT(op_ctx->ctx_export->fsal_export);
	 *   char child_path[PATH_MAX];
	 *   snprintf(child_path, sizeof(child_path), "%s/%s",
	 *            parent_path_from_handle(parent_hdl), name);
	 *   ObjectStorageMeta *meta = dotfs_meta_get(exp->dotfs_ctx, child_path);
	 *   if (!meta) return fsalstat(ERR_FSAL_NOENT, 0);
	 *   struct dotfs_fsal_obj_handle *hdl =
	 *       dotfs_alloc_handle(exp, build_fh(meta), meta->type, NULL, child_path);
	 *   *handle = &hdl->obj_handle;
	 */
	(void)parent;
	(void)name;
	(void)handle;
	(void)attrs_out;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_readdir — enumerate directory entries.
 *
 * Iterates entries in the directory @p dir_hdl, calling @p cb for each one.
 * Supports resumable iteration via @p whence (cookie from a prior call).
 * Sets *eof = true when the last entry has been delivered.
 *
 * @param[in]  dir_hdl   Directory to iterate.
 * @param[in]  whence    Resume cookie (NULL or 0 = start from beginning).
 * @param[in]  dir_state Opaque caller state passed through to @p cb.
 * @param[in]  cb        Callback invoked per entry; return false to stop.
 * @param[in]  attrmask  Attribute bitmask requested by the caller.
 * @param[out] eof       Set to true when directory end is reached.
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: implement using omashu GetByPrefix with the directory path as
 *       prefix, and IterateByPrefix for cursor-based pagination.
 */
fsal_status_t dotfs_readdir(struct fsal_obj_handle *dir_hdl,
			    fsal_cookie_t *whence, void *dir_state,
			    fsal_readdir_cb cb, attrmask_t attrmask, bool *eof)
{
	/* TODO:
	 *   struct dotfs_fsal_obj_handle *dir = DOTFS_OBJ(dir_hdl);
	 *   const char *cursor = whence ? cookie_to_str(*whence) : "";
	 *   Use omashu IterateByPrefix(ctx, dir_path, cursor, PAGE_SIZE, ...)
	 *   For each entry: build handle, call cb(name, hdl, attrs, dir_state, &cookie).
	 *   Set *eof when IterateByPrefix returns empty next cursor.
	 */
	(void)dir_hdl;
	(void)whence;
	(void)dir_state;
	(void)cb;
	(void)attrmask;
	*eof = true;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_getattrs — retrieve POSIX attributes for an object.
 *
 * Called by Ganesha whenever it needs to fill in or refresh the attribute
 * cache for an object (GETATTR RPC, post-op attrs, etc.).
 *
 * @param[in]  obj_hdl   Object whose attributes to fetch.
 * @param[out] attrs_out Attribute structure to populate.
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_STALE if the object is gone.
 *
 * TODO: fetch ObjectStorageMeta from the omashu store and map fields:
 *   meta.Size          → attrs.filesize
 *   meta.ObjectId hash → attrs.fileid (stable inode number)
 *   meta.MTime         → attrs.mtime
 *   meta.CTime         → attrs.ctime
 *   (mode, uid, gid — stored as xattrs or in a separate meta key)
 */
fsal_status_t dotfs_getattrs(struct fsal_obj_handle *obj_hdl,
			     struct fsal_attrlist *attrs_out)
{
	/* TODO:
	 *   struct dotfs_fsal_obj_handle *myself = DOTFS_OBJ(obj_hdl);
	 *   ObjectStorageMeta *meta = dotfs_meta_get_by_id(exp->dotfs_ctx,
	 *                                                  handle_to_id(&myself->handle));
	 *   if (!meta) return fsalstat(ERR_FSAL_STALE, 0);
	 *   populate attrs_out from meta fields.
	 */
	(void)obj_hdl;
	(void)attrs_out;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_setattrs — apply POSIX attribute changes to an object.
 *
 * Handles chmod, chown, utimes, and file truncation.  @p bypass indicates
 * whether mandatory-lock / share-reservation checks should be skipped
 * (e.g. for internal server-side operations).
 *
 * @param[in] obj_hdl    Object to modify.
 * @param[in] bypass     If true, skip share/lock conflict checks.
 * @param[in] state      Open state (used for conflict checking); may be NULL.
 * @param[in] attrib_set Attributes to apply (VALID bit indicates which).
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: persist changes via an UpdateJson call on the omashu metadata key
 *       for this object.  Truncation requires dotfs StorageEngine support.
 */
fsal_status_t dotfs_setattrs(struct fsal_obj_handle *obj_hdl, bool bypass,
			     struct state_t *state,
			     struct fsal_attrlist *attrib_set)
{
	/* TODO:
	 *   - If ATTR_SIZE is set: call dotfs StorageEngine truncate.
	 *   - For mode/uid/gid/times: UpdateJson on the metadata key.
	 *   - Honour bypass flag for share-reservation conflict detection.
	 */
	(void)obj_hdl;
	(void)bypass;
	(void)state;
	(void)attrib_set;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_mkdir — create a new directory.
 *
 * Creates a directory entry in the dotfs namespace at @p name inside
 * the directory @p dir_hdl.
 *
 * @param[in]  dir_hdl   Parent directory.
 * @param[in]  name      New directory name (single path component).
 * @param[in]  attrib    Initial attributes (mode, owner) for the new dir.
 * @param[out] new_obj   Handle for the newly created directory.
 * @param[out] attrs_out Optional: attributes of the new directory.
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_EXIST if name already exists.
 *
 * TODO: create an ObjectStorageMeta entry of type DIRECTORY in omashu,
 *       atomically guarded by a transaction to prevent duplicate creation.
 */
fsal_status_t dotfs_mkdir(struct fsal_obj_handle *dir_hdl, const char *name,
			  struct fsal_attrlist *attrib,
			  struct fsal_obj_handle **new_obj,
			  struct fsal_attrlist *attrs_out)
{
	/* TODO:
	 *   - Compose new path: parent_path + "/" + name.
	 *   - Open an omashu transaction.
	 *   - Check for existing entry (ERR_FSAL_EXIST on conflict).
	 *   - Store new ObjectStorageMeta{type=DIRECTORY, ...}.
	 *   - Commit transaction.
	 *   - Allocate handle via dotfs_alloc_handle().
	 */
	(void)dir_hdl;
	(void)name;
	(void)attrib;
	(void)new_obj;
	(void)attrs_out;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_mknode — create a special filesystem node (device, socket, FIFO).
 *
 * @param[in]  dir_hdl   Parent directory.
 * @param[in]  name      New node name.
 * @param[in]  nodetype  Type: CHARACTER_FILE, BLOCK_FILE, SOCKET_FILE, FIFO_FILE.
 * @param[in]  dev       Device major/minor numbers (for block/char devices).
 * @param[in]  attrib    Initial attributes.
 * @param[out] new_obj   Handle for the new node.
 * @param[out] attrs_out Optional: attributes of the new node.
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: store node metadata in omashu; note that device nodes cannot be
 *       opened by handle and use the unopenable union branch.
 */
fsal_status_t dotfs_mknode(struct fsal_obj_handle *dir_hdl, const char *name,
			   object_file_type_t nodetype, fsal_dev_t *dev,
			   struct fsal_attrlist *attrib,
			   struct fsal_obj_handle **new_obj,
			   struct fsal_attrlist *attrs_out)
{
	/* TODO: persist node metadata in omashu; populate unopenable union. */
	(void)dir_hdl;
	(void)name;
	(void)nodetype;
	(void)dev;
	(void)attrib;
	(void)new_obj;
	(void)attrs_out;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_symlink — create a symbolic link.
 *
 * Creates a symlink entry @p name in directory @p dir_hdl pointing to
 * @p link_path.  The target path is stored in omashu metadata; it is not
 * validated at creation time (dangling symlinks are legal POSIX behaviour).
 *
 * @param[in]  dir_hdl   Parent directory.
 * @param[in]  name      Symlink entry name.
 * @param[in]  link_path Symlink target (may be relative or absolute).
 * @param[in]  attrib    Initial attributes.
 * @param[out] new_obj   Handle for the new symlink.
 * @param[out] attrs_out Optional: attributes of the new symlink.
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: persist {type=SYMLINK, link_target=link_path} in omashu.
 */
fsal_status_t dotfs_symlink(struct fsal_obj_handle *dir_hdl, const char *name,
			    const char *link_path, struct fsal_attrlist *attrib,
			    struct fsal_obj_handle **new_obj,
			    struct fsal_attrlist *attrs_out)
{
	/* TODO: store symlink metadata in omashu; populate symlink union. */
	(void)dir_hdl;
	(void)name;
	(void)link_path;
	(void)attrib;
	(void)new_obj;
	(void)attrs_out;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_readlink — read the target of a symbolic link.
 *
 * Returns the stored target string for symlink @p obj_hdl.  May use a
 * cached value from the handle union if @p refresh is false.
 *
 * @param[in]  obj_hdl      Symlink object handle.
 * @param[out] link_content Buffer descriptor to receive the target string.
 * @param[in]  refresh      If true, bypass any cached value and re-fetch.
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_INVAL if not a symlink.
 *
 * TODO: if !refresh and u.symlink.link_content is set, return the cache.
 *       Otherwise fetch from omashu and update the cache.
 */
fsal_status_t dotfs_readlink(struct fsal_obj_handle *obj_hdl,
			     struct gsh_buffdesc *link_content, bool refresh)
{
	/* TODO:
	 *   struct dotfs_fsal_obj_handle *myself = DOTFS_OBJ(obj_hdl);
	 *   if (obj_hdl->type != SYMBOLIC_LINK) return fsalstat(ERR_FSAL_INVAL, 0);
	 *   if (!refresh && myself->u.symlink.link_content) { fill from cache; return; }
	 *   Fetch link_target from omashu for this object's ID.
	 *   Cache in myself->u.symlink.link_content.
	 *   Fill link_content->addr / len.
	 */
	(void)obj_hdl;
	(void)link_content;
	(void)refresh;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_link — create a hard link.
 *
 * Creates a new directory entry @p name in @p destdir_hdl that refers to
 * the same underlying object as @p obj_hdl.  Increments the object's
 * link count in the metadata store atomically.
 *
 * @param[in] obj_hdl     Object to link to (must be a regular file).
 * @param[in] destdir_hdl Destination directory.
 * @param[in] name        New entry name in the destination directory.
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_MLINK if link limit reached.
 *
 * TODO: implement via an omashu transaction that atomically:
 *   1. Checks nlink < POSIX_LINK_MAX.
 *   2. Creates the new directory entry.
 *   3. Increments nlink on the target object.
 */
fsal_status_t dotfs_link(struct fsal_obj_handle *obj_hdl,
			 struct fsal_obj_handle *destdir_hdl, const char *name)
{
	/* TODO: implement via omashu atomic transaction. */
	(void)obj_hdl;
	(void)destdir_hdl;
	(void)name;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_rename — rename / move a directory entry.
 *
 * Atomically moves @p old_name from @p olddir_hdl to @p new_name in
 * @p newdir_hdl.  If @p new_name exists it is replaced (POSIX rename
 * semantics).
 *
 * @param[in] obj_hdl    Handle of the object being renamed.
 * @param[in] olddir_hdl Source directory.
 * @param[in] old_name   Current entry name in the source directory.
 * @param[in] newdir_hdl Destination directory (may equal olddir_hdl).
 * @param[in] new_name   New entry name in the destination directory.
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: implement via an omashu transaction:
 *   1. Read old entry, validate it matches obj_hdl.
 *   2. If new_name exists: validate replace conditions (not a non-empty dir).
 *   3. Create new entry; delete old entry atomically.
 *   4. The object's UUID (handle identity) remains unchanged.
 */
fsal_status_t dotfs_rename(struct fsal_obj_handle *obj_hdl,
			   struct fsal_obj_handle *olddir_hdl,
			   const char *old_name,
			   struct fsal_obj_handle *newdir_hdl,
			   const char *new_name)
{
	/* TODO: implement via omashu transaction; UUID stays constant. */
	(void)obj_hdl;
	(void)olddir_hdl;
	(void)old_name;
	(void)newdir_hdl;
	(void)new_name;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_unlink — remove a directory entry.
 *
 * Decrements the link count of @p obj_hdl and removes the @p name entry
 * from @p dir_hdl.  When nlink reaches zero, schedules object data
 * deletion from the StorageEngine (data GC path).
 *
 * @param[in] dir_hdl  Parent directory.
 * @param[in] obj_hdl  Object to unlink.
 * @param[in] name     Entry name to remove from the parent directory.
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_NOTEMPTY if dir is non-empty.
 *
 * TODO: for files with nlink==0 after decrement, enqueue object ID for
 *       async deletion from StorageEngine shards + metadata GC.
 */
fsal_status_t dotfs_unlink(struct fsal_obj_handle *dir_hdl,
			   struct fsal_obj_handle *obj_hdl, const char *name)
{
	/* TODO:
	 *   - Decrement nlink in omashu.
	 *   - Delete the namespace entry.
	 *   - If nlink == 0: enqueue object UUID for StorageEngine GC.
	 *     (P0: atomic chunk commits / scavenger needed first.)
	 */
	(void)dir_hdl;
	(void)obj_hdl;
	(void)name;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_handle_to_wire — serialise a handle into an NFS wire filehandle.
 *
 * Ganesha calls this when it needs to send an opaque filehandle to an NFS
 * client (LOOKUP/CREATE reply, READDIR with attrs, etc.).
 * The @p output_type controls the encoding format; FSAL_DIGEST_NFSV4 is
 * the most common.
 *
 * @param[in]  obj_hdl     Object whose handle to serialise.
 * @param[in]  output_type Wire encoding type (FSAL_DIGEST_NFSV3 / _NFSV4).
 * @param[out] fh_desc     Buffer to write the serialised bytes into.
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_TOOSMALL if fh_desc is too short.
 *
 * TODO: validate fh_desc->len >= handle_len before copying.
 */
fsal_status_t dotfs_handle_to_wire(const struct fsal_obj_handle *obj_hdl,
				   fsal_digesttype_t output_type,
				   struct gsh_buffdesc *fh_desc)
{
	const struct dotfs_fsal_obj_handle *myself =
		container_of(obj_hdl, struct dotfs_fsal_obj_handle, obj_handle);

	if (fh_desc->len < myself->handle.handle_len)
		return fsalstat(ERR_FSAL_TOOSMALL, 0);

	/* TODO: support output_type variants if needed. */
	(void)output_type;

	memcpy(fh_desc->addr, myself->handle.handle_data,
	       myself->handle.handle_len);
	fh_desc->len = myself->handle.handle_len;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * dotfs_handle_to_key — return the handle's hash-key descriptor.
 *
 * Used by Ganesha's MDCACHE to key the object in its internal hash table.
 * The key must uniquely identify the object within this FSAL's namespace
 * and must not change for the lifetime of the object (even across renames).
 *
 * @param[in]  obj_hdl  The object handle.
 * @param[out] fh_desc  Descriptor pointing into the handle's key bytes.
 *
 * NOTE: fh_desc->addr points directly into the handle struct.  The handle
 * must remain live for as long as fh_desc is in use.
 */
void dotfs_handle_to_key(struct fsal_obj_handle *obj_hdl,
			 struct gsh_buffdesc *fh_desc)
{
	struct dotfs_fsal_obj_handle *myself = DOTFS_OBJ(obj_hdl);

	/* Use only the UUID prefix (first 36 bytes) as the key so that
	 * renames (which change the path component) do not invalidate
	 * the MDCACHE entry.
	 *
	 * TODO: define a fixed-size key layout:
	 *   handle_data[0..35]  = 36-char UUID (rename-stable identity)
	 * Key length = 36.
	 */
	fh_desc->addr = myself->handle.handle_data;
	fh_desc->len = myself->handle.handle_len;
}

/* =========================================================================
 * Handle ops table initialisation
 * ========================================================================= */

/**
 * dotfs_handle_ops_init — populate the fsal_obj_ops table for DOTFS.
 *
 * Called once from dotfs_init() at module load time.  Sets every function
 * pointer in @p ops; slots left as NULL will trigger assertion failures if
 * Ganesha ever calls them — which makes missing implementations visible
 * immediately rather than silently returning wrong results.
 *
 * @param[in,out] ops  The ops table to populate.
 */
void dotfs_handle_ops_init(struct fsal_obj_ops *ops)
{
	/* Start from Ganesha's default (mostly stubs that log + return NOTSUPP). */
	fsal_default_obj_ops_init(ops);

	/* Namespace ops */
	ops->release = dotfs_release_obj;
	ops->merge = dotfs_merge;
	ops->lookup = dotfs_lookup;
	ops->readdir = dotfs_readdir;
	ops->mkdir = dotfs_mkdir;
	ops->mknode = dotfs_mknode;
	ops->symlink = dotfs_symlink;
	ops->readlink = dotfs_readlink;
	ops->getattrs = dotfs_getattrs;
	ops->link = dotfs_link;
	ops->rename = dotfs_rename;
	ops->unlink = dotfs_unlink;

	/* Attribute ops */
	ops->setattr2 = dotfs_setattrs;

	/* File I/O ops (implemented in file.c) */
	ops->open2 = dotfs_open2;
	ops->status2 = dotfs_status2;
	ops->reopen2 = dotfs_reopen2;
	ops->read2 = dotfs_read2;
	ops->write2 = dotfs_write2;
	ops->commit2 = dotfs_commit2;
	ops->close2 = dotfs_close2;
	ops->lock_op2 = dotfs_lock_op2;
	ops->close = dotfs_close;

	/* Handle serialisation */
	ops->handle_to_wire = dotfs_handle_to_wire;
	ops->handle_to_key = dotfs_handle_to_key;

	/* State allocation (NFSv4 open/lock state) */
	ops->alloc_state = dotfs_alloc_state;
}

/**
 * dotfs_close — close the global (non-state) file descriptor on a handle.
 *
 * Called by Ganesha when the last reference to an open handle without an
 * associated NFSv4 state is dropped.  Delegates to dotfs_close_my_fd().
 *
 * @param[in] obj_hdl  Regular file handle whose fd to close.
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_NOT_OPENED if already closed.
 */
fsal_status_t dotfs_close(struct fsal_obj_handle *obj_hdl)
{
	struct dotfs_fsal_obj_handle *myself = DOTFS_OBJ(obj_hdl);

	if (obj_hdl->type != REGULAR_FILE)
		return fsalstat(ERR_FSAL_BADTYPE, 0);

	/* TODO: guard with the handle's content lock once added. */
	return dotfs_close_my_fd(&myself->u.file.fd);
}
