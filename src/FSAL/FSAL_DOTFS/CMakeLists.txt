# SPDX-License-Identifier: LGPL-3.0-or-later
#-------------------------------------------------------------------------------
#
# Copyright Panasas, 2012
# Contributor: Jim Lieb <jlieb@panasas.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 3 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
#
#-------------------------------------------------------------------------------

add_definitions(
  -D__USE_GNU
)

set(fsaldotfs_LIB_SRCS
  dotfs.h
  export.c
  handle.c
  helpers.c
  main.c
  messages.c
  messages.h
  sock.c
  storage.c
)

add_library(fsaldotfs MODULE ${fsaldotfs_LIB_SRCS})
add_sanitizers(fsaldotfs)

target_link_libraries(fsaldotfs
  ganesha_nfsd
  ${SYSTEM_LIBRARIES}       # like pthread, rt, dl
  ${LTTNG_LIBRARIES}        # Linux Trace Toolkit Next Generation (Tracing tools)
  ${LDFLAG_DISALLOW_UNDEF}  # compile time safety check
)

set_target_properties(fsaldotfs PROPERTIES VERSION 4.2.0 SOVERSION 4)
install(TARGETS fsaldotfs COMPONENT fsal DESTINATION ${FSAL_DESTINATION})

########### install files ###############
