// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Author: Sachin S <sarodhesachin96@gmail.com>
 *
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

/* =========================================================================
 * Supported attribute mask
 *
 * ATTRS_POSIX covers the mandatory POSIX attribute set (mode, uid, gid,
 * size, atime, mtime, ctime, nlink, …).  Extend this once dotfs adds
 * ACL support (ATTR_ACL) or xattr support (ATTR4_XATTR).
 * ========================================================================= */

#define DOTFS_SUPPORTED_ATTRIBUTES ((const attrmask_t)(ATTRS_POSIX))

static const char myname[] = "DOTFS";

