# SWGEmu Database Converter

A standalone offline tool for migrating SWGEmu Berkeley DB databases between server code versions. Built for the SWGEmu Core3 engine.

> **WARNING: This tool modifies your databases in place. Incorrect use will permanently destroy player data.**
>
> - **Always back up your databases before running this tool.** No backup = no recovery.
> - **Test the full conversion on a copy of your production data first.** Do not run this against a live production database until you have verified the results on a test environment.
> - **Verify the converted server boots cleanly and players can log in** before deleting your backup.
> - **Read the [Core3 source changes](docs/CORE3_CHANGES.md) carefully.** Missing even one change can silently zero out template CRCs, erase player skills, or wipe zone data — with no errors during conversion.
>
> This tool has been tested extensively on databases ranging from 1M to 65M objects, but every server has different custom content and code modifications. Your setup may surface edge cases that haven't been seen before. Test first. Then test again.

## The Problem

SWGEmu stores all game objects (players, buildings, items, creatures) as serialized C++ objects in Berkeley DB. When the server code changes — renamed fields, new fields, removed fields, different class hierarchies — the bytes stored in BDB no longer match what the current code expects.

On first boot after a code change, the server's CRC dirty-detection system sees that **every single object** has changed. It marks them all dirty and tries to re-save them on the next auto-save cycle. With millions of objects, this overwhelms BDB with writes, causing the server to lag, crash, or corrupt the database.

This tool solves the problem by converting all objects **offline** before the server starts. After conversion, the stored bytes match exactly what the current code would produce, so zero objects are dirty on first boot.

## Pipeline Overview

The conversion runs as a 4-phase pipeline. Each phase writes a manifest file and the next phase refuses to run without it, preventing out-of-order execution.

```
Phase 1: clean       Strip BDB LSNs, remove __db.* and log.* files
Phase 2: hashfix     Byte-level field hash replacement (~500K records/sec)
Phase 3: reserialize C++ object round-trip (classic=all, smart=selective)
Phase 4: finalize    BDB checkpoint, remove __db.*, prepare for core3
```

Two modes are available:

- **Classic** — hashfix all records, then reserialize all records. Safe fallback, converts everything.
- **Smart** — hashfix all records with metadata scan, probe per class, only reserialize classes where bytes actually differ. On a typical production database, this reduces reserialize work by 90%+.

### Phase 1: Clean

Prepares databases for a fresh BDB environment:

1. If `log.*` files exist, open the BDB environment with `DB_RECOVER` and force a checkpoint — this replays any uncommitted transaction log data into the `.db` file pages before the logs are removed. Without this step, data written by tools that didn't checkpoint (e.g., small databases like guilds, chatrooms) would be permanently lost when the logs are deleted.
2. Remove `__db.*` files (shared memory regions) and `log.*` files (transaction logs)
3. Create a private BDB environment and call `lsn_reset()` on each `.db` file
4. Remove any leftover `__db.*` from the reset
5. Clear stale phase manifests and `.converted` files from previous runs
6. Write `.phase1_complete` manifest

### Phase 2: Hash Fix

SWGEmu's serialization format stores field names as 32-bit hashes:

```
[uint16 varCount]
[uint32 nameHash][uint32 dataSize][N bytes data]   <- field 1
[uint32 nameHash][uint32 dataSize][N bytes data]   <- field 2
...
```

When a class is renamed upstream (e.g., `QuadTreeEntry` -> `TreeEntry`), the field name hashes change. Phase 2 scans every record's raw bytes and replaces old hashes with new ones. No C++ objects are created — it's a pure byte-level scan-and-replace that runs at ~500K records/second.

The hash replacements:

```cpp
{ 0x2970c5d9 -> 0x763502f7, "coordinates" }  // QuadTreeEntry -> TreeEntry
{ 0x5a47d37d -> 0xb1649cd1, "bounding"    }
{ 0x5284d7c8 -> 0x256e90b3, "parent"      }
{ 0xac3d85f4 -> 0xdbd7c28f, "radius"      }
```

**Why hash fix must come first:** Without fixing hashes, `readObject()` can't find renamed fields by their old hash codes. The data silently reads as zeros. When `writeObject()` then saves the object, coordinates are permanently zeroed — all objects end up at 0,0,0.

