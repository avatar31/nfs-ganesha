Yes — for someone **writing a new FSAL**, I'd actually recommend almost the reverse of how most people learn Ganesha.

A lot of documentation and architecture discussions start with:

```text
RPC
 -> NFSv4
 -> Cache_inode
 -> FSAL
```

But as an FSAL developer, you're not implementing the protocol. You're implementing the contract that the upper layers consume.

I'd suggest:

## Phase 1: FSAL First (Most Important)

Understand:

```c
struct fsal_obj_handle
struct fsal_export
struct export_ops
struct fsal_obj_ops
```

Focus on:

```c
lookup()
create()
mkdir()
read()
write()
getattrs()
setattrs()
readdir()
```

Questions to answer:

* What object lifetime rules does Ganesha expect?
* When does Ganesha call `lookup()` vs `create()`?
* How are file handles generated?
* How are references counted?
* Which methods are mandatory?

At this stage, ignore NFS entirely.

Think:

> "What API contract must my filesystem backend satisfy?"

---

## Phase 2: Cache Inode Layer

Once you understand FSAL interfaces, study:

```text
Cache_inode/
```

This is where you'll learn:

```text
cache_inode_lookup()
cache_inode_readdir()
cache_inode_getattr()
```

The key insight is:

**Protocol code rarely calls your FSAL directly.**

Usually:

```text
NFS
  ->
Cache_inode
  ->
FSAL
```

Understanding cache_inode tells you:

* Why your FSAL methods get called
* When they're bypassed due to cache hits
* Why certain operations appear unexpectedly

Many new FSAL developers spend days debugging their FSAL when the cache layer is actually involved.

---

## Phase 3: Read One Complete Request Path

Don't learn the entire protocol layer.

Instead trace:

```text
LOOKUP
```

end-to-end.

Something like:

```text
nfs4_op_lookup()
    ->
cache_inode_lookup()
    ->
FSAL lookup()
```

Then:

```text
READ
```

```text
nfs4_op_read()
    ->
cache_inode_read()
    ->
FSAL read()
```

Then:

```text
CREATE
```

```text
nfs4_op_create()
    ->
cache_inode_create()
    ->
FSAL create()
```

After 3-4 operations you'll understand 80% of the interaction model.

---

## Phase 4: SAL (Only if Needed)

If your FSAL supports:

```text
NFSv4 opens
locks
delegations
```

then learn:

```text
SAL/
```

Otherwise postpone it.

Many FSAL implementations require surprisingly little SAL knowledge initially.

---

## A Practical Reading Order

If I were joining a team today to write a new FSAL, I'd read:

```text
1. FSAL_VFS
2. FSAL API headers
3. FSAL object model
4. Cache_inode lookup path
5. Cache_inode read path
6. NFSv4 lookup operation
7. NFSv4 read operation
8. SAL (later)
```

The reason I recommend **FSAL_VFS** first is that it's usually the cleanest "reference implementation" of how Ganesha expects an FSAL to behave.

---

One thing that catches almost every new FSAL developer: spend time understanding **reference counting and object lifetime** around `fsal_obj_handle`. Bugs there tend to cause much more pain than implementing `read()` or `lookup()`. If you tell me which backend you're writing (object store, distributed FS, proprietary storage, cloud storage, etc.), I can point out the specific FSAL operations that are usually hardest to implement correctly.

--- New Question ---

That actually narrows things down quite a bit.

For a proprietary **object storage** backend, you're likely implementing something that is fundamentally different from a POSIX filesystem, while NFS-Ganesha expects filesystem semantics. Most of the complexity won't be in the FSAL API itself—it will be in bridging that semantic gap.

A few things I'd focus on early:

### 1. Understand the Handle Model Before Anything Else

For object stores, the crucial question is:

> What uniquely identifies an object and can be turned into a stable NFS file handle?

Ganesha revolves around `fsal_obj_handle`. Every lookup, read, getattr, readdir, etc. eventually comes back to an object handle.

Before implementing operations, figure out:

* What is your backend's object identifier?
* Is it immutable?
* Can it survive server restart?
* Can you reconstruct an object from a file handle alone?

If this isn't designed well, everything downstream becomes painful.

---

### 2. Study Existing Non-POSIX FSALs

Instead of spending all your time in FSAL_VFS, look for FSALs that are not simple filesystem wrappers.

The interesting question is:

> How did they map non-filesystem storage into Ganesha's object model?

You'll often find patterns around:

* virtual directories
* synthetic inode numbers
* handle encoding
* metadata caching

Those design decisions matter more than the actual `read()` implementation.

---

### 3. Learn the Cache Layer Earlier Than Most People

For object storage, cache behavior becomes critical.

You'll want to understand:

```text
LOOKUP
  -> cache hit?
  -> FSAL lookup?

GETATTR
  -> cached?
  -> backend query?
```

Otherwise you may accidentally make expensive object-store calls for operations that Ganesha expects to be cheap.

---

### 4. Read These Operations in This Order

I'd personally study:

```text
lookup()
getattrs()
readdir()
read()
write()
create()
remove()
```

Not because they're equally important, but because they reveal the object model.

`lookup()` and `getattrs()` usually expose whether your mapping from NFS namespace to object-store namespace makes sense.

---

### 5. Pay Special Attention to Directories

Object stores generally don't have "real" directories.

NFS absolutely does.

So eventually you'll need answers for questions like:

```text
/foo
/foo/bar
/foo/bar/file.txt
```

Are directories:

* Explicit backend objects?
* Metadata entries?
* Virtual constructs derived from key prefixes?

Many FSAL implementations spend more code on directory semantics than on file I/O.

---

### 6. Don't Start by Reading All of NFSv4

A common mistake is:

```text
FSAL
 -> Cache
 -> NFSv4
 -> SAL
 -> RPC
```

and then getting lost in thousands of files.

For your use case, a better learning loop is:

```text
Client issues LOOKUP
      ↓
Protocol handler
      ↓
cache_inode_lookup()
      ↓
your FSAL lookup()
```

Trace one operation end-to-end.

Then do the same for:

```text
READ
GETATTR
READDIR
```

After that, the protocol layer becomes much less mysterious.

---

### 7. The First Design Review Question I'd Ask

For an object-store-backed FSAL, I'd want a whiteboard answer to:

```text
NFS Path
    ↓
FSAL Object
    ↓
Backend Object
```

For example:

```text
/tenant1/data/a.txt
      ↓
fsal_obj_handle
      ↓
bucket=tenant1
key=data/a.txt
object_id=xyz123
```

If that mapping is clean and stable, the rest of the FSAL implementation is usually manageable.

Out of curiosity: is your backend **S3-like (bucket/key)**, or is it more of a proprietary object database with its own namespace and metadata model? That distinction affects which FSAL operations tend to become the hardest parts.

--- New Question ---

If your goal is to write an FSAL for a proprietary object storage system, I'd prioritize FSALs based on **how similar their problems are to yours**, not based on popularity.

## 1. FSAL_CEPH (Very Worth Studying)

CephFS isn't object storage from Ganesha's perspective, but its FSAL is mature and heavily used.

Study it for:

* Object handle lifecycle
* Export management
* Reference counting
* Error handling patterns
* Ganesha coding conventions

Don't study it to learn namespace mapping.

Study it to learn:

> "How does a production-quality FSAL integrate with Ganesha?"

---

## 2. FSAL_RGW (If Present in Your Version)

This is often the most relevant one.

RGW = RADOS Gateway.

This is much closer to:

```text
NFS
   ↓
Object Storage
```

than VFS or CephFS.

Things to study:

* Object ↔ file mapping
* Synthetic directories
* Readdir implementation
* Handle generation

If your repository contains:

```text
src/FSAL/FSAL_RGW
```

I'd spend significant time there.

---

## 3. FSAL_PROXY

This one is underrated.

It doesn't teach storage semantics, but it teaches:

* FSAL contracts
* Object handle operations
* Export lifecycle
* Request forwarding

The code is often easier to follow than large production FSALs.

---

## 4. FSAL_VFS

Everybody studies this first.

You should read it, but don't live in it.

It assumes:

```text
open()
read()
write()
stat()
readdir()
```

already exist in the backend.

Your object store probably doesn't.

Read VFS to understand:

```c
fsal_obj_handle
fsal_export
obj_ops
```

Then move on.

---

## What I'd Actually Read

If I joined your project tomorrow, I'd probably do:

```text
FSAL_VFS
   ↓
FSAL_RGW
   ↓
Cache_inode lookup path
   ↓
Cache_inode readdir path
   ↓
FSAL_CEPH
```

---

## The Three Operations I'd Reverse Engineer First

Regardless of FSAL:

