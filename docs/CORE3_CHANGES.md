# Required Core3 Source Changes

dbconvert requires modifications to Core3 and engine3 source files beyond the 4 engine3 patches and the autogen patch. These changes fall into three categories:

1. **Engine3 infrastructure** — `Core::MANAGED_REFERENCE_LOAD` static bool and ManagedReference short-circuits
2. **Crash guards** — Early returns in `initializeTransientMembers()` / `notifyLoadFromDatabase()` to prevent crashes when server infrastructure (ORB, ZoneServer, managers) doesn't exist
3. **Data preservation** — Cached values that survive the deserialize→reserialize round-trip when manager lookups return null

Without these changes, dbconvert will either crash or silently destroy data (skills, abilities, template CRCs, zone names, creature templates).

---

## Engine3: `Core::MANAGED_REFERENCE_LOAD`

This static bool controls whether ManagedReferences resolve OIDs to live objects during deserialization. dbconvert sets it to `false` so objects can be loaded and re-saved without triggering the entire server dependency chain.

**Check if your engine3 already has this.** The upstream swgemu ODB3 tool uses `Core::MANAGED_REFERENCE_LOAD`, so many engine3 versions include it. If yours doesn't, add the following:

### `engine/core/Core.h`

Add to the `Core` class public section:

```cpp
static bool MANAGED_REFERENCE_LOAD;
```

Context:
```cpp
public:
    static SynchronizedHashTable<String, ArrayList<String>> properties;
    static AtomicInteger propertiesVersion;
    static bool MANAGED_REFERENCE_LOAD;  // <-- add this
```

### `engine/core/Core.cpp`

Add the static member definition:

```cpp
bool Core::MANAGED_REFERENCE_LOAD = true;
```

### `engine/core/ManagedReference.h`

Three changes inside `#ifdef ODB_REFERENCES` blocks. These short-circuit ManagedReference operations when `MANAGED_REFERENCE_LOAD` is false, preventing object resolution from BDB during conversion.

**1. `getObjectID()` — return cached OID directly:**

```cpp
inline uint64 getObjectID() const {
#ifdef ODB_REFERENCES
    if (!Core::MANAGED_REFERENCE_LOAD)
        return loadedOID;
#endif
    auto val = get();
    // ... rest unchanged
```

**2. `compareTo()` — compare using cached OIDs:**

```cpp
inline int compareTo(const ManagedReference& ref) const {
#ifdef ODB_REFERENCES
    if (!Core::MANAGED_REFERENCE_LOAD) {
        if (loadedOID < ref.loadedOID)
            return 1;
        else if (loadedOID > ref.loadedOID)
            return -1;
        else
            return 0;
    }
#endif
    // ... rest unchanged
```

**3. `parseFromBinaryStream()` — read OID but skip resolving it:**

```cpp
template<class O> bool ManagedReference<O>::parseFromBinaryStream(ObjectInputStream* stream) {
#ifdef ODB_REFERENCES
    uint64 oid = loadedOID = stream->readLong();

    if (!Core::MANAGED_REFERENCE_LOAD)
        return true;
#else
    uint64 oid = stream->readLong();
#endif
    // ... rest unchanged (object lookup)
```

---

## Category A: Crash Guards

These prevent segfaults and null dereferences when objects call `initializeTransientMembers()` or `notifyLoadFromDatabase()` in standalone mode. Each guard checks `Core::MANAGED_REFERENCE_LOAD` and skips operations that require server infrastructure.

**All files in this category need:** `#include "engine/core/Core.h"` (add if not already present).

### 1. `src/server/zone/objects/scene/SceneObjectImplementation.cpp`

**`initializeTransientMembers()`** — Guard ZoneProcessServer lookup:

```cpp
// BEFORE (upstream):
if (server == nullptr)
    server = Core::lookupObject<ZoneProcessServer>("ZoneProcessServer").get();

// AFTER:
if (server == nullptr && Core::MANAGED_REFERENCE_LOAD)
    server = Core::lookupObject<ZoneProcessServer>("ZoneProcessServer").get();
```

**`notifyLoadFromDatabase()`** — Early return to skip child object iteration:

```cpp
void SceneObjectImplementation::notifyLoadFromDatabase() {
    // Add at the very beginning:
    if (!Core::MANAGED_REFERENCE_LOAD) {
        return;
    }
    // ... rest unchanged
```

