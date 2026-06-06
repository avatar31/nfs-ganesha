# dotfs FSAL
This is the FSAL implementation for the dotfs filesystem. It is based on the FSAL_VFS implementation, but it is not a wrapper around the VFS. Instead, it is a direct implementation of the FSAL interface for the dotfs filesystem.

## Building

> [!NOTE]
> - The below steps are applicable in building in **Ubuntu / Debian** systems.
> - If you haven't cloned submodules, please run `git submodule update --init --recursive` before proceeding.

### Prerequisites
```sh
apt update

# Dev Dependencies
apt install -y build-essential gcc make cmake

# KRB5 and related dependencies
apt install -y libkrb5-dev comerr-dev libtirpc-dev krb5-multidev pkg-config

# ACL dependency
apt install -y libacl1-dev

# (Optional) jemalloc is a highly optimized memory allocator designed to prevent memory fragmentation and improve performance
apt install -y libjemalloc-dev

# D-Bus gives ability to send and receive dynamic control signals from the operating system or other running services.
apt install -y libdbus-1-dev

# NFSIDMAP is a library that provides functions for mapping between user and group IDs and their corresponding names, which is essential for the FSAL to correctly identify and manage file permissions and ownership.
apt install -y libnfsidmap-dev

# In an Active Directory (AD) enterprise network, Windows domain controllers attach extra user authorization details (like Group SIDs and security privileges) inside a Kerberos ticket payload called the PAC (Privilege Attribute Certificate).For a user-space file system server like NFS-Ganesha, enabling MSPAC_SUPPORT allows the server to use Samba's libwbclient to decode that Windows PAC payload. This means that when a user authenticates with Kerberos, the server can extract the user's group memberships and privileges from the PAC, which is crucial for enforcing correct access controls and permissions on the file system. Without MSPAC_SUPPORT, the server would not be able to utilize this information, potentially leading to incorrect permission handling for users authenticated via Kerberos in an AD environment.
apt install -y libwbclient-dev

# Enabling USE_CAPS allows the daemon to drop standard root privileges while retaining only the exact, microscopic Linux kernel capabilities it needs to modify file ownerships and cross security boundaries
apt install -y libcap-dev

# libunwind is a library that provides a programmatic interface for accessing the call stack of a running program. It allows developers to retrieve information about the function calls that led to a particular point in the execution of a program, which can be useful for debugging and profiling purposes.
apt install -y libunwind-dev

# The Userspace Read-Copy-Update (urcu) library handles highly parallel data synchronization across multiple CPU cores
apt install -y liburcu-dev

# Bison and Flex are compiler-construction tools (a parser generator and a lexical analyzer) that libntirpc and NFS-Ganesha use to read, parse, and process complex file configurations and network protocols. Without the actual executable binaries on your system, CMake cannot generate the required code parsers.
apt install -y bison flex

# NFS-Ganesha uses procps library to read system metric information directly from the Linux /proc filesystem. It allows the daemon to track memory footprint, operational statistics, and system resource limits.
apt install -y libproc2-dev

# Single command to install all dependencies
apt update && apt install -y \
    build-essential \
    gcc \
    make \
    cmake \
    libkrb5-dev \
    comerr-dev \
    libtirpc-dev \
    krb5-multidev \
    pkg-config \
    libacl1-dev \
    libjemalloc-dev \
    libdbus-1-dev \
    libnfsidmap-dev \
    libwbclient-dev \
    libcap-dev \
    libunwind-dev \
    liburcu-dev \
    bison \
    flex \
    libproc2-dev
```

### Building the FSAL
1. Clone the repository including all submodules:
```bash
git clone --recurse-submodules <repository_url>

# If you have already cloned the repository without the `--recurse-submodules` flag, you can initialize and update the submodules with the following command:
git submodule update --init --recursive
```
2. Navigate to the FSAL_DOTFS directory:
```bash
cd src/FSAL/FSAL_DOTFS
cd build
rm -rf *
clear
cmake ../../../ -DCMAKE_BUILD_TYPE=Debug -DUSE_FSAL_DOTFS=ON -DUSE_FSAL_VFS=OFF -DUSE_FSAL_PROXY_V4=OFF -DUSE_FSAL_PROXY_V3=OFF -DUSE_FSAL_CEPH=OFF -DUSE_FSAL_GPFS=OFF -DUSE_FSAL_MEM=OFF -DUSE_FSAL_LUSTRE=OFF -DUSE_FSAL_SAUNAFS=OFF
make -j$(nproc)
```
