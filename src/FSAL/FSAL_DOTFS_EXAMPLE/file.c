// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * file.c
 * FSAL_DOTFS — file I/O operations.
 *
 * Implements all fsal_obj_ops related to opening, reading, writing, and
 * closing regular files, plus NFSv4 stateful open/lock state management.
 *
 * I/O model
 * ---------
 * dotfs stores file data as erasure-coded shards across physical drives
 * via its StorageEngine.  The logical view exposed to NFS clients is a
 * flat byte-stream keyed by ObjectStorageMeta.ObjectId.
 *
 * An open "file descriptor" in this FSAL is a dotfs_fd, which holds an
 * opaque dotfs_stream pointer obtained from the dotfs C-binding layer.
 * The stream is opened lazily on first access and reference-counted by
 * Ganesha's fsal_fd / fsal_share infrastructure.
 *
 * NFSv4 state
 * -----------
 * Each NFSv4 OPEN creates a dotfs_state_fd (state_t + dotfs_fd).
 * Multiple concurrent opens on the same file are tracked via Ganesha's
 * share-reservation mechanism (fsal_share).  Byte-range locks are
 * advisory and enforced by Ganesha's SAL; the FSAL only needs to call
 * through to the underlying locking layer.
 *
 * Concurrency
 * -----------
 * The obj_handle content lock (MDCACHE / fsal_obj_handle) serialises
 * open/close on the per-handle fd.  State-fd operations are serialised by
 * the per-state lock held by Ganesha before calling into these ops.
 * No additional FSAL-level locking is required unless noted.
 */

#include "config.h"

#include <assert.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/access_check.h"
#include "sal_data.h"
#include "sal_functions.h"

#include "dotfs_methods.h"

/* =========================================================================
 * Internal fd helpers
 * ========================================================================= */

/**
 * dotfs_open_my_fd — open a dotfs stream and record it in @p my_fd.
 *
 * Translates FSAL open flags to dotfs open flags, calls the dotfs
 * C-binding to obtain a stream context, and populates @p my_fd.
 *
 * Pre-conditions (asserted):
 *   - my_fd->dotfs_stream == NULL  (not already open)
 *   - openflags != 0
 *
 * @param[in]  myself    Object handle owning the fd.
 * @param[in]  openflags FSAL open-mode flags (FSAL_O_READ, FSAL_O_WRITE, …).
 * @param[out] my_fd     Populated with the opened stream on success.
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: call the dotfs C-binding:
 *   dotfs_stream_t *s;
 *   int rc = dotfs_open(exp->dotfs_ctx, object_id, dotfs_flags, &s);
 *   if (rc) return fsalstat(posix2fsal_error(-rc), -rc);
 *   my_fd->dotfs_stream = s;
 *   my_fd->fsal_fd.openflags = FSAL_O_NFS_FLAGS(openflags);
 */
fsal_status_t dotfs_open_my_fd(struct dotfs_fsal_obj_handle *myself,
			       fsal_openflags_t openflags,
			       struct dotfs_fd *my_fd)
{
	assert(my_fd->dotfs_stream == NULL);
	assert(my_fd->fsal_fd.openflags == FSAL_O_CLOSED);
	assert(openflags != 0);

	LogFullDebug(COMPONENT_FSAL,
		     "DOTFS open_my_fd: obj=%p openflags=0x%x",
		     myself, openflags);