In **smart mode** (`--smart`), Phase 2 also extracts per-class metadata during the same scan pass:
- `_className` for each record
- Coordinate values (`posX`, `posZ`) via `MAX(ABS())` per class
- First 20 and last 20 OIDs per class (quorum probe candidates for Phase 3)

### Phase 3: Reserialize

Each object is loaded through the full C++ deserialization chain (`ObjectManager::loadPersistentObject`), then re-serialized with `writeObject()` and written back to BDB. This catches everything Phase 2 can't: new fields with default values, removed fields, reordered fields, changed data types, and any other structural changes.

**Classic mode:** Converts every record in every database.

**Smart mode:** Uses metadata from Phase 2 to selectively skip unchanged classes in `sceneobjects` (the only database large enough to benefit). All other databases are fully reserialized regardless of mode.

For `sceneobjects`:

1. **Quorum probe** — for each unique `_className`, load up to 40 records (first 20 + last 20 OIDs from Phase 2). For each: load, reserialize, compare bytes with current BDB data.
2. If **any** of the 40 probe records differ, the entire class is marked `RESERIALIZE`.
3. Print a class report showing which classes need work and which are clean.
4. **Selective reserialize** — iterate all records, read `_className`, skip records whose class is `CLEAN`. Only reserialize records whose class needs it.

Example smart mode output:
```
    TangibleObject                   479511  maxCoord=0.0      CLEAN
    WeaponObject                     121735  maxCoord=0.0      CLEAN
    FurnitureObject                   33033  maxCoord=5241.3   RESERIALIZE
    PlayerObject                         15  maxCoord=0.0      RESERIALIZE
    ──────────────────────────────────────────────
    Total to reserialize: 33048 / 838886 (3.9%)
```

Key safety measures:

- **`Core::MANAGED_REFERENCE_LOAD = false`** — Prevents `ManagedReference` from resolving cross-references. Without this, loading one object would trigger loading every object it references, blowing up memory.
- **`initializeTransientMembers()` suppressed** — The autogen patch prevents runtime initialization code from corrupting persistent data during conversion.
- **500-object batches with DOB eviction** — Objects are evicted from the Distributed Object Broker after every 500 objects to keep memory bounded.
- **Direct `putData()`** — Writes go straight to BDB, bypassing the dirty-flag system.

### Phase 4: Finalize

Proper BDB cleanup using engine3's checkpoint API:

1. Initialize engine with `ObjectDatabaseManager` (opens BDB environment with transactions)
2. Force a BDB checkpoint (`txn_checkpoint(DB_FORCE)`) — flushes all dirty pages to disk
3. Remove `__db.*` files (shared memory regions, per-process, always recreated)
4. Remove all phase manifest files (conversion is complete)

**Important:** Phase 4 does **not** remove `log.*` files. The `.db` files contain LSNs that reference the log files. core3 opens with `DB_RECOVER` on startup, which reconciles the log state. Removing logs before core3 sees them causes the "LSN past end of log" error.

---

## Quick Start

```bash
# 1. Back up your databases (always!)
./scripts/backup.sh pre_convert /path/to/MMOCoreORB/bin/databases

# 2. Build dbconvert (patches engine3 temporarily, reverts when done)
./scripts/build.sh /path/to/MMOCoreORB

# 3. Stop core3 if running, then convert
cd /path/to/MMOCoreORB/bin
./dbconvert all              # Classic mode (safe, converts everything)
./dbconvert all --smart      # Smart mode (probes per class, skips unchanged)

# 4. Start the server — zero dirty objects on first boot
./core3
```

The build script (`scripts/build.sh`) handles everything automatically:

1. Copies `dbconvert.cpp` into the server source tree
2. Applies 4 engine3 patches for standalone mode
3. Patches ~325 autogenerated `readObject()` methods
4. Adds the dbconvert CMake target
5. Builds with `cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo .. && make dbconvert`
6. **Reverts all patches and removes `dbconvert.cpp` on exit** (even if the build fails)

Engine3 is always left clean.

---

## Cleanup After Conversion

After the server boots successfully, these files can be safely removed:

### `bin/` directory

| File | Purpose | Safe to remove? |
|------|---------|-----------------|
| `bin/dbconvert` | The converter binary | Yes — only needed for conversion |
| `databases/dbconvert_errors.log` | Error log from conversion | Yes — review first, then delete |
| `databases/*.converted` | Per-database resume manifests | Yes — removed by Phase 4, but may remain if Phase 4 was skipped |
| `databases/.phase*_complete` | Phase gate manifests | Yes — removed by Phase 4 |