### lookup()

```text
NFS path
    ↓
backend object
```

This is where namespace design appears.

---

### readdir()

This is where most object-store-backed FSALs become interesting.

For a POSIX FS:

```text
readdir()
    ↓
kernel returns directory entries
```

For object storage:

```text
list objects
    ↓
construct directory entries
    ↓
generate cookies
```

Much more logic.

---

### handle_to_key / key_to_handle

The exact function names vary.

But somewhere every FSAL answers:

```text
NFS file handle
      ↓
backend object

backend object
      ↓
NFS file handle
```

This is often the hardest design problem in custom FSALs.

---

My recommendation: before reading thousands of lines of protocol code, spend a day tracing just these FSAL methods:

```c
lookup()
readdir()
create_handle()   // or equivalent
getattrs()
```

Those four functions will tell you more about how a non-POSIX FSAL works than reading a week of NFSv4 compound processing.

One thing I'm curious about: does your object store expose a **directory hierarchy natively**, or are directories being synthesized by the FSAL from object metadata? That answer changes the complexity of `lookup()` and `readdir()` dramatically.

--- New Question ---

Short answer: **usually no — SAL is not something you “implement for your storage backend.”** It sits *above* FSAL and is mostly about NFSv4 state, not storage.

But the real answer depends on what features you want to support.

---

# 1. What SAL actually is in Ganesha

In NFS-Ganesha, the **SAL (State Abstraction Layer)** handles:

* NFSv4 open/close state
* byte-range locks
* delegations
* client IDs
* sessions

Think of it as:

```text id="sal1"
NFSv4 protocol state machine
```

NOT storage.

---

# 2. Where SAL sits

```text id="sal2"
NFSv4 Protocol
      ↓
      SAL  ← state tracking (OPEN, LOCK, CLOSE)
      ↓
   Cache_inode
      ↓
     FSAL  ← your storage backend
      ↓
Object Store
```

👉 FSAL = “how do I read/write objects”
👉 SAL = “who has this file open, and with what rights?”

---

# 3. Do you need to modify SAL for your FSAL?

### ❌ Almost never

You do NOT implement SAL logic for your backend.

You do NOT plug storage behavior into SAL.

SAL is backend-agnostic.

It doesn’t care if your FSAL is:

* object storage
* Ceph
* POSIX
* database

---

# 4. What you *might* need to support in FSAL (indirectly)

Even though SAL is separate, it depends on FSAL capabilities.

So your FSAL must correctly support operations SAL relies on:

## A) OPEN/CREATE semantics

SAL expects FSAL to handle:

* create file
* truncate
* verify existence

So FSAL must support:

```text id="sal3"
create()
open()
truncate()
```

---

## B) LOCKing (important for object stores)

SAL implements locking logic, but FSAL must:

* enforce consistency if needed
* support atomic write semantics if possible
* handle byte-range operations or reject them

If your object store cannot do byte-range locking:

👉 FSAL must say “no lock support” or emulate carefully

---

## C) Delegations (advanced, optional)

SAL may hand out delegations for performance.

FSAL must ensure:

* cache consistency
* invalidation correctness

Object stores often:

* do NOT support strong consistency semantics
* so delegations may be limited or disabled

---

# 5. The real responsibility split

## SAL handles:

* OPEN state tables
* client sessions
* lock ownership
* replay protection

## FSAL handles:

* file existence
* reads/writes
* attributes
* namespace

---

# 6. What you *should care about* as FSAL writer

For your object storage FSAL, SAL becomes relevant only in these cases:

---

## Case 1: Open-heavy workloads

NFSv4 clients do:

```text id="sal4"
OPEN → READ → CLOSE
```

FSAL must ensure:

* object exists at OPEN time
* permissions checked correctly

---

## Case 2: Locking behavior mismatch

Object stores usually don’t support:

* POSIX locks
* byte-range locks

So SAL will call into FSAL lock operations.

You must decide:

```text id="sal5"
- reject locks (common)
- emulate locks in metadata store
- or no-op (risky)
```

---

## Case 3: Consistency expectations

SAL assumes:

* stable file identity
* consistent attributes during open state

FSAL must ensure:

* handle stability
* deterministic lookup

---

# 7. The most important misconception

> “Do I need to build SAL support for my storage backend?”

❌ No.

Correct framing:

> SAL is already built into Ganesha.
> Your FSAL must provide correct storage semantics so SAL can function.

