/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * dotfs_methods.h
 * Internal types, structs, and function prototypes for FSAL_DOTFS.
 *
 * FSAL_DOTFS bridges NFS-Ganesha to the dotfs virtual filesystem — a
 * distributed, erasure-coded object store exposed as a POSIX namespace.
 * Metadata is persisted in an omashu (BadgerDB + Raft) cluster; file data
 * is erasure-coded across a set of physical drives via the dotfs
 * StorageEngine.
 *
 * Structural pattern mirrors FSAL_VFS:
 *   dotfs_fsal_module   — one per process (singleton)
 *   dotfs_fsal_export   — one per exported path / NFS export entry
 *   dotfs_fsal_obj_handle — one per live filesystem object (file, dir, …)
 *   dotfs_fd            — per-open-state file descriptor abstraction
 */

#ifndef DOTFS_METHODS_H
#define DOTFS_METHODS_H

#include "fsal_api.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/access_check.h"

/* ---------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */

struct dotfs_fsal_obj_handle;
struct dotfs_fsal_export;

/* ---------------------------------------------------------------------------
 * Module-level private storage
 *
 * Embeds fsal_module so that container_of() can cast back and forth between
 * the Ganesha generic type and our private extension.
 * ------------------------------------------------------------------------- */

struct dotfs_fsal_module {
	/** Ganesha generic module — MUST be first member. */
	struct fsal_module module;

	/** Pre-initialised handle ops shared across all exports in this
	 *  module instance.  Populated once in dotfs_handle_ops_init(). */
	struct fsal_obj_ops handle_ops;
};

/* ---------------------------------------------------------------------------
 * Export-level private storage
 *
 * One instance exists for each NFS export that targets a dotfs path.
 * ------------------------------------------------------------------------- */

struct dotfs_fsal_export {
	/** Ganesha generic export — MUST be first member. */
	struct fsal_export export;

	/**
	 * Root path inside the dotfs namespace that this export exposes.
	 * Allocated on export creation; freed in dotfs_release_export().
	 */
	char *root_path;

	/**
	 * Opaque handle to the dotfs VFS context (Go-layer handle).
	 * Lifetime: created in dotfs_create_export(), closed in
	 * dotfs_release_export().
	 *
	 * TODO: replace void* with the real CGo / FFI type once the
	 *       dotfs C-binding header is available.
	 */
	void *dotfs_ctx;
};

/** Convenience cast: fsal_export → dotfs_fsal_export */
#define DOTFS_EXPORT(fsal_exp) \
	container_of((fsal_exp), struct dotfs_fsal_export, export)

/* ---------------------------------------------------------------------------
 * Per-file-descriptor state
 *
 * Wraps a dotfs-level file handle / stream context.  One instance lives
 * inside the per-open-state (dotfs_state_fd) or as a temporary fd on the
 * object handle (dotfs_fsal_obj_handle.u.file.fd).
 * ------------------------------------------------------------------------- */

struct dotfs_fd {
	/** Ganesha fd management base (openflags, fd_type, refcount). */
	struct fsal_fd fsal_fd;

	/**
	 * Active dotfs stream context returned by the dotfs open call.
	 * NULL when the fd is closed (fsal_fd.openflags == FSAL_O_CLOSED).
	 *
	 * TODO: replace void* with the real dotfs stream type once the
	 *       C-binding layer is finalised.
	 */
	void *dotfs_stream;
};

/* ---------------------------------------------------------------------------
 * Per-state open file descriptor (NFSv4 stateful open)
 *
 * The first member MUST be struct state_t so Ganesha's default
 * free_state() can free this allocation without knowing the subtype.
 * ------------------------------------------------------------------------- */

struct dotfs_state_fd {
	/** NFSv4 open-state — MUST be first member. */
	struct state_t state;
	/** The underlying fd for this state. */
	struct dotfs_fd dotfs_fd;
};

/* ---------------------------------------------------------------------------
 * Object handle
 *
 * One instance per live filesystem object (regular file, directory,
 * symlink, …).  The handle opaque bytes stored in dotfs_obj_handle are
 * the serialised (namespace-path + object-id) tuple used to re-locate
 * the object across server restarts.
 * ------------------------------------------------------------------------- */

/**
 * Maximum byte length of a serialised dotfs object handle.
 * Covers a 36-byte UUID string + '/' + up to 1023-char path + NUL.
 */
#define DOTFS_HANDLE_MAX_LEN 1088

/** On-wire / in-memory object handle payload. */
struct dotfs_obj_handle {
	/** Serialised handle bytes (object_id + ':' + logical_path). */
	uint8_t handle_data[DOTFS_HANDLE_MAX_LEN];
	/** Actual length of valid bytes in handle_data. */
	uint16_t handle_len;
};

struct dotfs_fsal_obj_handle {
	/** Ganesha generic object handle — MUST be first member. */
	struct fsal_obj_handle obj_handle;

	/** Serialised dotfs object handle used as a stable NFS FH payload. */
	struct dotfs_obj_handle handle;