### `databases/` directory (generated by BDB at runtime)

| File pattern | Purpose | Safe to remove? |
|--------------|---------|-----------------|
| `databases/__db.*` | BDB shared memory regions | Yes — recreated on every core3 start |
| `databases/log.*` | BDB transaction logs | **Only after** core3 has booted successfully once (it replays them via `DB_RECOVER`) |

### Source tree (if using manual build)

If you followed the manual build steps in [INSTALL.md](docs/INSTALL.md) instead of `build.sh`, these were added to your source tree and can be removed after building:

```bash
rm -f src/tools/dbconvert.cpp        # Converter source (not needed by core3)
rmdir src/tools 2>/dev/null           # Only if empty
```

The autogen patches (`if (Core::MANAGED_REFERENCE_LOAD) initializeTransientMembers()`) are harmless and get overwritten on the next `make rebuild-idl`. The CMakeLists.txt additions are also harmless — the `if(EXISTS)` / `if(TARGET)` guards prevent errors when `dbconvert.cpp` is absent.

---

## Commands

### Full Pipeline

```bash
./dbconvert all                 # Phases 1-2-3-4, classic mode
./dbconvert all --smart         # Phases 1-2-3-4, smart mode
```

Runs all four phases in order. If interrupted, re-run the same command — Phase 3 skips already-converted databases via `.converted` manifest files.

### Individual Phases

Each phase can be run independently. Phase gates enforce ordering: each phase requires the previous phase's manifest to exist.

```bash
./dbconvert clean               # Phase 1: Strip LSNs, clean environment
./dbconvert hashfix             # Phase 2: Byte-level hash replacement
./dbconvert hashfix --smart     # Phase 2: Hash replacement + metadata scan
./dbconvert reserialize         # Phase 3: Classic (all records)
./dbconvert reserialize --smart # Phase 3: Smart (probe + selective)
./dbconvert finalize            # Phase 4: Checkpoint + cleanup
```

Running phases individually is useful for:
- **Debugging:** Run just Phase 2 to verify hash fix counts match expectations
- **Incremental work:** Run Phase 1-2, inspect, then decide classic vs smart for Phase 3
- **Recovery:** If Phase 3 fails, fix the issue and re-run Phase 3 alone (Phase 2 is still done)

### Resume After Interruption

If conversion is interrupted (crash, Ctrl+C, power loss), re-run the same command:

```bash
./dbconvert all --smart
```

Already-converted databases are tracked via `.converted` manifest files and automatically skipped. To force re-conversion of a specific database, delete its manifest and re-run Phase 3:

```bash
rm databases/sceneobjects.converted
./dbconvert reserialize --smart
```

---

## Performance

Scales from small dev databases to large production datasets. Phase 2 (hashfix) is I/O-bound; Phase 3 (reserialize) is CPU-bound.

| Phase | ~1M objects | ~65M objects |
|-------|-------------|--------------|
| Phase 1 (clean) | ~2s | ~5s |
| Phase 2 (hashfix) | ~30s (~500K/s) | ~2 min (~500K/s) |
| Phase 3 classic | ~90s (~12K/s) | ~13 min (~84K/s) |
| Phase 3 smart | ~10s (skips 90%+) | ~1 min (skips 90%+) |
| Phase 4 (finalize) | ~3s | ~5s |
| **Total (classic)** | **~2 min** | **~15 min** |
| **Total (smart)** | **~45s** | **~3 min** |

Memory usage stays bounded regardless of database size thanks to the 500-object batch eviction pattern.

---

## Error Handling

Errors are logged to both stdout and `databases/dbconvert_errors.log`:

```
[sceneobjects] OID=0x0001000009a75efe class=Component error=loadPersistentObject returned null (unregistered class?)
```

| Error | Meaning | Action |
|-------|---------|--------|
| `loadPersistentObject returned null` | Object's template CRC not registered | Usually removed custom content. Safe to ignore. |
| `Could not load database` | `.db` file exists but isn't in the manifest | Orphan file. Safe to ignore. |
| `StreamIndexOutOfBoundsException` | Read past end of stream | Corrupt record. Investigate the specific object. |
| `unknown exception (possible corrupt data)` | Unhandled exception during deserialization | Corrupt record. Object left as-is. |