---

# 8. Simple mental model

Think:

```text id="sal6"
FSAL = storage adapter
SAL  = NFSv4 state machine
```

You only implement the adapter.

---

# 9. When would you touch SAL code?

Only if:

* you are adding a new NFSv4 feature
* fixing lock/state bugs
* modifying protocol behavior

NOT when writing FSAL.

---

# 10. Bottom line

For your object storage FSAL:

✔ You do NOT implement SAL
✔ You do NOT extend SAL for storage
✔ You only ensure FSAL supports required semantics

SAL will “just work” as long as:

* lookup is stable
* attributes are consistent
* operations behave predictably
* locking is defined (even if limited)

---

If you want, I can map the **exact OPEN → FSAL call flow**, which is where SAL + FSAL interaction becomes very clear (and where most FSAL bugs actually show up in real deployments).

--- New Question ---

**Yes, absolutely.** This is a highly recommended and common architectural pattern when integrating NFS-Ganesha with modern orchestrators (like Kubernetes) or dynamic storage systems (like GlusterFS, Ceph, or your proprietary object storage).

You do not need to hardcode any `EXPORT {}` blocks in your master `ganesha.conf` file at startup. You can start the daemon with **zero exports** and dynamically inject or remove them at runtime using **D-Bus**.

---

## 1. How to Prepare the Master `ganesha.conf`

To enable dynamic exports, you simply omit the static `EXPORT` blocks entirely, but you **must** ensure the D-Bus parameter rules are configured so Ganesha spins up its listening management loop:

```nasl
# ganesha.conf (Minimal Dynamic Base)
NFS_CORE_PARAM {
    MNT_Port = 20048;
    NFS_Port = 2049;
    FSAL_Path = "/path/to/your/nfs-ganesha/build/FSAL";
    
    # Optional: Customize the D-Bus prefix if running multiple instances
    # Dbus_Name_Prefix = "org.ganesha.nfsd"; 
}

# No EXPORT blocks here! Ganesha will start cleanly with an empty export table.

```

---

## 2. Dynamic Export Management Options

When your control plane needs to add or remove an export at runtime, you have two native ways to communicate via D-Bus:

### Option A: Using `dbus-send` (Shell Scripts / CLI)

You can instruct Ganesha to dynamically read an isolated export configuration snippet or a raw config string using the `AddExport` method:

#### To Add an Export:

```bash
dbus-send --print-reply --system \
  --dest=org.ganesha.nfsd \
  /org/ganesha/nfsd/ExportMgr \
  org.ganesha.nfsd.exportmgr.AddExport \
  string:"/tmp/dynamic_export_101.conf" \
  string:"EXPORT(Export_Id=101)"

```

* **Argument 1 (`string:"/tmp/..."`):** Path to a standalone snippet file containing the exact `EXPORT { ... }` configuration block you want to load.
* **Argument 2 (`string:"EXPORT(...)"`):** A matching identification string selector so Ganesha knows which block inside that file to parse.

#### To Remove an Export:

When a user deletes a tenant or container, you simply wipe it by its unique `Export_Id`:

```bash
dbus-send --print-reply --system \
  --dest=org.ganesha.nfsd \
  /org/ganesha/nfsd/ExportMgr \
  org.ganesha.nfsd.exportmgr.RemoveExport \
  uint16:101

```

---

### Option B: Using the `ganeshactl` / `manage_exports` Tool

NFS-Ganesha ships with built-in administrative scripts that wrap these complex `dbus-send` strings into clean commands:

```bash
# Add a dynamic export
manage_exports add /tmp/new_share.conf 'export(export_id = 101)'

# Remove a dynamic export
manage_exports remove 101

```

---

## 3. Why This is Perfect for Object Storage

By adopting the D-Bus runtime approach, your workflow becomes highly decoupled:

1. When a tenant requests a new file-gateway access layer, your system orchestrator drops a tiny template file to a temporary location (e.g., `/var/run/ganesha/export_101.conf`) containing your `FSAL { Name = MYSTORAGE; ... }` definitions.
2. You fire the `AddExport` D-Bus command. Ganesha parses it instantly, loads your custom `.so` module, hooks up the export, and responds over the network to the client **in milliseconds—without drops or service restarts**.
3. You can safely delete the temporary snippet file after it's loaded; Ganesha holds the operational state directly inside memory.

--- New Question ---

