## 1. Compile check (fastest feedback loop)

Build only the DOTFS FSAL shared library, not the entire server:

```bash
# One-time: set up out-of-tree build
mkdir -p ~/build/ganesha && cd ~/build/ganesha
cmake ~/vscode/nfs-ganesha/src \
  -DCMAKE_BUILD_TYPE=Debug \
  -DUSE_FSAL_DOTFS=ON \
  -DUSE_FSAL_VFS=OFF \
  -DUSE_FSAL_PROXY_V4=OFF \
  -DUSE_FSAL_CEPH=OFF

# After every code change — only rebuilds what changed:
make -j$(nproc) fsaldotfs 2>&1 | head -50
```

Add `-DUSE_ASAN=ON` to catch memory bugs immediately during compile-and-run.

---

## 2. FSAL API unit tests (Ganesha's gtest framework)

NFS-Ganesha ships a complete FSAL test harness in fsal_api. Each file tests one op (lookup, read, write, mkdir…) for **latency** and **correctness** against a live FSAL via a running Ganesha instance.

**Create `src/gtest/fsal_api/test_dotfs_lookup.cc`** following the existing pattern:

```cpp
// Skeleton — mirrors test_lookup_latency.cc but targets DOTFS
#include "gtest.hh"
extern "C" {
#include "fsal.h"
#include "export_mgr.h"
}

#define TEST_ROOT "dotfs_test_dir"

namespace {
char *ganesha_conf = nullptr;
char *lpath = nullptr;
int dlevel = -1;
uint16_t export_id = 77;

class DotfsLookupTest : public gtest::GaneshaFSALBaseTest {
protected:
    void SetUp() override { gtest::GaneshaFSALBaseTest::SetUp(); }
    void TearDown() override { gtest::GaneshaFSALBaseTest::TearDown(); }
};
}

TEST_F(DotfsLookupTest, LOOKUP_NONEXISTENT) {
    struct fsal_obj_handle *lookup = nullptr;
    fsal_status_t status = root_entry->obj_ops->lookup(
        root_entry, "does_not_exist", &lookup, nullptr);
    EXPECT_EQ(status.major, ERR_FSAL_NOENT);
    EXPECT_EQ(lookup, nullptr);
}
```

Run the test binary against a Ganesha config:

```bash
./test_dotfs_lookup \
  -f /etc/ganesha/dotfs.conf \
  -p /tmp/dotfs_test \
  -x 77
```

---

## 3. Minimal Ganesha config for manual testing

Create `/etc/ganesha/dotfs.conf`:

```
NFS_CORE_PARAM {
    Protocols = 4;
}

EXPORT {
    Export_Id = 77;
    Path = /;
    Pseudo = /dotfs;
    Access_Type = RW;
    Squash = No_Root_Squash;
    FSAL {
        Name = DOTFS;
    }
}

LOG {
    Default_Log_Level = DEBUG;
    Components {
        FSAL = FULL_DEBUG;
    }
}
```

Run (as root, Linux only):

```bash
# Install the .so first
make install

# Run ganesha in the foreground with full FSAL debug
ganesha.nfsd -F -L /dev/stderr -f /etc/ganesha/dotfs.conf

# In another terminal — mount and exercise manually
mount -t nfs4 -o vers=4.1 localhost:/ /mnt/dotfs
ls /mnt/dotfs
```

Watch for `COMPONENT_FSAL` log lines showing which ops are called and whether they return `ERR_FSAL_NOTSUPP` (stubs) or errors.

---

## 4. Filesystem correctness tests

Once ops return real data:

```bash
# pjdfstest — POSIX correctness (runs as root)
cd /mnt/dotfs
prove -r /opt/pjdfstest/tests/

# fsstress — concurrency / stress
fsstress -d /mnt/dotfs -n 1000 -p 8

# xfstests — comprehensive FS regression suite
cd /opt/xfstests
./check -g quick
```

---

## Iterative development loop