**Why:** ORB lookup crashes without a deployed ZoneProcessServer. Child object iteration resolves ManagedReferences and sets parents, which crashes without a running server.

### 2. `src/server/zone/objects/creature/CreatureObjectImplementation.cpp`

**`initializeTransientMembers()`** — Guard creditObject and setMood:

```cpp
// Wrap creditObject block:
if (Core::MANAGED_REFERENCE_LOAD) {
    if (creditObject == nullptr) {
        creditObject = getCreditObject();
    }

    if (creditObject != nullptr) {
        creditObject->setOwner(asCreatureObject());
    }
}

// ... later in the same method, guard setMood():
// BEFORE:
setMood(moodID);

// AFTER:
if (server != nullptr) {
    setMood(moodID);
}
```

**Why:** `getCreditObject()` calls `getSlottedObject()` which resolves ManagedReferences. `setMood()` calls `server->getChatManager()` which is nullptr in standalone mode.

### 3. `src/server/zone/objects/player/PlayerObjectImplementation.cpp`

**`initializeTransientMembers()`** — Guard account initialization:

```cpp
// BEFORE:
initializeAccount();

// AFTER:
if (Core::MANAGED_REFERENCE_LOAD) {
    initializeAccount();
}
```

**Why:** `initializeAccount()` calls `PlayerManager->getAccount()` which requires MySQL and AccountManager.

### 4. `src/server/zone/objects/cell/CellObjectImplementation.cpp`

**`notifyLoadFromDatabase()`** — Early return:

```cpp
void CellObjectImplementation::notifyLoadFromDatabase() {
    // Add at the very beginning:
    if (!Core::MANAGED_REFERENCE_LOAD) {
        return;
    }
    // ... rest unchanged
```

**Why:** Iterates container objects and resolves ManagedReferences to set parents and notify buildings.

### 5. `src/server/zone/objects/creature/ai/AiAgentImplementation.cpp`

**`notifyLoadFromDatabase()`** — Early return:

```cpp
void AiAgentImplementation::notifyLoadFromDatabase() {
    // Add at the very beginning:
    if (!Core::MANAGED_REFERENCE_LOAD) {
        return;
    }
    // ... rest unchanged
```

**Why:** `isPet()` and `controlDevice.get()` resolve ManagedReferences. `getLinkedCreature()` does the same.

### 6. `src/server/zone/objects/mission/MissionObjectImplementation.cpp`

**`initializeTransientMembers()`** — Guard waypoint creation:

```cpp
// BEFORE:
if (waypointToMission == nullptr)
    waypointToMission = createWaypoint();

// AFTER:
if (waypointToMission == nullptr && Core::MANAGED_REFERENCE_LOAD) {
    waypointToMission = createWaypoint();
}
```

**Why:** `createWaypoint()` calls `getZoneServer()->createObject()` which requires a running ZoneServer.

### 7. `src/server/zone/objects/structure/StructureObjectImplementation.cpp`

**`notifyLoadFromDatabase()`** — Guard permission migration task:

```cpp
// BEFORE:
if (permissionsFixed == false) {

// AFTER:
if (permissionsFixed == false && Core::MANAGED_REFERENCE_LOAD) {
```

**Why:** Creates a `MigratePermissionsTask` and schedules it via TaskManager, which has no worker threads in standalone mode.

---

## Category B: Data Preservation (Round-Trip Caching)

These prevent permanent data loss. When an object is deserialized in standalone mode, manager lookups (TemplateManager, CreatureTemplateManager, ZoneServer, SkillManager) return null because those managers aren't initialized. Without caching, `toBinaryStream()` writes 0 or empty strings, permanently destroying the original values.

### 8. `src/templates/TemplateReference.h`

Every SceneObject stores a `TemplateReference` pointing to its shared template. Without this fix, all template CRCs become 0 after conversion.

**Add cached CRC member:**

```cpp
template<class O> class TemplateReference : public Reference<O> {
    // ... existing members ...
    uint32 cachedCRC = 0;  // <-- add this
```

**Propagate in copy constructors and operator=:**

```cpp
// In each copy constructor and operator=, add:
cachedCRC = ref.cachedCRC;
```

**`toBinaryStream()` — write cached CRC when object is null:**