	/** Upcall vector for cache invalidation / layout recalls. */
	const struct fsal_up_vector *up_ops;

	/** Per-type union of additional state. */
	union {
		struct {
			/** Share reservation state (NFSv4 share modes). */
			struct fsal_share share;
			/** Current open fd; fd == NULL when not open. */
			struct dotfs_fd fd;
		} file;

		struct {
			/** Resolved symlink target (heap-allocated, NUL-term). */
			char *link_content;
			/** Byte length of link_content (excluding NUL). */
			size_t link_length;
		} symlink;

		struct {
			/**
			 * Path of parent directory — needed so that
			 * AF_UNIX / block / char device nodes can be re-found
			 * without a kernel file handle.
			 */
			char *dir;
			/** Entry name inside the parent directory. */
			char *name;
		} unopenable;
	} u;
};

/** Convenience cast: fsal_obj_handle → dotfs_fsal_obj_handle */
#define DOTFS_OBJ(fsal_hdl) \
	container_of((fsal_hdl), struct dotfs_fsal_obj_handle, obj_handle)

/* ---------------------------------------------------------------------------
 * Helper: test whether a file type cannot be opened by handle
 * (AF_UNIX sockets, block devices, character devices)
 * ------------------------------------------------------------------------- */
static inline bool dotfs_unopenable_type(object_file_type_t type)
{
	return type == SOCKET_FILE || type == CHARACTER_FILE ||
	       type == BLOCK_FILE;
}

/* ---------------------------------------------------------------------------
 * Module / export lifecycle  (export.c)
 * ------------------------------------------------------------------------- */

/**
 * @brief Create a new dotfs export instance.
 *
 * Called by Ganesha when an NFS export entry with FSAL { name = DOTFS; }
 * is activated.  Allocates a dotfs_fsal_export, opens a dotfs VFS context
 * for the target path, and registers the export with Ganesha.
 */
fsal_status_t dotfs_create_export(struct fsal_module *fsal_hdl,
				  void *parse_node,
				  struct config_error_type *err_type,
				  const struct fsal_up_vector *up_ops);

/**
 * @brief Hot-update an existing dotfs export.
 *
 * Called on SIGHUP / config reload.  Must apply changed parameters
 * (e.g. ACL policy) without disrupting active client sessions.
 */
fsal_status_t dotfs_update_export(struct fsal_module *fsal_hdl,
				  void *parse_node,
				  struct config_error_type *err_type,
				  struct fsal_export *original,
				  struct fsal_module *updated_super);

/**
 * @brief Initialise the handle ops table.
 *
 * Populates @p ops with all dotfs-specific fsal_obj_ops function pointers.
 * Called once from dotfs_init().
 */
void dotfs_handle_ops_init(struct fsal_obj_ops *ops);

/* ---------------------------------------------------------------------------
 * Export ops  (export.c, declared for ops-table assignment)
 * ------------------------------------------------------------------------- */

void dotfs_release_export(struct fsal_export *exp_hdl);

fsal_status_t dotfs_get_dynamic_info(struct fsal_export *exp_hdl,
				     struct fsal_obj_handle *obj_hdl,
				     fsal_dynamicfsinfo_t *infop);

fsal_status_t dotfs_lookup_path(struct fsal_export *exp_hdl, const char *path,
				struct fsal_obj_handle **handle,
				struct fsal_attrlist *attrs_out);

fsal_status_t dotfs_create_handle(struct fsal_export *exp_hdl,
				  struct gsh_buffdesc *hdl_desc,
				  struct fsal_obj_handle **handle,
				  struct fsal_attrlist *attrs_out);

fsal_status_t dotfs_get_quota(struct fsal_export *exp_hdl,
			      const char *filepath, int quota_type,
			      fsal_quota_t *pquota);

fsal_status_t dotfs_set_quota(struct fsal_export *exp_hdl,
			      const char *filepath, int quota_type,
			      fsal_quota_t *pquota, fsal_quota_t *presquota);

/* ---------------------------------------------------------------------------
 * Object handle ops  (handle.c)
 * ------------------------------------------------------------------------- */

fsal_status_t dotfs_lookup(struct fsal_obj_handle *parent,
			   const char *name,
			   struct fsal_obj_handle **handle,
			   struct fsal_attrlist *attrs_out);

fsal_status_t dotfs_readdir(struct fsal_obj_handle *dir_hdl,
			    fsal_cookie_t *whence, void *dir_state,
			    fsal_readdir_cb cb, attrmask_t attrmask,
			    bool *eof);

fsal_status_t dotfs_getattrs(struct fsal_obj_handle *obj_hdl,
			     struct fsal_attrlist *attrs_out);

fsal_status_t dotfs_setattrs(struct fsal_obj_handle *obj_hdl, bool bypass,
			     struct state_t *state,
			     struct fsal_attrlist *attrib_set);

fsal_status_t dotfs_mkdir(struct fsal_obj_handle *dir_hdl, const char *name,
			  struct fsal_attrlist *attrib,
			  struct fsal_obj_handle **new_obj,
			  struct fsal_attrlist *attrs_out);

