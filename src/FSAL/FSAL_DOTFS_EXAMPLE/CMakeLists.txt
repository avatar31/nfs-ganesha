# SPDX-License-Identifier: LGPL-3.0-or-later
#-------------------------------------------------------------------------------
#
# CMakeLists.txt — FSAL_DOTFS
#
# Builds libfsaldotfs.so — the NFS-Ganesha FSAL plugin for dotfs, a
# distributed erasure-coded virtual filesystem backed by the omashu
# metadata store (BadgerDB + Raft) and the dotfs StorageEngine.
#
# Build requirements:
#   - NFS-Ganesha build tree (ganesha_nfsd target must exist)
#   - TODO: dotfs C-binding shared library (libdotfs.so) once available
#
# CMake variables consumed:
#   FSAL_DESTINATION   — install directory for FSAL shared libraries
#   SYSTEM_LIBRARIES   — system libs linked into every FSAL (pthread, etc.)
#   LDFLAG_DISALLOW_UNDEF — linker flag to catch missing symbols at link time
#
#-------------------------------------------------------------------------------

add_definitions(
  -D__USE_GNU
)

set(fsaldotfs_LIB_SRCS
  main.c
  export.c
  handle.c
  file.c
  # TODO: add state.c once NFSv4 open-state AVL tree is implemented
  # TODO: add xattrs.c once extended attribute support is added (P2)
  dotfs_methods.h
)

add_library(fsaldotfs MODULE ${fsaldotfs_LIB_SRCS})
add_sanitizers(fsaldotfs)

target_link_libraries(fsaldotfs
  ganesha_nfsd
  ${SYSTEM_LIBRARIES}
  ${LDFLAG_DISALLOW_UNDEF}
  # TODO: link the dotfs C-binding once its shared library is available:
  #   dotfs_cgo   (or the CGo-generated .so name)
)

set_target_properties(fsaldotfs PROPERTIES VERSION 4.2.0 SOVERSION 4)

install(
  TARGETS fsaldotfs
  COMPONENT fsal
  DESTINATION ${FSAL_DESTINATION}
)

########### install files ###############