```cpp
// BEFORE:
if (object != nullptr)
    stream->writeInt(object->getServerObjectCRC());
else
    stream->writeInt(0);

// AFTER:
if (object != nullptr)
    stream->writeInt(object->getServerObjectCRC());
else
    stream->writeInt(cachedCRC);
```

**`parseFromBinaryStream()` — cache CRC before lookup:**

```cpp
bool parseFromBinaryStream(ObjectInputStream* stream) {
    uint32 oid = stream->readInt();
    cachedCRC = oid;  // <-- add this line

    SharedObjectTemplate* obj = TemplateManager::instance()->getTemplate(oid);
    // ... rest unchanged
```

### 9-10. `src/server/zone/objects/creature/ai/variables/CreatureTemplateReference.h` + `.cpp`

All AiAgent creatures store a `CreatureTemplateReference`. Without this fix, all NPC/creature template names are permanently erased.

**In `.h` — add cached member:**

```cpp
class CreatureTemplateReference : public Reference<CreatureTemplate*> {
    // ... existing members ...
    String cachedTemplateName;  // <-- add this
```

**In `.cpp` — `toBinaryStream()` — write cached name when object is null:**

```cpp
// BEFORE:
if (obj != nullptr) {
    obj->getTemplateName().toBinaryStream(stream);
} else {
    stream->writeShort(0);
}

// AFTER:
if (obj != nullptr) {
    obj->getTemplateName().toBinaryStream(stream);
} else if (!cachedTemplateName.isEmpty()) {
    cachedTemplateName.toBinaryStream(stream);
} else {
    stream->writeShort(0);
}
```

**In `.cpp` — `parseFromBinaryStream()` — cache name before lookup:**

```cpp
String templateName;
templateName.parseFromBinaryStream(stream);

cachedTemplateName = templateName;  // <-- add this line

CreatureTemplate* obj = CreatureTemplateManager::instance()->getTemplate(templateName);
// ... rest unchanged
```

### 11-12. `src/server/zone/ZoneReference.h` + `.cpp`

Every object with a zone stores a `ZoneReference`. Without this fix, conversion either crashes (null ZoneServer) or erases all zone names.

**In `.h` — add cached member and accessor:**

```cpp
class ZoneReference : public ManagedReference<Zone*> {
protected:
    String cachedZoneName;  // <-- add this
public:
    // ... existing methods ...
    const String& getZoneName() const {  // <-- add this
        return cachedZoneName;
    }
```

**In `.cpp` — `toBinaryStream()` — write cached name when Zone* is null:**

```cpp
// BEFORE:
if (object != nullptr) {
    object->getZoneName().toBinaryStream(stream);
} else {
    stream->writeShort(0);
}

// AFTER:
if (object != nullptr) {
    object->getZoneName().toBinaryStream(stream);
} else {
    cachedZoneName.toBinaryStream(stream);
}
```

**In `.cpp` — `parseFromBinaryStream()` — cache name and handle null ZoneServer:**

```cpp
// BEFORE:
String zoneName;
zoneName.parseFromBinaryStream(stream);
Zone* obj = ServerCore::getZoneServer()->getZone(zoneName);

// AFTER:
cachedZoneName.parseFromBinaryStream(stream);

auto* zoneServer = ServerCore::getZoneServer();
if (zoneServer == nullptr) {
    updateObject(nullptr);
    return true;
}

Zone* obj = zoneServer->getZone(cachedZoneName);
```

**In `.cpp` — `operator=` — cache zone name on assignment:**

```cpp
Zone* ZoneReference::operator=(Zone* obj) {
    updateObject(obj);

    if (obj != nullptr)
        cachedZoneName = obj->getZoneName();
    else
        cachedZoneName = "";

    return obj;
}
```

### 13-14. `src/server/zone/objects/creature/variables/SkillList.h` + `.cpp`

Player skills are stored as a `SkillList`. Without this fix, all player skills are permanently lost during conversion.

**Needs:** `#include "engine/core/Core.h"` in `.cpp`

**In `.h` — add raw names member:**

```cpp
class SkillList : public DeltaVector<Reference<Skill*> > {
    // ... existing members ...
    Vector<String> rawSkillNames;  // <-- add this
```

**In `.cpp` — `toBinaryStream()` — write raw names if available:**