	/* TODO: translate openflags → dotfs open flags and call dotfs_open(). */
	(void)myself;
	(void)openflags;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
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
fsal_status_t dotfs_close_my_fd(struct dotfs_fd *my_fd)
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

/* =========================================================================
 * Ganesha multi-fd callbacks (used internally by the fd management layer)
 * ========================================================================= */

/**
 * dotfs_reopen_func — reopen or upgrade an existing fd to new flags.
 *
 * Called by Ganesha's fd management layer when the current open flags on
 * @p fsal_fd are insufficient for a new operation (e.g. a read-only fd
 * needs to be upgraded to read-write).
 *
 * @param[in]     obj_hdl   Object handle that owns the fd.
 * @param[in]     openflags New desired open flags.
 * @param[in,out] fsal_fd   The fd to upgrade.
 *
 * @return FSAL_NO_ERROR on success.
 *
 * TODO: close the existing stream and re-open with the combined flags,
 *       or use the dotfs reopen API if available (avoids read-cache flush).
 */
fsal_status_t dotfs_reopen_func(struct fsal_obj_handle *obj_hdl,
				fsal_openflags_t openflags,
				struct fsal_fd *fsal_fd)
{
	struct dotfs_fsal_obj_handle *myself = DOTFS_OBJ(obj_hdl);
	struct dotfs_fd *my_fd =
		container_of(fsal_fd, struct dotfs_fd, fsal_fd);
	fsal_status_t status;

	/* Close existing stream before reopening with new flags. */
	status = dotfs_close_my_fd(my_fd);
	if (FSAL_IS_ERROR(status) &&
	    status.major != ERR_FSAL_NOT_OPENED)
		return status;

	return dotfs_open_my_fd(myself, openflags, my_fd);
}

/**
 * dotfs_close_func — generic close callback for the fsal_fd layer.
 *
 * Adapts the fsal_fd-typed signature to dotfs_close_my_fd().
 *
 * @param[in] obj_hdl  Object handle (unused; fd is self-contained).
 * @param[in] fd       The fsal_fd to close.
 *
 * @return FSAL_NO_ERROR on success.
 */
fsal_status_t dotfs_close_func(struct fsal_obj_handle *obj_hdl,
			       struct fsal_fd *fd)
{
	struct dotfs_fd *my_fd = container_of(fd, struct dotfs_fd, fsal_fd);

	(void)obj_hdl;
	return dotfs_close_my_fd(my_fd);
}

/* =========================================================================
 * NFSv4 state allocation
 * ========================================================================= */

/**
 * dotfs_alloc_state — allocate per-open NFSv4 state.
 *
 * Ganesha calls this when a new NFSv4 OPEN or LOCK state needs to be
 * created.  Allocates a dotfs_state_fd whose first member is a state_t
 * so that Ganesha's default free_state() can release it without knowing
 * the FSAL subtype.
 *
 * @param[in] exp_hdl       Export handle.
 * @param[in] state_type    Type of state (OPEN, LOCK, DELEGATION, …).
 * @param[in] related_state Related state for lock/delegation allocation.
 *
 * @return Pointer to the generic state_t base; NULL on allocation failure.
 */
struct state_t *dotfs_alloc_state(struct fsal_export *exp_hdl,
				  enum state_type state_type,
				  struct state_t *related_state)
{
	struct dotfs_state_fd *state_fd;

	state_fd = gsh_calloc(1, sizeof(*state_fd));

	/* Initialise the dotfs_fd portion. */
	state_fd->dotfs_fd.fsal_fd.openflags = FSAL_O_CLOSED;
	state_fd->dotfs_fd.dotfs_stream = NULL;

	/* Let Ganesha initialise the state_t base. */
	return init_state(&state_fd->state, exp_hdl, state_type, related_state);
}

/* =========================================================================
 * fsal_obj_ops file I/O callbacks
 * ========================================================================= */

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
	 *   struct dotfs_state_fd *sfd =
	 *       container_of(state, struct dotfs_state_fd, state);
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
	 *   struct dotfs_state_fd *sfd =
	 *       container_of(state, struct dotfs_state_fd, state);
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
	 *   struct dotfs_state_fd *sfd =
	 *       container_of(state, struct dotfs_state_fd, state);
	 *   fsal_status_t status = dotfs_close_my_fd(&sfd->dotfs_fd);
	 *   update_share_reservation(&DOTFS_OBJ(obj_hdl)->u.file.share, state);
	 *   return status;
	 */
	(void)obj_hdl;
	(void)state;
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}