Errors do **not** stop conversion — the tool logs them and continues. A small number of errors (< 50 out of 1M+) is normal for production databases with legacy custom content.

---

## Repository Structure

```
swgemu-dbconvert/
├── README.md
├── LICENSE
├── .gitignore
│
├── src/tools/
│   └── dbconvert.cpp                  # The converter (4-phase pipeline)
│
├── patches/                            # Engine3 patches (temporary, applied during build)
│   ├── 001-taskmanager-standalone-mode.patch
│   ├── 002-uncompress-error-handling.patch
│   ├── 003-objectdatabase-decompress-guard.patch
│   └── 004-orb-null-check.patch
│
├── scripts/
│   ├── build.sh                        # Automated build (patches -> compile -> revert)
│   ├── patch_autogen.py                # Patches autogenerated readObject() methods
│   ├── backup.sh                       # Snapshot databases before conversion
│   └── restore.sh                      # Restore databases from a snapshot
│
├── cmake/
│   └── CMakeLists.txt.patch            # CMake additions reference
│
├── docs/
│   ├── INSTALL.md                      # Step-by-step manual install guide
│   └── CORE3_CHANGES.md               # Required Core3 source modifications
│
└── examples/
    └── main.cpp.example                # Optional: add ./core3 convert subcommand
```

---

## Engine3 Patches

The `patches/` directory contains 4 minimal patches for engine3. These are **only needed during compilation** — `build.sh` applies them before building and reverts them immediately after, leaving engine3 clean.

| # | Patch | Purpose |
|---|-------|---------|
| 1 | `001-taskmanager-standalone-mode` | Returns early when `workerCount < 0` (no task threads). Prevents divide-by-zero. |
| 2 | `002-uncompress-error-handling` | Changes `uncompress()` from `void` to `bool`. Returns `false` on corrupt data. |
| 3 | `003-objectdatabase-decompress-guard` | getData/getNextKeyAndValue check uncompress() return. Skip corrupt records. |
| 4 | `004-orb-null-check` | Null check for `objectManager` in `lookUp()`. Prevents crash without full ORB. |

These patches do **not** change core3 behavior. They only add guards for edge cases that occur in standalone tool mode.

---

## Core3 Source Changes

In addition to the engine3 patches and autogen patch, dbconvert requires modifications to **16 Core3 source files**. These are permanent changes to your server code (not temporary build patches) that add:

- **Crash guards** — Early returns in `initializeTransientMembers()` / `notifyLoadFromDatabase()` to prevent segfaults when server infrastructure doesn't exist
- **Data preservation** — Cached values in `TemplateReference`, `ZoneReference`, `CreatureTemplateReference`, `SkillList`, and `AbilityList` that survive the load→save round-trip when manager lookups return null

Without these changes, dbconvert will either crash or **silently destroy data** (template CRCs zeroed, zone names erased, all player skills and abilities permanently lost).

**See [docs/CORE3_CHANGES.md](docs/CORE3_CHANGES.md) for the complete list with exact code for each change.**

The changes are safe for normal server operation — they only activate when `Core::MANAGED_REFERENCE_LOAD` is `false`, which only happens during conversion.

---

## The `initializeTransientMembers()` Patch

Every autogenerated `readObject()` method (from IDL files) ends with a call to `initializeTransientMembers()`. During normal server operation, this sets up runtime state. During **conversion**, it's catastrophic:

| Class | What it does | Consequence during conversion |
|-------|-------------|------------------------------|
| `SceneObjectImpl` | Loads template, creates ContainerComponent | Segfault (no TemplateManager) |
| `TangibleObjectImpl` | Resets `faction = 0` for non-standard factions | Factions lost |
| `BuildingObjectImpl` | Clears access lists, calls `updatePaidAccessList()` | Building access wiped |
| `CreatureObjectImpl` | Accesses ZoneServer, loads skills | Segfault or corrupt data |

The `scripts/patch_autogen.py` script patches all ~325 autogenerated files:

```cpp
if (Core::MANAGED_REFERENCE_LOAD) initializeTransientMembers();
```

`MANAGED_REFERENCE_LOAD` is `true` during normal server operation and `false` during conversion.

**Re-run this script after every `make rebuild-idl`** since IDL regeneration overwrites the patched files.

---

## Architecture

### Serialization Format

Every SWGEmu object serializes to a flat, hash-indexed binary format:

