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

#include "FSAL/fsal_commonlib.h"

#include "dotfs.h"

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
	fsal_default_obj_ops_init(ops);

	// Namespace ops
	ops->release = dotfs_release_obj;	// TODO: Yet to implement

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

	// Attribute ops
	ops->setattr2 = dotfs_setattrs;

	// File I/O ops
	ops->open2 = dotfs_open2;
	ops->status2 = dotfs_status2;
	ops->reopen2 = dotfs_reopen2;
	ops->read2 = dotfs_read2;
	ops->write2 = dotfs_write2;
	ops->commit2 = dotfs_commit2;
	ops->close2 = dotfs_close2;
	ops->lock_op2 = dotfs_lock_op2;
	ops->close = dotfs_close;

	// Handle serialisation
	ops->handle_to_wire = dotfs_handle_to_wire;
	ops->handle_to_key = dotfs_handle_to_key;

	// State allocation (NFSv4 open/lock state)
	// ops->alloc_state = dotfs_alloc_state;
}

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
 */
fsal_status_t dotfs_merge(struct fsal_obj_handle *orig_hdl,
			  struct fsal_obj_handle *dupe_hdl)
{
	fsal_status_t status = { ERR_FSAL_NO_ERROR, 0 };

	// TODO: transfer open-state / share-reservation data from dupe to orig
	// once NFSv4 state is implemented.

	if (orig_hdl->type == REGULAR_FILE && dupe_hdl->type == REGULAR_FILE) {
		/* TODO: merge share reservations.
		 *   dotfs_fsal_obj_handle_t *orig = DOTFS_OBJ(orig_hdl);
		 *   dotfs_fsal_obj_handle_t *dupe = DOTFS_OBJ(dupe_hdl);
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
	 *   dotfs_fsal_obj_handle_t *parent_hdl = DOTFS_OBJ(parent);
	 *   struct dotfs_fsal_export *exp = DOTFS_EXPORT(op_ctx->ctx_export->fsal_export);
	 *   char child_path[PATH_MAX];
	 *   snprintf(child_path, sizeof(child_path), "%s/%s",
	 *            parent_path_from_handle(parent_hdl), name);
	 *   ObjectStorageMeta *meta = dotfs_meta_get(exp->dotfs_ctx, child_path);
	 *   if (!meta) return fsalstat(ERR_FSAL_NOENT, 0);
	 *   dotfs_fsal_obj_handle_t *hdl =
	 *       dotfs_alloc_handle(exp, build_fh(meta), meta->type, NULL, child_path);
	 *   *handle = &hdl->obj_handle;
	 */
	(void)parent;
    (void)name;

    if (handle != NULL) {
        *handle = NULL;
    }
    if (attrs_out != NULL) {
        memset(attrs_out, 0, sizeof(struct fsal_attrlist));
    }

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
	 *   dotfs_fsal_obj_handle_t *dir = DOTFS_OBJ(dir_hdl);
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

    if (eof != NULL) {
        *eof = true; // Tell Ganesha there's nothing here to read
    }
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
			  struct fsal_attrlist *attrs_out,
			  struct fsal_attrlist *parent_pre_attrs_out,
			  struct fsal_attrlist *parent_post_attrs_out)
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
	(void)attrs_out;

	if (new_obj != NULL) {
        *new_obj = NULL; 
    }
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
			   object_file_type_t nodetype, /* IN */
			   struct fsal_attrlist *attrib,
			   struct fsal_obj_handle **handle,
			   struct fsal_attrlist *attrs_out,
			   struct fsal_attrlist *parent_pre_attrs_out,
			   struct fsal_attrlist *parent_post_attrs_out)
{
	/* TODO: persist node metadata in omashu; populate unopenable union. */
	(void)dir_hdl;
	(void)name;
	(void)nodetype;
	// (void)dev;
	(void)attrib;
	// (void)new_obj;
	(void)attrs_out;

	if (handle != NULL) {
        *handle = NULL; 
    }

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
			    struct fsal_obj_handle **handle,
			    struct fsal_attrlist *attrs_out,
			    struct fsal_attrlist *parent_pre_attrs_out,
			    struct fsal_attrlist *parent_post_attrs_out)
{
	/* TODO: store symlink metadata in omashu; populate symlink union. */
	(void)dir_hdl;
	(void)name;
	(void)link_path;
	(void)attrib;
	// (void)new_obj;
	(void)attrs_out;