```cpp
// In the non-ODB_SERIALIZATION branch:

// BEFORE:
Vector<String> names;
getStringList(names);
names.toBinaryStream(stream);

// AFTER:
if (!rawSkillNames.isEmpty()) {
    rawSkillNames.toBinaryStream(stream);
} else {
    Vector<String> names;
    getStringList(names);
    names.toBinaryStream(stream);
}
```

**In `.cpp` — `parseFromBinaryStream()` — cache raw names in standalone mode:**

```cpp
// In the non-ODB_SERIALIZATION branch:

Vector<String> skills;
skills.parseFromBinaryStream(stream);

// BEFORE:
loadFromNames(skills);

// AFTER:
if (!Core::MANAGED_REFERENCE_LOAD) {
    rawSkillNames = skills;
} else {
    loadFromNames(skills);
}
```

### 15-16. `src/server/zone/objects/player/variables/AbilityList.h` + `.cpp`

Player abilities are stored as an `AbilityList`. Without this fix, all player abilities are permanently lost during conversion.

**Needs:** `#include "engine/core/Core.h"` in `.cpp`

**In `.h` — add raw names member:**

```cpp
class AbilityList : public DeltaVector<Ability*> {
private:
    // ... existing members ...
    Vector<String> rawAbilityNames;  // <-- add this
```

**In `.cpp` — `toBinaryStream()` — write raw names if available:**

```cpp
// In the non-ODB_SERIALIZATION branch:

// BEFORE:
Vector<String> names;
getStringList(names);
names.toBinaryStream(stream);

// AFTER:
if (!rawAbilityNames.isEmpty()) {
    rawAbilityNames.toBinaryStream(stream);
} else {
    Vector<String> names;
    getStringList(names);
    names.toBinaryStream(stream);
}
```

**In `.cpp` — `parseFromBinaryStream()` — cache raw names in standalone mode:**

```cpp
// In the non-ODB_SERIALIZATION branch:

Vector<String> abilities;
abilities.parseFromBinaryStream(stream);

// BEFORE:
loadFromNames(abilities);

// AFTER:
if (!Core::MANAGED_REFERENCE_LOAD) {
    rawAbilityNames = abilities;
} else {
    loadFromNames(abilities);
}
```

**In `.cpp` — `loadFromNames()` — null guard for ZoneServer:**

```cpp
void AbilityList::loadFromNames(Vector<String>& abilities) {
    ZoneServer* server = ServerCore::getZoneServer();

    // Add this null guard:
    if (server == nullptr) {
        return;
    }

    SkillManager* skillManager = server->getSkillManager();
    // ... rest unchanged
```

---

## Summary

| # | File | Category | What happens without it |
|---|------|----------|------------------------|
| | **Engine3** | | |
| E1 | `Core.h` / `Core.cpp` | Infrastructure | Compile error — `MANAGED_REFERENCE_LOAD` undefined |
| E2 | `ManagedReference.h` | Infrastructure | Every ManagedReference resolves during load — OOM / crash |
| | **Core3 — Crash Guards** | | |
| 1 | `SceneObjectImplementation.cpp` | Crash guard | Segfault on ORB lookup + child object iteration |
| 2 | `CreatureObjectImplementation.cpp` | Crash guard | Null deref on getCreditObject() and setMood() |
| 3 | `PlayerObjectImplementation.cpp` | Crash guard | Crash on AccountManager lookup |
| 4 | `CellObjectImplementation.cpp` | Crash guard | Crash on container object iteration |
| 5 | `AiAgentImplementation.cpp` | Crash guard | Crash on isPet() / controlDevice resolution |
| 6 | `MissionObjectImplementation.cpp` | Crash guard | Crash on createWaypoint() (no ZoneServer) |
| 7 | `StructureObjectImplementation.cpp` | Crash guard | Hang/crash scheduling task (no TaskManager workers) |
| | **Core3 — Data Preservation** | | |
| 8 | `TemplateReference.h` | Data loss | All template CRCs become 0 |
| 9-10 | `CreatureTemplateReference.h/.cpp` | Data loss | All NPC/creature template names erased |
| 11-12 | `ZoneReference.h/.cpp` | Data loss + crash | Zone names erased, null deref on ZoneServer |
| 13-14 | `SkillList.h/.cpp` | Data loss | All player skills permanently lost |
| 15-16 | `AbilityList.h/.cpp` | Data loss | All player abilities permanently lost |