```
edit handle.c / file.c
     │
     ▼
make -j$(nproc) fsaldotfs     ← catches compile + linker errors
     │
     ▼
restart ganesha.nfsd           ← dlopen picks up new .so
     │
     ▼
run the specific gtest binary  ← targeted op test
     │
     ▼
mount + manual smoke test      ← ls, cat, cp
     │
     ▼
pjdfstest / fsstress           ← once the op is functionally complete
```

The `make fsaldotfs` step is the critical inner loop — it recompiles only your FSAL and tells you immediately when a function signature doesn't match a prototype in `dotfs_methods.h` or an `ops->` assignment is wrong.

--- DEBUGGING ---

## Debugging and Logging for FSAL_DOTFS

---

### 1. Ganesha's built-in logging system

Ganesha has a structured `LogXxx(COMPONENT, fmt, ...)` macro system. In your FSAL code you already use `COMPONENT_FSAL`. The full severity ladder:

```c
LogFatal(COMPONENT_FSAL, "...");   // process exits
LogCrit(COMPONENT_FSAL, "...");    // serious error, keeps running
LogWarn(COMPONENT_FSAL, "...");    // unexpected but recoverable
LogInfo(COMPONENT_FSAL, "...");    // normal operational events
LogDebug(COMPONENT_FSAL, "...");   // per-call tracing
LogFullDebug(COMPONENT_FSAL, "..."); // hot-path, very verbose
```

Set the level per-component in `ganesha.conf`:

```
LOG {
    Default_Log_Level = WARN;      # baseline for everything else
    Components {
        FSAL         = FULL_DEBUG; # your FSAL code
        CACHE_INODE  = DEBUG;      # MDCACHE decisions
        EXPORT       = DEBUG;      # export mount/unmount
        STATE        = DEBUG;      # NFSv4 open/lock state
        NFS_V4       = DEBUG;      # protocol decode/encode
    }
    Facility {
        name  = FILE;
        destination = "/var/log/ganesha/ganesha.log";
        enable = active;
    }
}
```

Run ganesha in the foreground so logs go to stderr immediately:

```bash
ganesha.nfsd -F -L /dev/stderr -f /etc/ganesha/dotfs.conf
```

