/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 *   Author(s): Sachin S <sarodhesachin96@gmail.com>
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published
 *   by the Free Software Foundation; either version 2.1 of the License, or
 *   (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * This file includes declarations of data types, functions, variables, constants and macros
 * used in the dotfs implementation of FSAL.
 */

#ifndef DOTFS_H
#define DOTFS_H

#include "fsal_api.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/access_check.h"

/**
 * Maximum byte length of a serialised dotfs object handle.
 * Covers a 36-byte UUID string + '/' + up to 1023-char path + NUL.
 */
#define DOTFS_HANDLE_MAX_LEN 1088

/* ---------------------------------------------------------------------------
 * Module-level private storage
 *
 * Embeds fsal_module so that container_of() can cast back and forth between
 * the Ganesha generic type and our private extension.
 * ------------------------------------------------------------------------- */
typedef struct {
	struct fsal_module module;
	struct fsal_obj_ops handle_ops;
} dotfs_fsal_module_t;

/* ---------------------------------------------------------------------------
 * Per-file-descriptor state
 *
 * Wraps a dotfs-level file handle / stream context.  One instance lives
 * inside the per-open-state (dotfs_state_fd) or as a temporary fd on the
 * object handle (dotfs_fsal_obj_handle.u.file.fd).
 * ------------------------------------------------------------------------- */

typedef struct {
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
} dotfs_fd_t;

/* ---------------------------------------------------------------------------
 * Per-state open file descriptor (NFSv4 stateful open)
 *
 * The first member MUST be struct state_t so Ganesha's default
 * free_state() can free this allocation without knowing the subtype.
 * ------------------------------------------------------------------------- */

typedef struct {
	/** NFSv4 open-state — MUST be first member. */
	struct state_t state;
	/** The underlying fd for this state. */
	dotfs_fd_t dotfs_fd;
} dotfs_state_fd_t;

/** On-wire / in-memory object handle payload. */
typedef struct {
	/** Serialised handle bytes (object_id + ':' + logical_path). */
	uint8_t handle_data[DOTFS_HANDLE_MAX_LEN];
	/** Actual length of valid bytes in handle_data. */
	uint16_t handle_len;
} dotfs_obj_handle_t;

typedef struct {
	/** Ganesha generic object handle — MUST be first member. */
	struct fsal_obj_handle obj_handle;

	/** Serialised dotfs object handle used as a stable NFS FH payload. */
	dotfs_obj_handle_t handle;

	/** Upcall vector for cache invalidation / layout recalls. */
	const struct fsal_up_vector *up_ops;

	/** Per-type union of additional state. */
	union {
		struct {
			/** Share reservation state (NFSv4 share modes). */
			struct fsal_share share;
			/** Current open fd; fd == NULL when not open. */
			dotfs_fd_t fd;
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
} dotfs_fsal_obj_handle_t;

/** Convenience cast: fsal_obj_handle → dotfs_fsal_obj_handle */
#define DOTFS_OBJ(fsal_hdl) \
	container_of((fsal_hdl), dotfs_fsal_obj_handle_t, obj_handle)

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

/* ---------------------------------------------------------------------------
 * Object handle ops  (handle.c)
 * ------------------------------------------------------------------------- */

/**
 * @brief Initialise the handle ops table.
 *
 * Populates @p ops with all dotfs-specific fsal_obj_ops function pointers.
 * Called once from dotfs_init().
 */
void dotfs_handle_ops_init(struct fsal_obj_ops *ops);
void dotfs_release_obj(struct fsal_obj_handle *obj_hdl);
fsal_status_t dotfs_merge(struct fsal_obj_handle *orig_hdl,
			  struct fsal_obj_handle *dupe_hdl);
fsal_status_t dotfs_lookup(struct fsal_obj_handle *parent, const char *name,
			   struct fsal_obj_handle **handle,
			   struct fsal_attrlist *attrs_out);
fsal_status_t dotfs_readdir(struct fsal_obj_handle *dir_hdl,
			    fsal_cookie_t *whence, void *dir_state,
			    fsal_readdir_cb cb, attrmask_t attrmask, bool *eof);
fsal_status_t dotfs_mkdir(struct fsal_obj_handle *dir_hdl, const char *name,
			  struct fsal_attrlist *attrib,
			  struct fsal_obj_handle **new_obj,
			  struct fsal_attrlist *attrs_out,
			  struct fsal_attrlist *parent_pre_attrs_out,
			  struct fsal_attrlist *parent_post_attrs_out);
fsal_status_t dotfs_mknode(struct fsal_obj_handle *dir_hdl, const char *name,
			   object_file_type_t nodetype, /* IN */
			   struct fsal_attrlist *attrib,
			   struct fsal_obj_handle **handle,
			   struct fsal_attrlist *attrs_out,
			   struct fsal_attrlist *parent_pre_attrs_out,
			   struct fsal_attrlist *parent_post_attrs_out);
fsal_status_t dotfs_symlink(struct fsal_obj_handle *dir_hdl, const char *name,
			    const char *link_path, struct fsal_attrlist *attrib,
			    struct fsal_obj_handle **handle,
			    struct fsal_attrlist *attrs_out,
			    struct fsal_attrlist *parent_pre_attrs_out,
			    struct fsal_attrlist *parent_post_attrs_out);
fsal_status_t dotfs_readlink(struct fsal_obj_handle *obj_hdl,
			     utf8string *link_content, bool refresh);
fsal_status_t dotfs_getattrs(struct fsal_obj_handle *obj_hdl,
			     struct fsal_attrlist *attrs_out);
fsal_status_t dotfs_link(struct fsal_obj_handle *obj_hdl,
			 struct fsal_obj_handle *destdir_hdl, const char *name,
			 struct fsal_attrlist *destdir_pre_attrs_out,
			 struct fsal_attrlist *destdir_post_attrs_out);
fsal_status_t dotfs_rename(struct fsal_obj_handle *obj_hdl,
			   struct fsal_obj_handle *olddir_hdl,
			   const char *old_name,
			   struct fsal_obj_handle *newdir_hdl,
			   const char *new_name,
			   struct fsal_attrlist *olddir_pre_attrs_out,
			   struct fsal_attrlist *olddir_post_attrs_out,
			   struct fsal_attrlist *newdir_pre_attrs_out,
			   struct fsal_attrlist *newdir_post_attrs_out);
fsal_status_t dotfs_unlink(struct fsal_obj_handle *dir_hdl,
			   struct fsal_obj_handle *obj_hdl, const char *name,
			   struct fsal_attrlist *parent_pre_attrs_out,
			   struct fsal_attrlist *parent_post_attrs_out);
fsal_status_t dotfs_setattrs(struct fsal_obj_handle *obj_hdl, bool bypass,
			     struct state_t *state,
			     struct fsal_attrlist *attrib_set);
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
fsal_status_t dotfs_close(struct fsal_obj_handle *obj_hdl);
fsal_status_t dotfs_close_my_fd(dotfs_fd_t *my_fd);
fsal_status_t dotfs_handle_to_wire(const struct fsal_obj_handle *obj_hdl,
				   fsal_digesttype_t output_type,
				   struct gsh_buffdesc *fh_desc);
void dotfs_handle_to_key(struct fsal_obj_handle *obj_hdl,
			 struct gsh_buffdesc *key_desc);
struct state_t *dotfs_alloc_state(struct fsal_export *exp_hdl,
				  enum state_type state_type,
				  struct state_t *related_state);

#endif /* DOTFS_H */
