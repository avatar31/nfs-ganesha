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

#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_config.h"
#include "FSAL/fsal_commonlib.h"

#include "dotfs.h"

/**
 * @brief Populates a Ganesha attribute list with static directory attributes for root export.
 *
 * @param[in,out] attrs Pointer to the attribute list to populate. If NULL, the function does nothing.
 * @param[in] fileid The unique file identifier (inode number) for the root directory.
 * @param[in] fsid The filesystem identifier for the export.
 */
void set_root_attrs(struct fsal_attrlist *attrs, uint64_t fileid,
            fsal_fsid_t fsid)
{
    memset(attrs, 0, sizeof(struct fsal_attrlist));

    attrs->request_mask = ATTR_TYPE | ATTR_MODE | ATTR_NUMLINKS | ATTR_OWNER | 
                          ATTR_GROUP | ATTR_SIZE | ATTR_FILEID | ATTR_FSID;

    attrs->type = DIRECTORY;
    attrs->mode = 0755;         /* rwxr-xr-x */
    attrs->numlinks = 2;        /* Standard minimal links for a directory i.e. . and .. */
    attrs->owner = 0;           /* root */
    attrs->group = 0;           /* root */
    attrs->filesize = 4096;     /* Standard dummy directory size */
    attrs->spaceused = 4096;
    
    attrs->fileid = fileid; 
    
    /** 
     * FSID identifies the filesystem instance
     * The FSID Attribute: Pay close attention to attrs->fsid. NFS requires a file system identifier
     * to understand where one mount point ends and another begins. If you have different exports
     * running on the same server, their fsid.major numbers must be completely unique, or clients
     * will experience mounting conflicts (such as masking or caching overlap bugs).
     */
    attrs->fsid = fsid;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    attrs->atime = now;
    attrs->mtime = now;
    attrs->ctime = now;
    
    attrs->valid_mask = attrs->request_mask;
    attrs->supported = DOTFS_SUPPORTED_ATTRIBUTES | ATTR_FSID | ATTR_FILEID;
}

dotfs_fsal_obj_handle_t *dotfs_alloc_handle(dotfs_fsal_export_t *exp_hdl,
                                            struct fsal_attrlist *attr,
                                            handle_type_t type,
                                            const char *backend_key)
{
    dotfs_fsal_obj_handle_t *hdl = NULL;
    dotfs_fsal_module_t *my_module = NULL;
    size_t key_len = 0;
    if (backend_key) {
        key_len = strlen(backend_key) + 1;
    }

    if (!exp_hdl || !attr) {
        return NULL;
    }

    my_module = container_of(exp_hdl->export.fsal, dotfs_fsal_module_t, module);

    /* Contiguous Allocation: Allocate Handle + Backend Key in one shot */
    hdl = gsh_calloc(1, sizeof(dotfs_fsal_obj_handle_t) + key_len);

    /* Assign the dynamic tail memory pointer to your internal key buffer */
    if (key_len > 0) {
        /* Map the key string area directly behind the main handle struct allocation */
        hdl->object_key = (char *)&hdl[1];
        memcpy(hdl->object_key, backend_key, key_len);
    }

    hdl->parent_export = exp_hdl;
    hdl->type = type;
    pthread_mutex_init(&hdl->obj_mutex, NULL);

    hdl->handle.handle_len = snprintf((char *) hdl->handle.handle_data, sizeof(hdl->handle.handle_data), "%s", hdl->object_key);

    hdl->fsal_handle.type = attr->type;       /* DIRECTORY, REGULAR_FILE, etc. */
    hdl->fsal_handle.fsid = attr->fsid;       /* Filesystem ID identifier */
    hdl->fsal_handle.fileid = attr->fileid;   /* The unguessable unique 64-bit inode */

    /* Core Ganesha Handle Construction (Configures internal atomic refs) */
    fsal_obj_handle_init(&hdl->fsal_handle, &exp_hdl->export, attr->type, true);

    /* Setup Global Core Vtable Reference */
    hdl->fsal_handle.obj_ops = &my_module->handle_ops;

    fsal_copy_attrs(&hdl->attrs, attr, false);

    return hdl;
}