| Flag | Meaning |
|---|---|
| `-F` | Foreground (don't daemonize) |
| `-L /dev/stderr` | Log destination |
| `-f <conf>` | Config file |
| `-d` | Extra debug output at startup |

---

### 2. Add strategic logging in your stub functions

Every `ERR_FSAL_NOTSUPP` return is invisible without a log line. Before each TODO return, add:

```c
fsal_status_t dotfs_lookup(struct fsal_obj_handle *parent, const char *name,
                           struct fsal_obj_handle **handle,
                           struct fsal_attrlist *attrs_out)
{
    LogDebug(COMPONENT_FSAL, "DOTFS lookup: name=%s", name);

    // ... implementation ...

    LogDebug(COMPONENT_FSAL, "DOTFS lookup: name=%s -> major=%d",
             name, status.major);
    return status;
}
```

For I/O hot paths use `LogFullDebug` to avoid flooding at `DEBUG` level:

```c
void dotfs_read2(struct fsal_obj_handle *obj_hdl, bool bypass,
                 fsal_async_cb done_cb, struct fsal_io_arg *read_arg,
                 void *caller_arg)
{
    LogFullDebug(COMPONENT_FSAL,
                 "DOTFS read2: offset=%" PRId64 " len=%zu",
                 read_arg->offset, read_arg->iov[0].iov_len);
    // ...
}
```

---

### 3. GDB — step through a live FSAL call

Build with debug symbols (no optimisation):

```bash
cmake ~/vscode/nfs-ganesha/src \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-O0 -g3" \
  -DUSE_FSAL_DOTFS=ON
make -j$(nproc) fsaldotfs ganesha.nfsd
```

Attach GDB to the running server:

```bash
# Start ganesha
sudo ganesha.nfsd -F -f /etc/ganesha/dotfs.conf &
GPID=$!

# Attach
sudo gdb -p $GPID

# Inside GDB — set a breakpoint on your function
(gdb) break dotfs_lookup
(gdb) break dotfs_read2
(gdb) continue

# When hit, inspect state
(gdb) bt               # backtrace showing the full NFS call stack
(gdb) p *myself        # print the dotfs_fsal_obj_handle
(gdb) p name           # the filename being looked up
(gdb) p openflags      # open flags
(gdb) finish           # run to end of this function
```

For shared library symbols (dlopen'd FSAL):

```bash
(gdb) info sharedlibrary          # confirm libfsaldotfs.so is loaded
(gdb) set solib-search-path ~/build/ganesha/FSAL/FSAL_DOTFS
```

---

### 4. AddressSanitizer — catch memory bugs at runtime

```bash
cmake ... -DUSE_ASAN=ON -DUSE_UBSAN=ON
make -j$(nproc) fsaldotfs

# Run — ASAN output goes to stderr automatically
sudo ganesha.nfsd -F -L /dev/stderr -f /etc/ganesha/dotfs.conf
```

ASAN catches: heap-use-after-free, buffer overread/overwrite, double-free, use of uninitialised memory. Any violation prints a full stack trace and aborts.

---

### 5. Valgrind — deep memory analysis

For cases ASAN misses (leak detection, uninitialised value propagation):

```bash
sudo valgrind \
  --tool=memcheck \
  --leak-check=full \
  --track-origins=yes \
  --show-leak-kinds=all \
  --log-file=/tmp/vg-dotfs.log \
  ganesha.nfsd -F -f /etc/ganesha/dotfs.conf
```

Note: Valgrind is ~20× slower — only use for targeted reproduction, not general dev.

---

### 6. strace — trace every syscall

Useful when your dotfs C-binding makes unexpected syscalls, or you want to verify `open`, `read`, `write` are actually being issued:

```bash
# Attach to running ganesha
sudo strace -p $GPID -f \
  -e trace=open,openat,read,write,close,pread64,pwrite64 \
  -T -tt 2>&1 | grep -v ENOENT
```

| Flag | Meaning |
|---|---|
| `-f` | Follow forked threads |
| `-T` | Show time spent in each syscall |
| `-tt` | Timestamp each line |
| `-e trace=...` | Filter to specific syscalls |

---

### 7. Identifying which op is being called

When you first bring up the FSAL, many ops will return `ERR_FSAL_NOTSUPP`. To know which NFS RPC triggered which FSAL op, enable NFS protocol logging:

```
LOG { Components { NFS_V4 = DEBUG; FSAL = DEBUG; } }
```

The log will show:

```
[NFS_V4] COMPOUND v4 op 15 (LOOKUP) name=myfile
[FSAL]   DOTFS lookup: name=myfile
[FSAL]   DOTFS lookup: name=myfile -> major=10  ← ERR_FSAL_NOTSUPP
[NFS_V4] COMPOUND v4 op 15 LOOKUP returned NFS4ERR_NOTSUPP
```

This gives you a direct map: NFS4 op → FSAL function → your return code.

---

### 8. Runtime log level change without restart

Ganesha supports D-Bus log control — change log levels on a live server:

```bash
# Crank FSAL to FULL_DEBUG without restarting
gdbus call --system \
  --dest org.ganesha.nfsd \
  --object-path /org/ganesha/nfsd/log \
  --method org.ganesha.nfsd.log.SetLevel \
  "FSAL" "FULL_DEBUG"

# Drop back to WARN when done
gdbus call --system \
  --dest org.ganesha.nfsd \
  --object-path /org/ganesha/nfsd/log \
  --method org.ganesha.nfsd.log.SetLevel \
  "FSAL" "WARN"
```

---

### Quick reference — what to use when

| Problem | Tool |
|---|---|
| "Why does this NFS call fail?" | `LOG NFS_V4=DEBUG; FSAL=DEBUG` + log lines |
| "Where does execution go?" | GDB breakpoint + `bt` |
| "Memory corruption / crash" | `-DUSE_ASAN=ON` + ASAN output |
| "Memory leak" | Valgrind `--leak-check=full` |
| "What syscalls are issued?" | `strace -p $PID` |
| "Change log level live" | D-Bus `SetLevel` call |
| "Slow path hotspot" | `perf record -p $PID; perf report` |