```
Offset  Content
------  -----------------------
0x0000  uint16 varCount
0x0002  uint32 nameHash_0 (STRING_HASHCODE of "ClassName.fieldName")
0x0006  uint32 dataSize_0
0x000A  byte[dataSize_0] data_0
...     repeats for each field
```

**Critical:** engine3 `readInt()` uses native byte order (little-endian on x86_64), **not** big-endian network order. Hash replacements use `memcpy()` to match.

### Phase Gate Manifests

```
.phase1_complete   <- written by clean
.phase2_complete   <- written by hashfix, requires .phase1_complete
.phase3_complete   <- written by reserialize, requires .phase2_complete
<dbname>.converted <- per-database resume tracking during Phase 3
(finalize)         <- requires .phase3_complete, removes all manifests
```

### Memory Management (Phase 3)

```
for each batch of 500 objects:
  1. loadPersistentObject(oid) -> object in DOB memory
  2. writeObject() -> new serialized bytes
  3. putData(oid, bytes) -> write to BDB
  4. After 500: commitLocalTransaction()
  5. removeObject(oid) from DOB for each -> frees C++ memory
  6. Continue with next batch
```

Memory usage stays constant regardless of database size.

### Smart Mode Data Flow

```
Phase 2 (hashfix --smart)
  |
  | For each record: extract _className, coordinates, OID
  | Build ClassInfo per class:
  |   - count, maxAbsX, maxAbsZ
  |   - first 20 OIDs, last 20 OIDs (ring buffer)
  v
Phase 3 (reserialize --smart)
  |
  | For each class: load probe OIDs (first 20 + last 20)
  |   Load -> reserialize -> compare bytes with BDB
  |   If ANY differ -> class marked RESERIALIZE
  |   If ALL match -> class marked CLEAN
  |
  | Full iteration: skip CLEAN records, only reserialize RESERIALIZE records
  v
Result: typically 3-10% of records actually need C++ round-trip
```

---

## Adapting for Your Server

### Different Field Renames

The hash table is specific to `QuadTreeEntry` -> `TreeEntry`. For other migrations:

1. Compute the old and new hashes:
   ```cpp
   printf("old: 0x%08x\n", STRING_HASHCODE("OldClass.fieldName"));
   printf("new: 0x%08x\n", STRING_HASHCODE("NewClass.fieldName"));
   ```
2. Add entries to the `HASH_TABLE[]` array in `dbconvert.cpp`

If no field names changed (only new/removed fields), Phase 3 handles it automatically. Phase 2 becomes a fast no-op (no hashes match).

### Custom Object Types

Custom C++ types are converted automatically as long as:
1. The class is registered in `ObjectManager` (has a `serverObjectCRC`)
2. `readObject()` and `writeObject()` work correctly
3. The autogenerated files were patched (if the class has IDL-generated code)

### Index Databases

Databases with "index" in the name are **skipped** — core3 rebuilds them at boot.

### Ephemeral Databases (Auto-Deleted)

The tool automatically deletes these databases during discovery since they are rebuilt from scratch by core3 on every boot:

| Database | Purpose | Why delete |
|----------|---------|------------|
| `clientobjects.db` | Client-side object cache | Rebuilt from templates at boot |
| `navareas.db` | Navigation mesh areas | Rebuilt from navmesh data at boot |
| `buffs.db` | Active buff state | Ephemeral — rebuilt at boot, no player data |

These contain no persistent player data and are never worth converting. You may also want to manually delete `spawnareas.db` (rebuilt from Lua spawn scripts at boot) before conversion.

---

## Helper Scripts

### `scripts/backup.sh`

```bash
./scripts/backup.sh <snapshot_name> [source_dir]
./scripts/backup.sh pre_convert /path/to/bin/databases
```

Creates a named copy of all `.db` files.

### `scripts/restore.sh`

```bash
./scripts/restore.sh <snapshot_name> [target_dir]
./scripts/restore.sh pre_convert /path/to/bin/databases
```

Wipes the target (`.db`, `log.*`, `__db.*`, `.converted`, `.progress` files) and copies the snapshot. Checks that core3 and dbconvert aren't running first.

### `scripts/patch_autogen.py`

```bash
python3 scripts/patch_autogen.py /path/to/MMOCoreORB/src
```

Patches all autogenerated `readObject()` methods. Must be re-run after every `make rebuild-idl`.

---

## Manual Build

See [docs/INSTALL.md](docs/INSTALL.md) for step-by-step instructions.