Implementing a custom recovery backend inside NFS-Ganesha requires modifying the core protocol layer to recognize your new backend, hooking it into the configuration parser, and then implementing the required state tracking callback operations.

Here are the concrete steps to implement **Advanced Engine Mode** for your object-backed cluster.

---

## Step 1: Define the New Backend Token

First, you need to tell Ganesha's configuration parser that a new `RecoveryBackend` option exists.

Open **`src/ConfigParsing/config_parsing.c`** (or your version's equivalent file containing the `recovery_backend_types` array you shared earlier). Add your custom token identifier at the bottom:

```c
static struct config_item_list recovery_backend_types[] = {
    CONFIG_LIST_TOK("fs",              RECOVERY_BACKEND_FS),
    CONFIG_LIST_TOK("fs_ng",           RECOVERY_BACKEND_FS_NG),
    CONFIG_LIST_TOK("rados_kv",        RECOVERY_BACKEND_RADOS_KV),
    CONFIG_LIST_TOK("rados_ng",        RECOVERY_BACKEND_RADOS_NG),
    CONFIG_LIST_TOK("rados_cluster",   RECOVERY_BACKEND_RADOS_CLUSTER),
    CONFIG_LIST_TOK("custom_object",   RECOVERY_BACKEND_CUSTOM_OBJECT), /* Add this line */
    CONFIG_LIST_EOL
};

```

*(Make sure to also add `RECOVERY_BACKEND_CUSTOM_OBJECT` to the corresponding `enum recovery_backend_type` in the matching header file, usually `src/include/config_parsing.h`).*

---

## Step 2: Implement the Backend Callbacks

Create a new source file or append to your FSAL code to implement the recovery backend structure. Ganesha defines these callback hooks to handle the client lifecycle.

```c
#include "nfs4_recovery.h"

// 1. Called when Ganesha boots up
static int obj_recovery_init(void) {
    // Connect to your object store metadata service or shared database
    return obj_store_db_connect(); 
}

// 2. Called when an NFSv4 client opens its first state / connects
static void obj_add_clid(const char *clid_dir, const char *clid_name) {
    // clid_name contains the unique text string identifier of the client
    // Write this record out to your shared central DB
    obj_store_db_insert_client(clid_name);
}

// 3. Called when an NFSv4 client closes cleanly or unmounts
static void obj_rm_clid(const char *clid_dir, const char *clid_name) {
    // Delete the record from your central DB
    obj_store_db_delete_client(clid_name);
}

// 4. Called when a surviving node takes over a failed node's traffic
static int obj_recovery_read_clids(void) {
    // Query your central DB for all client strings belonging to the failed cluster node
    // Load them into Ganesha's internal memory struct so it knows who to expect
    return obj_store_db_fetch_and_load_clients();
}

// 5. Called when the recovery grace period ends
static void obj_end_grace(void) {
    LogDebug(COMPONENT_STATE, "Custom Object Recovery Grace Period Finished.");
}

// Bind your functions to the structural backend template
struct nfs4_recovery_backend object_backend = {
    .recovery_init        = obj_recovery_init,
    .end_grace           = obj_end_grace,
    .recovery_read_clids = obj_recovery_read_clids,
    .add_clid            = obj_add_clid,
    .rm_clid             = obj_rm_clid,
};

```

---

## Step 3: Register Your Backend in the Recovery Manager

Open **`src/protocols/NFS/nfs4_recovery.c`**. This file handles selecting the active backend at startup based on what was typed in the configuration file.

Find the initialization function (usually `nfs4_recovery_init` or `s_backend_init`) and add a switch case or conditional block for your new enum token:

```c
void s_backend_init(struct nfs4_recovery_backend **backend)
{
    switch (nfs_param.v4.recovery_backend) {
        case RECOVERY_BACKEND_FS:
            *backend = &fs_backend;
            break;
        // ... other existing backends ...
        
        case RECOVERY_BACKEND_CUSTOM_OBJECT:
            extern struct nfs4_recovery_backend object_backend;
            *backend = &object_backend;  // Hook in your implementation!
            break;
            
        default:
            LogCrit(COMPONENT_INIT, "Invalid recovery backend specified!");
            break;
    }
}

```

---

## Step 4: Enable it in `ganesha.conf`

Once everything is compiled back into your binary, update your `ganesha.conf` file to tell Ganesha to ignore local filesystems and route client states directly through your new cluster engine hooks:

```nasl
NFSv4 {
    # Match the string you defined in Step 1
    RecoveryBackend = "custom_object"; 
    
    # Optional: Adjust grace period duration in seconds (Default is usually 90)
    Grace_Period = 60; 
}

```

---

## Step 5: Test and Debug via Logs

Because you've set your logs to `FULL_DEBUG`, you can watch Ganesha hit your callbacks sequentially as network clients interact with it.

1. Start Ganesha: `sudo ./ganesha.nfsd -F -f ganesha.conf`
2. Mount the share from an NFSv4 client.
3. Look for your custom DB insertion logs:
```bash
tail -f /tmp/ganesha.log | grep -E "obj_recovery|obj_add_clid"

```



If you see the callbacks firing when clients connect and disconnect, your active-active distributed state tracking cluster plumbing is complete.


--- New Question ---

The recovery data is structurally **Key-Value objects**.

Even when NFS-Ganesha utilizes its default local filesystem backend (`fs` or `fs_ng`), it isn't writing bulk file data—it simply writes empty files where the **filename itself** is the Key, and the file contents are completely empty (0 bytes).

---

## 1. What the Data Actually Looks Like

When an NFSv4 client connects, it provides a unique long string called its Client Owner ID (often containing its machine UUID, hostname, or IP address). Ganesha converts this into a sanitized hexadecimal or ascii string.

* **The Key:** The unique client identifier (e.g., `nfsclient-01.local` or a hex representation like `41fa6e22...`).
* **The Value:** Strictly **empty** (or a timestamp). Ganesha only needs to know *existence*. If the key exists in your database, that client is authorized to reclaim its locks when a crash failover occurs.

### Example in Ganesha's native `fs` layer:

If you check `/var/lib/nfs/ganesha/v4recov/`, you will see something like:

```bash
$ ls -la /var/lib/nfs/ganesha/v4recov/
-rw-r--r-- 1 root root    0 Jun 21 15:30 ganesha-node-01.client_92a3f82c...

```

Notice the file size is **0 bytes**. The directory listing itself *is* the Key-Value database.

---

## 2. Calculating the Load on Your Metadata DB

Because this tracking is lightweight, the performance impact on your database will be minimal. It is **not** an inline operation for everyday file modifications like `READ` or `WRITE`.

Here is the exact lifecycle and transaction frequency to help you calculate your load:

### Operation 1: Client Establishment (`add_clid`)

* **When it happens:** Only when a client mounts the share for the first time, or after a client drops off the network for hours and re-authenticates.
* **DB Load:** **1 Write (INSERT) per client**. If you have 500 servers mounting your NFS share, this is a total of 500 insert operations *ever*, not per second.

### Operation 2: Regular Operations (`READ`/`WRITE`/`LOCK`)

* **When it happens:** Constant active traffic.
* **DB Load:** **Zero (0)**. Once the client is registered, regular file system modifications call your standard FSAL methods. They do not write anything to the state recovery database.

### Operation 3: Graceful Teardown (`rm_clid`)

* **When it happens:** Only when an administrator explicitly runs `umount` on a client machine.
* **DB Load:** **1 Write (DELETE) per unmount**.

### Operation 4: Failover / Node Crash Recovery (`recovery_read_clids`)

* **When it happens:** Only during an unexpected server crash when a surviving node takes over.
* **DB Load:** **1 Bulk Read (SELECT * )** to retrieve the full list of client keys so the server can initialize its grace period.

---

## The Verdict for Your Database

Unless your architecture involves thousands of container clients mounting and unmounting your NFS export every few seconds, the transaction volume on your metadata database for state recovery will register at **nearly 0% utilization**.

> ⚠️ **Where the real load goes:**
> Your database will face its primary load from standard FSAL operations if you map file/directory inodes to database records. However, for the **Cluster Recovery Backend (`add_clid`/`rm_clid`)**, the database handles almost nothing except minor lease state bookkeeping. Feel free to use any simple metadata database or key-value table without worrying about a performance bottleneck.

--- New Question ---

**Yes, your understanding is 100% correct.** This is one of the single biggest advantages of writing an FSAL for NFS-Ganesha instead of writing an NFS server from scratch.

NFS-Ganesha uses a cleanly layered architecture that separates the network protocol logic from the storage logic:

---

## What Ganesha Handles for You (The Heavy Lifting)

The core Ganesha daemon acts as a massive protocol translator and state-tracking engine. Your custom FSAL never has to worry about:

* **NFSv4 Lock State Machines:** Ganesha manages the complex client lock tables, lock conflicts (e.g., if Client A tries to lock a file that Client B already has locked), lease expirations, and `OPEN`/`CLOSE`/`LOCK`/`LOCKU` compound tracking entirely in its own memory space.
* **Network Serialization:** Ganesha handles the XDR (External Data Representation) decoding of incoming RPC packets and encoding of outgoing packets.
* **The Grace Period Engine:** Ganesha handles the timers, blocks new lock requests during a failover, and evaluates whether a reclaiming client is acting validly based on your recovery backend records.

---

## What Your Custom FSAL Actually Responsible For

Because Ganesha handles the abstract concept of *state*, your FSAL only needs to focus on executing the **primitive, raw storage operations** when Ganesha asks it to.

For example, when an NFSv4 client sends a `LOCK` request, Ganesha evaluates if that lock is legally allowed. If it is allowed, Ganesha updates its internal tracking tables and then maps that state to standard file access calls. Your FSAL just implements the basic file operations, such as:

```c
/* Ganesha calls this when it needs to read actual data */
static fsal_status_t mystorage_read(struct fsal_obj_handle *obj_hdl,
                                    uint64_t offset,
                                    size_t buffer_size,
                                    void *buffer,
                                    size_t *bytes_read) {
    // Your code just goes and gets the raw bytes from the Object Store bucket 
    // and copies them into the 'buffer'. You don't care about locks here!
}

```

### The Exception: Share Reservations (Optional)

The only time your FSAL needs to care slightly about coordination is if your underlying object storage engine has native capabilities to enforce atomic file updates or if you choose to implement `fsal_file_lock` hooks for clustered data safety down the line. But for standard NFSv4 operation, Ganesha's upper layers shield your code from the grueling complexity of the NFSv4 state machine.

--- New Question ---

Yes, absolutely. **NFS-Ganesha handles the locking logic completely.** You do not need to apologize for clarifying—this is a critical architectural point when building your own storage backend.

---

## Exactly How Ganesha Prevents Data Corruption in Memory

If 5 clients mount your object-backed share and all try to write to the exact same file simultaneously, here is how the layers interact:

### 1. The Client Requests a Lock

Before a client writes to a file, an application running on that client usually issues an NFSv4 `LOCK` compound request to the NFS-Ganesha server.

### 2. Ganesha Evaluates the Lock Table (In-Memory)

The core engine of NFS-Ganesha maintains an internal, highly optimized in-memory state table.

* **If Client 1 gets there first:** Ganesha assigns an exclusive lock to Client 1 in its internal tables and returns a success status (`NFS4_OK`) back to Client 1.
* **If Clients 2 through 5 try to lock it immediately after:** Ganesha looks at its memory table, sees that Client 1 holds an exclusive lock on that file byte-range, and **automatically blocks or rejects** the requests from Clients 2–5 with an `NFS4ERR_DENIED` or `NFS4ERR_BLOCKED` status.

Clients 2 through 5 will sit and wait at the network layer. Their write requests won't even reach your custom FSAL code yet because Ganesha intercepts and pauses them.

### 3. The Write Passes to Your FSAL

Once Ganesha guarantees that Client 1 has the legal right to write, it calls your FSAL's write function (`mystorage_write`). Your code only sees a single, verified write stream at that moment.

---

## What You *Do* Have to Care About in Your Custom FSAL

While Ganesha perfectly manages the protocol-level locks, you must ensure that your backend implementation respects basic multi-threaded safety rules:

* **Thread Safety in C:** Ganesha is heavily multi-threaded. If two different clients are writing to **two different files** at the same time, Ganesha will call your FSAL's write function on two different threads simultaneously. You must ensure your custom backend code uses mutexes or thread-safe SDK handles so that internal memory arrays or network sockets don't experience race conditions.
* **Atomic Object Commits:** If your object storage works by uploading whole objects (like an S3 PUT), and two clients manage to write to the same file because they didn't use application-level locking, your backend will receive two sequential write streams. You should ensure your backend handles this gracefully (e.g., the last write wins, or you update the specific object bytes atomically depending on your underlying object store's capabilities).

Ultimately, you can rest easy knowing that the complex, multi-client state tracking and locking enforcement happen before the request ever touches your code.