	if (handle != NULL) {
        *handle = NULL; 
    }

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
			     utf8string *link_content, bool refresh)
{
	/* TODO:
	 *   dotfs_fsal_obj_handle_t *myself = DOTFS_OBJ(obj_hdl);
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
	 *   dotfs_fsal_obj_handle_t *myself = DOTFS_OBJ(obj_hdl);
	 *   ObjectStorageMeta *meta = dotfs_meta_get_by_id(exp->dotfs_ctx,
	 *                                                  handle_to_id(&myself->handle));
	 *   if (!meta) return fsalstat(ERR_FSAL_STALE, 0);
	 *   populate attrs_out from meta fields.
	 */
	(void)obj_hdl;

    if (attrs_out != NULL) {
        memset(attrs_out, 0, sizeof(struct fsal_attrlist));
    }
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
			 struct fsal_obj_handle *destdir_hdl, const char *name,
			 struct fsal_attrlist *destdir_pre_attrs_out,
			 struct fsal_attrlist *destdir_post_attrs_out)
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
			   const char *new_name,
			   struct fsal_attrlist *olddir_pre_attrs_out,
			   struct fsal_attrlist *olddir_post_attrs_out,
			   struct fsal_attrlist *newdir_pre_attrs_out,
			   struct fsal_attrlist *newdir_post_attrs_out)
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
			   struct fsal_obj_handle *obj_hdl, const char *name,
			   struct fsal_attrlist *parent_pre_attrs_out,
			   struct fsal_attrlist *parent_post_attrs_out)
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
 * dotfs_open2 — open or create a file (NFSv4 OPEN / NFSv3 CREATE).
 *
 * The workhorse for all file open and create operations.  Handles:
 *   - Opening an existing file (FSAL_NO_CREATE)
 *   - Creating a new file (FSAL_UNCHECKED_CREATE, FSAL_GUARDED_CREATE,
 *     FSAL_EXCLUSIVE_CREATE)
 *   - Exclusive create using verifier (FSAL_EXCLUSIVE_CREATE)
 *
 * When @p state is non-NULL, the fd is stored in the state_fd and share
 * reservations are updated.  When @p state is NULL, the fd is stored
 * directly on the object handle (NFSv3 / anonymous open).
 *
 * @param[in]  obj_hdl               Directory (for create) or file to open.
 * @param[in]  state                 NFSv4 open state; NULL for NFSv3.
 * @param[in]  openflags             Requested open mode.
 * @param[in]  createmode            Create semantics.
 * @param[in]  name                  File name (only for create; NULL for open).
 * @param[in]  attrib_set            Attributes for new file (may be NULL).
 * @param[in]  verifier              Exclusive-create verifier.
 * @param[out] new_obj               New handle (only for create ops).
 * @param[out] attrs_out             Post-op attributes (may be NULL).
 * @param[out] caller_perm_check     Set true if the FSAL wants the caller
 *                                   to perform an access check.
 * @param[out] parent_pre_attrs_out  Pre-op dir attrs (may be NULL).
 * @param[out] parent_post_attrs_out Post-op dir attrs (may be NULL).
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: implement full create/open path:
 *   CREATE path:
 *     1. Compose new object path.
 *     2. Open omashu transaction.
 *     3. Honour createmode (GUARDED → ERR_FSAL_EXIST if present;
 *        EXCLUSIVE → write verifier as xattr for idempotency check).
 *     4. Allocate UUID; create ObjectStorageMeta in omashu.
 *     5. Commit transaction; open dotfs stream.
 *   OPEN path:
 *     1. Validate share reservations against existing opens.
 *     2. Open dotfs stream with requested flags.
 */