---

## Troubleshooting

### "Could not open databases/ directory"

Run `dbconvert` from the `bin/` directory where `databases/` is located.

### Phase gate error ("Phase X has not been completed")

Run the required phase first, or use `./dbconvert all` to run the full pipeline.

### Patch script reports "Skipped: 325, Patched: 0"

The pattern wasn't found. Verify:
1. Autogen files are built (`make rebuild-idl` or `make idlobjects`)
2. Open any autogen `.cpp` file, find `readObject()`, check the ending pattern

### Build fails with undefined references

Run `cmake ..` after adding the dbconvert target.

### Conversion crashes with segfault

- Verify the autogen patch was applied to the correct source tree
- Check `Core::MANAGED_REFERENCE_LOAD` is being set to `false`
- Use GDB: `gdb ./dbconvert` -> `run all` -> `bt` on crash

### Server boots but objects have wrong data

The autogen patch may not have applied:

```bash
grep -r "MANAGED_REFERENCE_LOAD.*initializeTransientMembers" src/autogen/ | wc -l
```

Should match ~325.

### guilds.db (or other small databases) broken after conversion

Phase 3 (reserialize) can occasionally corrupt small databases like guilds, chatrooms, or surveys. Symptoms include players not being in guilds on login, guild terminals showing no guilds, or similar missing data.

**The fix:** Replace the broken database with the pre-reserialize version (after Phase 1+2 but before Phase 3 damaged it) using a dump/load cycle to create a clean BDB file.

**Step 1: Locate the pre-reserialize backup.** If you used `scripts/backup.sh` before conversion, use that. Otherwise, if you still have the Phase 1+2 output (before Phase 3 ran), use that copy.

**Step 2: Dump, swap, and load.**

```bash
cd MMOCoreORB/bin

# Stop core3 if running

# Verify the clean copy has data
db5.3_stat -d /path/to/clean/guilds.db | grep "Number of keys"

# Dump the clean copy to text format
db5.3_dump /path/to/clean/guilds.db > /tmp/guilds_dump.txt

# Remove the broken database (db5.3_load will NOT overwrite existing files)
rm -f databases/guilds.db

# Load into a fresh database file
db5.3_load -f /tmp/guilds_dump.txt databases/guilds.db

# Remove stale shared memory regions
rm -f databases/__db.*

# Do NOT remove log.* files — other databases need them to boot

# Verify
db5.3_stat -d databases/guilds.db | grep "Number of keys"
db5.3_verify databases/guilds.db

# Start the server
./core3
```

**Why this works:** The `db5.3_dump` → `db5.3_load` cycle creates a brand new BDB file with zeroed page LSNs. When core3 starts with `DB_RECOVER`, pages with LSN 0 are treated as "never modified in the current log sequence" — recovery skips them. The existing `log.*` files remain valid for all other databases.

**Critical details:**
- **You must `rm -f` the broken file first** — `db5.3_load` refuses to overwrite existing files and fails silently.
- **Do not remove `log.*` files** — other databases have page LSNs referencing them. Removing logs causes "LSN past end of log" crashes on boot.
- **Do not use `db5.3_load -r lsn`** — it's unnecessary (the dump/load already creates clean LSNs) and the CLI tool silently corrupts large DB_HASH files (>700MB) like sceneobjects.db.
- **Remove `__db.*` files** — these are stale shared memory regions from the previous core3/dbconvert run.

**Why this only affects small databases:** `sceneobjects.db` is written through a raw BDB writer (TargetDB) that bypasses the transaction log system entirely. Small databases like guilds go through `ObjectDatabaseManager` which uses write-ahead logging — making them more sensitive to the reserialize round-trip.

### "LSN past end of log" on core3 startup

Phase 4 was not run, or was run incorrectly. Re-run `./dbconvert finalize` (requires Phase 3 complete) or run the full pipeline again from Phase 1.

---

## Prerequisites

- A working SWGEmu Core3 build environment (GCC/Clang, CMake, BerkeleyDB 5.3, Boost)
- Server must build and run successfully before adding dbconvert
- Python 3 (for `patch_autogen.py`)
- `conf/config.lua` must exist in the working directory — dbconvert loads it on startup for BDB environment configuration. Run dbconvert from the `bin/` directory where both `databases/` and `conf/` are located.

---

## License

MIT License. See [LICENSE](LICENSE) file.
