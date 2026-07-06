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

#include "fsal_convert.h"
#include "FSAL/fsal_config.h"
#include "FSAL/fsal_commonlib.h"

#include "dotfs.h"


fsal_status_t storage_get_share_details(const char *path);

fsal_status_t storage_export_lookup(dotfs_fsal_obj_handle_t *my_parent,
        const char *name, struct fsal_obj_handle **new_fsal_hdl)
{
    fsal_status_t status = {ERR_FSAL_NO_ERROR, 0};
    struct fsal_attrlist obj_attrs;
    dotfs_fsal_obj_handle_t *allocated_hdl = NULL;
    char target_backend_key[PATH_MAX];
    int rc;

    rc = snprintf(target_backend_key, sizeof(target_backend_key), "%s/%s",
                  my_parent->object_key, name);
    if (rc >= sizeof(target_backend_key)) {
        return fsalstat(ERR_FSAL_NAMETOOLONG, 0);
    }

    // TODO: Fetch details from the backend storage service for the given export name

    // TODO: Replace id 2 with the actual export ID retrieved from the backend storage service
    set_root_attrs(&obj_attrs, 2, my_parent->fsal_handle.fsid);

    allocated_hdl = dotfs_alloc_handle(my_parent->parent_export,
                                    &obj_attrs,
                                    HANDLE_TYPE_REGULAR_FILE_OR_DIR,
                                    target_backend_key);

    *new_fsal_hdl = &allocated_hdl->fsal_handle;

    LogInfoMsg("Successfully resolved backend object. Key: %s, Inode: %lu, Type: %s",
             target_backend_key, (unsigned long)obj_attrs.fileid, 
             object_file_type_to_str(obj_attrs.type));

    return (status);
}

fsal_status_t storage_file_or_dir_lookup(dotfs_fsal_obj_handle_t *my_parent,
        const char *name, struct fsal_obj_handle **new_fsal_hdl)
{
    fsal_status_t status = {ERR_FSAL_NO_ERROR, 0};
    struct fsal_attrlist obj_attrs;
    dotfs_fsal_obj_handle_t *allocated_hdl = NULL;
    char target_backend_key[PATH_MAX];
    int rc;

    rc = snprintf(target_backend_key, sizeof(target_backend_key), "%s/%s",
                  my_parent->object_key, name);
    if (rc >= sizeof(target_backend_key)) {
        return fsalstat(ERR_FSAL_NAMETOOLONG, 0);
    }

    // TODO: Fetch details from the backend storage service for the given export name

    // TODO: Replace id 2 with the actual export ID retrieved from the backend storage service
    set_root_attrs(&obj_attrs, 2, my_parent->fsal_handle.fsid);

    allocated_hdl = dotfs_alloc_handle(my_parent->parent_export,
                                    &obj_attrs,
                                    HANDLE_TYPE_REGULAR_FILE_OR_DIR,
                                    target_backend_key);

    *new_fsal_hdl = &allocated_hdl->fsal_handle;

    LogInfoMsg("Successfully resolved backend object. Key: %s, Inode: %lu, Type: %s",
             target_backend_key, (unsigned long)obj_attrs.fileid, 
             object_file_type_to_str(obj_attrs.type));

    return (status);
}