fsal_status_t dotfs_open2(struct fsal_obj_handle *obj_hdl,
			  struct state_t *state, fsal_openflags_t openflags,
			  enum fsal_create_mode createmode, const char *name,
			  struct fsal_attrlist *attrib_set,
			  fsal_verifier_t verifier,
			  struct fsal_obj_handle **new_obj,
			  struct fsal_attrlist *attrs_out,
			  bool *caller_perm_check,
			  struct fsal_attrlist *parent_pre_attrs_out,
			  struct fsal_attrlist *parent_post_attrs_out)
{
	/* TODO: implement create / open logic described above. */
	(void)obj_hdl;
	(void)state;
	(void)openflags;
	(void)createmode;
	(void)name;
	(void)attrib_set;
	(void)verifier;
	(void)new_obj;
	(void)attrs_out;
	(void)caller_perm_check;
	(void)parent_pre_attrs_out;
	(void)parent_post_attrs_out;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_status2 — return the current open flags for a stateful open.
 *
 * Called by Ganesha to determine whether an NFSv4 state's file descriptor
 * is already open in a compatible mode.
 *
 * @param[in] obj_hdl  Object handle.
 * @param[in] state    NFSv4 open state whose flags to query.
 *
 * @return Currently effective open flags, or FSAL_O_CLOSED.
 *
 * TODO: retrieve flags from the state_fd's dotfs_fd.fsal_fd.openflags.
 */
fsal_openflags_t dotfs_status2(struct fsal_obj_handle *obj_hdl,
			       struct state_t *state)
{
	/* TODO:
	 *   dotfs_state_fd_t *sfd =
	 *       container_of(state, dotfs_state_fd_t, state);
	 *   return sfd->dotfs_fd.fsal_fd.openflags;
	 */
	(void)obj_hdl;
	(void)state;
	return FSAL_O_CLOSED;
}

/**
 * dotfs_reopen2 — change open flags on an existing NFSv4 state fd.
 *
 * Called when an NFSv4 OPEN_UPGRADE or OPEN_DOWNGRADE changes the access
 * or deny mode on an existing open.  Must update the underlying stream
 * without disrupting inflight I/O.
 *
 * @param[in]     obj_hdl   Object handle.
 * @param[in,out] state     NFSv4 open state to upgrade.
 * @param[in]     openflags New desired open flags.
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: call dotfs_reopen_func on the state's fsal_fd.
 */
fsal_status_t dotfs_reopen2(struct fsal_obj_handle *obj_hdl,
			    struct state_t *state, fsal_openflags_t openflags)
{
	/* TODO:
	 *   dotfs_state_fd_t *sfd =
	 *       container_of(state, dotfs_state_fd_t, state);
	 *   return dotfs_reopen_func(obj_hdl, openflags, &sfd->dotfs_fd.fsal_fd);
	 */
	(void)obj_hdl;
	(void)state;
	(void)openflags;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_read2 — read data from a file (async callback model).
 *
 * Ganesha calls this with a pre-populated fsal_io_arg describing the I/O
 * request.  The operation may complete synchronously or asynchronously;
 * it MUST call @p done_cb exactly once when finished.
 *
 * For dotfs, reads are served by StorageEngine.ReadObjectByRange() which:
 *   1. Identifies chunks overlapping [offset, offset+size).
 *   2. Reads shards in parallel (auto-reconstructs from parity if needed).
 *   3. Writes assembled bytes into the provided io_vec.
 *
 * @param[in] obj_hdl    Object to read from.
 * @param[in] bypass     If true, bypass share-reservation checks.
 * @param[in] done_cb    Completion callback (MUST be called exactly once).
 * @param[in] read_arg   I/O request descriptor (offset, length, io_vec).
 * @param[in] caller_arg Opaque context passed through to done_cb.
 *
 * TODO: obtain the open fd (state or handle), call the dotfs C-binding
 *   dotfs_read_range(ctx, object_id, offset, len, iov, niov, &nread),
 *   populate read_arg->io_amount and read_arg->end_of_file, then call done_cb.
 */
void dotfs_read2(struct fsal_obj_handle *obj_hdl, bool bypass,
		 fsal_async_cb done_cb, struct fsal_io_arg *read_arg,
		 void *caller_arg)
{
	/* TODO:
	 *   1. Find the appropriate dotfs_fd (from state or obj handle).
	 *   2. Call dotfs ReadObjectByRange via C-binding.
	 *   3. On success: read_arg->io_amount = bytes_read;
	 *                  read_arg->end_of_file = (offset+bytes_read >= size);
	 *   4. done_cb(obj_hdl, caller_arg, read_arg, fsalstat(ERR_FSAL_NO_ERROR, 0));
	 */
	(void)obj_hdl;
	(void)bypass;
	(void)read_arg;
	done_cb(obj_hdl, fsalstat(ERR_FSAL_NOTSUPP, 0), read_arg, caller_arg);
}

/**
 * dotfs_write2 — write data to a file (async callback model).
 *
 * For dotfs, writes go through StorageEngine.EncodeObjectByChunks() which:
 *   1. Splits the incoming data into ≤4 MB chunks.
 *   2. Reed-Solomon encodes each chunk into data + parity shards.
 *   3. Dispatches shards in parallel to their assigned drives.
 *   4. Returns the chunk manifest for MetaDB persistence.
 *
 * NOTE: P0 TODO — writes must be staged to a .tmp/ path first; a scavenger
 * sweeps uncommitted shards.  Until atomic chunk commits are implemented,
 * data written here is NOT crash-safe.
 *
 * @param[in] obj_hdl    Object to write to.
 * @param[in] bypass     If true, bypass share-reservation checks.
 * @param[in] done_cb    Completion callback (MUST be called exactly once).
 * @param[in] write_arg  I/O request descriptor (offset, length, io_vec).
 * @param[in] caller_arg Opaque context passed through to done_cb.
 *
 * TODO: call dotfs EncodeObjectByChunks via C-binding, persist chunk
 *   manifest to omashu, update file size attribute, call done_cb.
 */
void dotfs_write2(struct fsal_obj_handle *obj_hdl, bool bypass,
		  fsal_async_cb done_cb, struct fsal_io_arg *write_arg,
		  void *caller_arg)
{
	/* TODO:
	 *   1. Find the appropriate dotfs_fd.
	 *   2. For each iovec entry: call dotfs_write_chunk() C-binding.
	 *      Note: P0 — atomic chunk commit / .tmp staging must be in place.
	 *   3. Update ObjectStorageMeta.Size if write extends the file.
	 *   4. Persist updated chunk manifest to omashu.
	 *   5. write_arg->io_amount = bytes_written;
	 *   6. done_cb(obj_hdl, caller_arg, write_arg, status);
	 */
	(void)obj_hdl;
	(void)bypass;
	(void)write_arg;
	done_cb(obj_hdl, fsalstat(ERR_FSAL_NOTSUPP, 0), write_arg, caller_arg);
}

/**
 * dotfs_commit2 — flush dirty data and metadata to stable storage.
 *
 * Called by Ganesha in response to an NFS COMMIT RPC or on NFSv4 CLOSE
 * with dirty data.  Must ensure all data in [offset, offset+len) is
 * durably written to the underlying drives and the chunk manifest is
 * persisted to omashu.
 *
 * @param[in] obj_hdl  File to commit.
 * @param[in] offset   Start of the range to commit.
 * @param[in] len      Byte length of the range; 0 means "commit everything".
 *
 * @return FSAL_NO_ERROR when all dirty data is durable.
 *
 * TODO: call the dotfs C-binding flush/sync API, then ensure omashu has
 *   committed the latest chunk manifest entry.
 */
fsal_status_t dotfs_commit2(struct fsal_obj_handle *obj_hdl, off_t offset,
			    size_t len)
{
	/* TODO:
	 *   1. Call dotfs_sync(stream, offset, len) to fsync drive shards.
	 *   2. Confirm omashu has persisted the chunk manifest (may be no-op
	 *      if writes already went through the Raft log synchronously).
	 */
	(void)obj_hdl;
	(void)offset;
	(void)len;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_close2 — close a stateful (NFSv4) open.
 *
 * Called when an NFSv4 CLOSE is processed.  Must:
 *   1. Flush any dirty data (if WRITE was the open mode).
 *   2. Close the underlying dotfs stream.
 *   3. Release the share reservation on the object handle.
 *
 * After this returns, the state_fd's dotfs_fd should be in the closed
 * state so that Ganesha can safely free the state.
 *
 * @param[in] obj_hdl  File being closed.
 * @param[in] state    The NFSv4 open state to close.
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: call dotfs_close_my_fd on the state_fd's fd; update share reservations.
 */
fsal_status_t dotfs_close2(struct fsal_obj_handle *obj_hdl,
			   struct state_t *state)
{
	/* TODO:
	 *   dotfs_state_fd_t *sfd =
	 *       container_of(state, dotfs_state_fd_t, state);
	 *   fsal_status_t status = dotfs_close_my_fd(&sfd->dotfs_fd);
	 *   update_share_reservation(&DOTFS_OBJ(obj_hdl)->u.file.share, state);
	 *   return status;
	 */
	(void)obj_hdl;
	(void)state;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/**
 * dotfs_lock_op2 — apply or test a byte-range lock.
 *
 * Implements advisory POSIX byte-range locking as required by NFSv4.
 * Ganesha's SAL handles the lock conflict matrix; the FSAL must forward
 * the request to the underlying lock mechanism.
 *
 * For dotfs (distributed filesystem), locks must be coordinated through
 * the omashu metadata store to be visible across all nodes.
 *
 * @param[in]  obj_hdl          File to lock.
 * @param[in]  state            NFSv4 open state (lock owner context).
 * @param[in]  owner            Lock owner opaque identifier.
 * @param[in]  lock_op          FSAL_OP_LOCK, FSAL_OP_UNLOCK, FSAL_OP_LOCKT.
 * @param[in]  request_lock     Requested lock parameters (type, range).
 * @param[out] conflicting_lock On FSAL_OP_LOCKT conflict: populated with the
 *                              conflicting lock details.
 *
 * @return FSAL_NO_ERROR, ERR_FSAL_LOCKED (conflict on LOCKT), or error.
 *
 * TODO: implement distributed lock coordination via omashu transactions.
 *       P1 item: initially return NOTSUPP to indicate no server-side locking
 *       (Ganesha will fall back to client-side advisory locking).
 */
fsal_status_t dotfs_lock_op2(struct fsal_obj_handle *obj_hdl,
			     struct state_t *state, void *owner,
			     fsal_lock_op_t lock_op,
			     fsal_lock_param_t *request_lock,
			     fsal_lock_param_t *conflicting_lock)
{
	/* TODO: implement distributed byte-range locking via omashu. */
	(void)obj_hdl;
	(void)state;
	(void)owner;
	(void)lock_op;
	(void)request_lock;
	(void)conflicting_lock;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
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
	dotfs_fsal_obj_handle_t *myself = DOTFS_OBJ(obj_hdl);

	if (obj_hdl->type != REGULAR_FILE)
		return fsalstat(ERR_FSAL_BADTYPE, 0);

	/* TODO: guard with the handle's content lock once added. */
	return dotfs_close_my_fd(&myself->u.file.fd);
}

/**
 * dotfs_close_my_fd — close a dotfs stream fd.
 *
 * Calls the dotfs C-binding to flush and release the stream, then resets
 * @p my_fd back to the closed state.
 *
 * @param[in,out] my_fd  The fd to close.
 *
 * @return FSAL_NO_ERROR on success, ERR_FSAL_NOT_OPENED if already closed.
 *
 * TODO: call dotfs_close(my_fd->dotfs_stream) and handle errors.
 */
fsal_status_t dotfs_close_my_fd(dotfs_fd_t *my_fd)
{
	if (my_fd->dotfs_stream == NULL ||
	    my_fd->fsal_fd.openflags == FSAL_O_CLOSED)
		return fsalstat(ERR_FSAL_NOT_OPENED, 0);

	LogFullDebug(COMPONENT_FSAL,
		     "DOTFS close_my_fd: stream=%p openflags=0x%x",
		     my_fd->dotfs_stream, my_fd->fsal_fd.openflags);

	/* TODO: dotfs_close(my_fd->dotfs_stream); */
	my_fd->dotfs_stream = NULL;
	my_fd->fsal_fd.openflags = FSAL_O_CLOSED;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
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
	const dotfs_fsal_obj_handle_t *myself =
		container_of(obj_hdl, dotfs_fsal_obj_handle_t, obj_handle);

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
	dotfs_fsal_obj_handle_t *myself = DOTFS_OBJ(obj_hdl);

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

// /**
//  * dotfs_alloc_state — allocate per-open NFSv4 state.
//  *
//  * Ganesha calls this when a new NFSv4 OPEN or LOCK state needs to be
//  * created.  Allocates a dotfs_state_fd whose first member is a state_t
//  * so that Ganesha's default free_state() can release it without knowing
//  * the FSAL subtype.
//  *
//  * @param[in] exp_hdl       Export handle.
//  * @param[in] state_type    Type of state (OPEN, LOCK, DELEGATION, …).
//  * @param[in] related_state Related state for lock/delegation allocation.
//  *
//  * @return Pointer to the generic state_t base; NULL on allocation failure.
//  */
// struct state_t *dotfs_alloc_state(struct fsal_export *exp_hdl,
// 				  enum state_type state_type,
// 				  struct state_t *related_state)
// {
// 	dotfs_state_fd_t *state_fd;

// 	state_fd = gsh_calloc(1, sizeof(*state_fd));

// 	/* Initialise the dotfs_fd portion. */
// 	state_fd->dotfs_fd.fsal_fd.openflags = FSAL_O_CLOSED;
// 	state_fd->dotfs_fd.dotfs_stream = NULL;

// 	/* Let Ganesha initialise the state_t base. */
// 	return init_state(&state_fd->state, exp_hdl, state_type, related_state);
// }
