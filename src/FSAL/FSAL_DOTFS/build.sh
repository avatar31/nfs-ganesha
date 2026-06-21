#!/bin/bash

cd build
rm -rf *
clear
cmake ../../../ -DCMAKE_BUILD_TYPE=Debug -DUSE_FSAL_DOTFS=ON \
    -DUSE_FSAL_PROXY_V4=OFF -DUSE_FSAL_PROXY_V3=OFF -DUSE_FSAL_CEPH=OFF \
    -DUSE_FSAL_GPFS=OFF -DUSE_FSAL_MEM=OFF -DUSE_FSAL_LUSTRE=OFF -DUSE_FSAL_SAUNAFS=OFF
make -j$(nproc) # this should generate ganesha.nfsd

# make install # this should install ganesha.nfsd to /usr/lib64/ganesha/

# ./ganesha.nfsd -F -f ../ganesha.conf