fsal_status_t dotfs_mknode(struct fsal_obj_handle *dir_hdl, const char *name,
			   object_file_type_t nodetype, fsal_dev_t *dev,
			   struct fsal_attrlist *attrib,
			   struct fsal_obj_handle **new_obj,
			   struct fsal_attrlist *attrs_out);

fsal_status_t dotfs_symlink(struct fsal_obj_handle *dir_hdl, const char *name,
			    const char *link_path,
			    struct fsal_attrlist *attrib,
			    struct fsal_obj_handle **new_obj,
			    struct fsal_attrlist *attrs_out);

fsal_status_t dotfs_readlink(struct fsal_obj_handle *obj_hdl,
			     struct gsh_buffdesc *link_content,
			     bool refresh);

fsal_status_t dotfs_link(struct fsal_obj_handle *obj_hdl,
			 struct fsal_obj_handle *destdir_hdl,
			 const char *name);

fsal_status_t dotfs_rename(struct fsal_obj_handle *obj_hdl,
			   struct fsal_obj_handle *olddir_hdl,
			   const char *old_name,
			   struct fsal_obj_handle *newdir_hdl,
			   const char *new_name);

fsal_status_t dotfs_unlink(struct fsal_obj_handle *dir_hdl,
			   struct fsal_obj_handle *obj_hdl,
			   const char *name);

fsal_status_t dotfs_handle_to_wire(const struct fsal_obj_handle *obj_hdl,
				   fsal_digesttype_t output_type,
				   struct gsh_buffdesc *fh_desc);

void dotfs_handle_to_key(struct fsal_obj_handle *obj_hdl,
			 struct gsh_buffdesc *fh_desc);

void dotfs_release_obj(struct fsal_obj_handle *obj_hdl);

fsal_status_t dotfs_merge(struct fsal_obj_handle *orig_hdl,
			  struct fsal_obj_handle *dupe_hdl);

fsal_status_t dotfs_close(struct fsal_obj_handle *obj_hdl);

/* Internal helpers (handle.c) */
struct dotfs_fsal_obj_handle *
dotfs_alloc_handle(struct dotfs_fsal_export *exp,
		   const struct dotfs_obj_handle *fh, object_file_type_t type,
		   const struct stat *sb, const char *path);

void dotfs_free_obj_handle(struct dotfs_fsal_obj_handle **hdl_ref);

/* ---------------------------------------------------------------------------
 * File I/O ops  (file.c)
 * ------------------------------------------------------------------------- */

fsal_status_t dotfs_open2(struct fsal_obj_handle *obj_hdl,
			  struct state_t *state, fsal_openflags_t openflags,
			  enum fsal_create_mode createmode, const char *name,
			  struct fsal_attrlist *attrib_set,
			  fsal_verifier_t verifier,
			  struct fsal_obj_handle **new_obj,
			  struct fsal_attrlist *attrs_out,
			  bool *caller_perm_check,
			  struct fsal_attrlist *parent_pre_attrs_out,
			  struct fsal_attrlist *parent_post_attrs_out);

fsal_openflags_t dotfs_status2(struct fsal_obj_handle *obj_hdl,
			       struct state_t *state);

fsal_status_t dotfs_reopen2(struct fsal_obj_handle *obj_hdl,
			    struct state_t *state, fsal_openflags_t openflags);

void dotfs_read2(struct fsal_obj_handle *obj_hdl, bool bypass,
		 fsal_async_cb done_cb, struct fsal_io_arg *read_arg,
		 void *caller_arg);

void dotfs_write2(struct fsal_obj_handle *obj_hdl, bool bypass,
		  fsal_async_cb done_cb, struct fsal_io_arg *write_arg,
		  void *caller_arg);

fsal_status_t dotfs_commit2(struct fsal_obj_handle *obj_hdl, off_t offset,
			    size_t len);

fsal_status_t dotfs_close2(struct fsal_obj_handle *obj_hdl,
			   struct state_t *state);

fsal_status_t dotfs_lock_op2(struct fsal_obj_handle *obj_hdl,
			     struct state_t *state, void *owner,
			     fsal_lock_op_t lock_op,
			     fsal_lock_param_t *request_lock,
			     fsal_lock_param_t *conflicting_lock);

fsal_status_t dotfs_reopen_func(struct fsal_obj_handle *obj_hdl,
				fsal_openflags_t openflags,
				struct fsal_fd *fsal_fd);

fsal_status_t dotfs_close_func(struct fsal_obj_handle *obj_hdl,
			       struct fsal_fd *fd);

struct state_t *dotfs_alloc_state(struct fsal_export *exp_hdl,
				  enum state_type state_type,
				  struct state_t *related_state);

/* Internal fd helpers (file.c) */
fsal_status_t dotfs_open_my_fd(struct dotfs_fsal_obj_handle *myself,
			       fsal_openflags_t openflags,
			       struct dotfs_fd *my_fd);

fsal_status_t dotfs_close_my_fd(struct dotfs_fd *my_fd);

#endif /* DOTFS_METHODS_H */
