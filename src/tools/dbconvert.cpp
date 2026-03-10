/*
 * SWGEmu Database Converter — 4-Phase Pipeline
 *
 * Converts SWGEmu Berkeley DB databases between server code versions.
 * Designed for the QuadTreeEntry → TreeEntry migration but extensible
 * to any field rename or serialization change.
 *
 * Pipeline:
 *   Phase 1: clean      — Strip BDB LSNs, remove environment files
 *   Phase 2: hashfix    — Byte-level field hash replacement (~500K records/sec)
 *   Phase 3: reserialize — C++ object round-trip (classic=all, smart=selective)
 *   Phase 4: finalize   — BDB checkpoint, clean environment for core3
 *
 * Modes:
 *   classic: hashfix all records, then reserialize all records (safe fallback)
 *   smart:   hashfix all records with metadata scan, probe per class,
 *            only reserialize classes where bytes actually changed
 *
 * Phase gates enforce ordering — each phase writes a manifest and the next
 * phase refuses to run without it. Prevents running out of order.
 *
 * Usage:
 *   ./dbconvert all                  - run all phases (classic)
 *   ./dbconvert all --smart          - run all phases (smart)
 *   ./dbconvert clean                - Phase 1 only
 *   ./dbconvert hashfix              - Phase 2 only
 *   ./dbconvert hashfix --smart      - Phase 2 with metadata scan
 *   ./dbconvert reserialize          - Phase 3 classic (all records)
 *   ./dbconvert reserialize --smart  - Phase 3 smart (probe + selective)
 *   ./dbconvert finalize             - Phase 4 only
 *
 * Build:
 *   make dbconvert -j24
 */

#include "server/zone/objects/creature/CreatureObject.h"
#include "server/zone/objects/scene/SceneObject.h"
#include "server/zone/objects/building/BuildingObject.h"
#include "server/zone/objects/player/PlayerObject.h"
#include "server/zone/managers/object/ObjectManager.h"
#include "engine/db/ObjectDatabase.h"
#include "engine/db/DatabaseManager.h"
#include "conf/ConfigManager.h"
#include "engine/orb/db/DOBObjectManager.h"

#include <db.h>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <algorithm>
#include <ctime>
#include <unordered_map>
#include <cmath>
#include <cstring>

// ─── Constants ──────────────────────────────────────────────────────────────

static const char* DB_DIR = "databases";

static const int BATCH_SIZE = 500;          // DOB eviction batch
static const int PROBE_SAMPLE_SIZE = 20;    // first/last N OIDs per class for quorum

// Hash replacement table: QuadTreeEntry → TreeEntry field renames.
// These 4 fields changed hash when the class was renamed upstream.
// The field data is identical — only the name hash in the serialization header changed.
struct HashReplacement {
	uint32 oldHash;
	uint32 newHash;
	const char* name;
};

static const HashReplacement HASH_TABLE[] = {
	{ 0x2970c5d9, 0x763502f7, "coordinates" },  // QuadTreeEntry.coordinates → TreeEntry.coordinates
	{ 0x5a47d37d, 0xb1649cd1, "bounding"    },  // QuadTreeEntry.bounding → TreeEntry.bounding
	{ 0x5284d7c8, 0x256e90b3, "parent"      },  // QuadTreeEntry.parent → TreeEntry.parent
	{ 0xac3d85f4, 0xdbd7c28f, "radius"      },  // QuadTreeEntry.radius → TreeEntry.radius
};
static const int HASH_TABLE_SIZE = 4;

// Hashes for metadata extraction (smart mode)
static const uint32 HASH_CLASSNAME    = STRING_HASHCODE("_className");
static const uint32 HASH_NEW_COORD    = 0x763502f7;  // TreeEntry.coordinates
static const uint32 HASH_OLD_COORD    = 0x2970c5d9;  // QuadTreeEntry.coordinates (pre-hashfix)
static const uint32 HASH_NEW_PARENT   = 0x256e90b3;  // TreeEntry.parent
static const uint32 HASH_OLD_PARENT   = 0x5284d7c8;  // QuadTreeEntry.parent (pre-hashfix)

// Phase manifest filenames
static const char* MANIFEST_PHASE1 = ".phase1_complete";
static const char* MANIFEST_PHASE2 = ".phase2_complete";
static const char* MANIFEST_PHASE3 = ".phase3_complete";

// ─── Per-class statistics (smart mode) ──────────────────────────────────────

struct ClassInfo {
	uint64 count;
	float maxAbsX;
	float maxAbsZ;
	uint64 totalSize;
	uint64 firstOIDs[PROBE_SAMPLE_SIZE];
	uint64 lastOIDs[PROBE_SAMPLE_SIZE];
	int firstCount;      // how many firstOIDs collected (up to PROBE_SAMPLE_SIZE)
	int lastIdx;         // ring buffer write position for lastOIDs
	int lastCount;       // total written to lastOIDs (for when count < PROBE_SAMPLE_SIZE)
	bool needsReserialize;

	ClassInfo() : count(0), maxAbsX(0), maxAbsZ(0), totalSize(0),
	              firstCount(0), lastIdx(0), lastCount(0), needsReserialize(false) {
		memset(firstOIDs, 0, sizeof(firstOIDs));
		memset(lastOIDs, 0, sizeof(lastOIDs));
	}

	void addRecord(uint64 oid, float absX, float absZ, int size) {
		count++;
		totalSize += size;
		if (absX > maxAbsX) maxAbsX = absX;
		if (absZ > maxAbsZ) maxAbsZ = absZ;

		// Collect first N OIDs
		if (firstCount < PROBE_SAMPLE_SIZE) {
			firstOIDs[firstCount++] = oid;
		}

		// Ring buffer for last N OIDs
		lastOIDs[lastIdx] = oid;
		lastIdx = (lastIdx + 1) % PROBE_SAMPLE_SIZE;
		lastCount++;
	}

	// Get all probe OIDs (first + last, deduplicated)
	std::vector<uint64> getProbeOIDs() const {
		std::vector<uint64> result;

		// Add first OIDs
		for (int i = 0; i < firstCount; i++) {
			result.push_back(firstOIDs[i]);
		}

		// Add last OIDs (ring buffer order doesn't matter for probing)
		int lastActual = (lastCount < PROBE_SAMPLE_SIZE) ? lastCount : PROBE_SAMPLE_SIZE;
		for (int i = 0; i < lastActual; i++) {
			uint64 oid = lastOIDs[i];
			// Deduplicate against firstOIDs
			bool dup = false;
			for (int j = 0; j < firstCount; j++) {
				if (firstOIDs[j] == oid) { dup = true; break; }
			}
			if (!dup) result.push_back(oid);
		}

		return result;
	}
};

// ─── Error Logging ──────────────────────────────────────────────────────────

static FILE* errorLog = nullptr;

static void openErrorLog() {
	char logPath[512];
	snprintf(logPath, sizeof(logPath), "%s/dbconvert_errors.log", DB_DIR);
	errorLog = fopen(logPath, "w");
	if (errorLog) {
		time_t now = time(nullptr);
		fprintf(errorLog, "# SWGEmu Database Converter - Error Log\n");
		fprintf(errorLog, "# Started: %s", ctime(&now));
		fprintf(errorLog, "#\n");
		fflush(errorLog);
	}
}

static void logError(const char* database, uint64 oid, const char* className, const char* message) {
	printf("  ERROR [%s] OID=0x%016llx class=%s: %s\n",
		database, (unsigned long long)oid, className, message);
	if (errorLog) {
		fprintf(errorLog, "[%s] OID=0x%016llx class=%s error=%s\n",
			database, (unsigned long long)oid, className, message);
		fflush(errorLog);
	}
}

static void logSummary(const char* message) {
	if (errorLog) {
		fprintf(errorLog, "# %s\n", message);
		fflush(errorLog);
	}
}

static void closeErrorLog(uint64 totalErrors) {
	if (errorLog) {
		time_t now = time(nullptr);
		fprintf(errorLog, "#\n# Total errors: %llu\n", (unsigned long long)totalErrors);
		fprintf(errorLog, "# Finished: %s", ctime(&now));
		fclose(errorLog);
		errorLog = nullptr;
	}
}

// ─── Helpers ────────────────────────────────────────────────────────────────

// Initialize the engine3 subsystems needed for database operations.
// Sets MANAGED_REFERENCE_LOAD=false to prevent initializeTransientMembers()
// from corrupting data during standalone deserialization.
static bool engineInitialized = false;

static void initEngine() {
	if (engineInitialized) return;

	Core::MANAGED_REFERENCE_LOAD = false;
	ConfigManager::instance()->loadConfigData();
	Core::initializeProperties("Core3");

	Core::setIntProperty("BerkeleyDB.envMaxLocks", 15000000);
	Core::setIntProperty("BerkeleyDB.envMaxLockers", 15000000);
	Core::setIntProperty("BerkeleyDB.envMaxLockObjects", 15000000);

	// Disable update threads — dbconvert is single-threaded, writes directly to BDB
	Core::setIntProperty("ObjectManager.initialUpdateModifiedObjectsThreads", 0);

	// Open BDB environment and load database registry (databases.db)
	ObjectDatabaseManager::instance()->loadDatabases(false);

	engineInitialized = true;
}

// Format elapsed time as "Xh Ym Zs"
static void formatElapsed(double seconds, char* buf, size_t bufSize) {
	int h = (int)seconds / 3600;
	int m = ((int)seconds % 3600) / 60;
	int s = (int)seconds % 60;
	if (h > 0)
		snprintf(buf, bufSize, "%dh %dm %ds", h, m, s);
	else if (m > 0)
		snprintf(buf, bufSize, "%dm %ds", m, s);
	else
		snprintf(buf, bufSize, "%ds", s);
}

// Format ETA from remaining count and rate
static void formatETA(uint64 remaining, double rate, char* buf, size_t bufSize) {
	if (rate <= 0) { snprintf(buf, bufSize, "?"); return; }
	uint64 etaSec = (uint64)(remaining / rate);
	formatElapsed((double)etaSec, buf, bufSize);
}

// Extract _className from raw serialized bytes (no database read needed)
static String extractClassNameFromBytes(const char* buf, int bufSize) {
	if (bufSize < 2) return "unknown";

	try {
		ObjectInputStream data;
		data.writeStream(buf, bufSize);
		data.reset();

		uint16 varCount = data.readShort();

		for (int v = 0; v < varCount; v++) {
			if (data.getOffset() + 8 > data.size()) break;

			uint32 nameHash = data.readInt();
			uint32 varSize = data.readInt();

			if ((uint32)data.getOffset() + varSize > (uint32)data.size()) break;

			if (nameHash == HASH_CLASSNAME) {
				String val;
				val.parseFromBinaryStream(&data);
				return val;
			}

			data.shiftOffset(varSize);
		}
	} catch (...) {}

	return "unknown";
}

// Extract _className by reading from BDB (for error reporting during reserialize)
static String extractClassName(ObjectDatabase* database, uint64 oid) {
	try {
		ObjectInputStream data;
		if (database->getData(oid, &data) != 0) return "unknown";
		return extractClassNameFromBytes(data.getBuffer(), data.size());
	} catch (...) {}
	return "unknown";
}

// ─── Phase Manifest Helpers ─────────────────────────────────────────────────

static String manifestPath(const char* manifestName) {
	return String(DB_DIR) + "/" + manifestName;
}

static bool phaseComplete(const char* manifestName) {
	String path = manifestPath(manifestName);
	FILE* f = fopen(path.toCharArray(), "r");
	if (f) { fclose(f); return true; }
	return false;
}

static void writeManifest(const char* manifestName, const char* content) {
	String path = manifestPath(manifestName);
	FILE* f = fopen(path.toCharArray(), "w");
	if (f) {
		time_t now = time(nullptr);
		fprintf(f, "completed=%ld\n", (long)now);
		if (content) fprintf(f, "%s", content);
		fclose(f);
	}
}

static void removeManifest(const char* manifestName) {
	String path = manifestPath(manifestName);
	remove(path.toCharArray());
}

static bool requirePhase(const char* manifestName, const char* phaseName) {
	if (!phaseComplete(manifestName)) {
		System::out << "ERROR: " << phaseName << " has not been completed." << endl;
		System::out << "Run the required phase first, or use './dbconvert all' to run the full pipeline." << endl;
		return false;
	}
	return true;
}

// ─── Database Discovery ─────────────────────────────────────────────────────
// Scan databases/ for .db files, skip internal and index databases, sort by size

struct DbEntry {
	String name;
	off_t size;
};

static std::vector<DbEntry> discoverDatabases() {
	std::vector<DbEntry> result;

	DIR* dir = opendir(DB_DIR);
	if (!dir) {
		System::out << "ERROR: Could not open " << DB_DIR << "/ directory" << endl;
		return result;
	}

	struct dirent* entry;
	while ((entry = readdir(dir)) != nullptr) {
		String filename = entry->d_name;
		if (!filename.endsWith(".db") || filename.length() <= 3)
			continue;
		String name = filename.subString(0, filename.length() - 3);
		// Skip BDB internal files, index databases, and ephemeral databases
		// (ephemeral ones are deleted in Phase 1, but skip if still present)
		if (name.beginsWith("__") || name.contains("index") || name == "databases")
			continue;
		if (name == "clientobjects" || name == "navareas" || name == "buffs")
			continue;

		struct stat st;
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", DB_DIR, filename.toCharArray());
		off_t fileSize = 0;
		if (stat(path, &st) == 0) fileSize = st.st_size;
		result.push_back({name, fileSize});
	}
	closedir(dir);

	// Sort smallest first for quick early progress
	std::sort(result.begin(), result.end(), [](const DbEntry& a, const DbEntry& b) {
		return a.size < b.size;
	});

	return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// PHASE 1: CLEAN
// Strip BDB LSNs, remove environment files.
// Prepares databases for a fresh BDB environment.
// ═════════════════════════════════════════════════════════════════════════════

static int phaseClean() {
	System::out << "================================================================" << endl;
	System::out << "  Phase 1: Clean BDB Environment" << endl;
	System::out << "================================================================" << endl << endl;

	DIR* dir = opendir(DB_DIR);
	if (!dir) {
		System::out << "ERROR: Could not open " << DB_DIR << "/ directory" << endl;
		return 1;
	}

	// Collect file lists
	Vector<String> envFiles;
	Vector<String> logFiles;
	Vector<String> dbFiles;
	struct dirent* entry;

	// Ephemeral databases rebuilt by the server at boot — delete before any processing
	static const char* EPHEMERAL_DBS[] = { "clientobjects", "navareas", "buffs", nullptr };

	while ((entry = readdir(dir)) != nullptr) {
		String filename = entry->d_name;
		if (filename == "." || filename == "..") continue;
		if (filename.beginsWith("__db."))
			envFiles.add(filename);
		else if (filename.beginsWith("log."))
			logFiles.add(filename);
		else if (filename.endsWith(".db")) {
			String name = filename.subString(0, filename.length() - 3);
			bool ephemeral = false;
			for (int e = 0; EPHEMERAL_DBS[e]; e++) {
				if (name == EPHEMERAL_DBS[e]) {
					ephemeral = true;
					break;
				}
			}
			if (ephemeral) {
				char delPath[512];
				snprintf(delPath, sizeof(delPath), "%s/%s", DB_DIR, filename.toCharArray());
				if (remove(delPath) == 0)
					System::out << "  Deleted " << name << ".db (ephemeral, rebuilt at boot)" << endl;
			} else {
				dbFiles.add(filename);
			}
		}
	}
	closedir(dir);

	// If transaction logs exist, replay them before removal.
	// Data written via ObjectDatabaseManager (guilds, chatrooms, etc.) may
	// only exist in log files if the writer never checkpointed. Opening with
	// DB_RECOVER replays the logs into the .db pages, then we checkpoint to
	// flush them to disk. Without this, removing log.* destroys that data.
	if (logFiles.size() > 0) {
		System::out << "  Found " << logFiles.size() << " transaction log(s) — recovering before cleanup..." << endl;

		DB_ENV* recEnv = nullptr;
		int recRet = db_env_create(&recEnv, 0);
		if (recRet == 0) {
			recRet = recEnv->open(recEnv, DB_DIR,
				DB_CREATE | DB_INIT_MPOOL | DB_INIT_LOG | DB_INIT_TXN | DB_RECOVER, 0);
			if (recRet == 0) {
				recEnv->txn_checkpoint(recEnv, 0, 0, DB_FORCE);
				System::out << "  Recovery checkpoint complete (dirty pages flushed)" << endl;
				recEnv->close(recEnv, 0);
			} else {
				System::out << "  WARNING: Could not open environment for recovery (ret=" << recRet << ")" << endl;
				System::out << "  Proceeding with cleanup — verify small databases (guilds, etc.) after conversion" << endl;
				recEnv->close(recEnv, 0);
			}
		}
	}

	// Remove environment and log files
	int removedCount = 0;
	for (int i = 0; i < envFiles.size(); ++i) {
		String fullPath = String(DB_DIR) + "/" + envFiles.get(i);
		remove(fullPath.toCharArray());
		removedCount++;
	}
	for (int i = 0; i < logFiles.size(); ++i) {
		String fullPath = String(DB_DIR) + "/" + logFiles.get(i);
		remove(fullPath.toCharArray());
		removedCount++;
	}
	System::out << "  Removed " << removedCount << " environment files (__db.*, log.*)" << endl;

	// Reset LSNs in all .db files using a private environment
	DB_ENV* dbenv = nullptr;
	int ret = db_env_create(&dbenv, 0);
	if (ret == 0) {
		ret = dbenv->open(dbenv, DB_DIR, DB_CREATE | DB_INIT_MPOOL | DB_PRIVATE, 0);
		if (ret == 0) {
			for (int i = 0; i < dbFiles.size(); ++i) {
				dbenv->lsn_reset(dbenv, dbFiles.get(i).toCharArray(), 0);
			}
			dbenv->close(dbenv, 0);
			System::out << "  Reset LSNs in " << dbFiles.size() << " databases" << endl;
		} else {
			System::out << "  WARNING: Could not open BDB environment for LSN reset" << endl;
			dbenv->close(dbenv, 0);
		}
	}

	// Remove any leftover __db.* from the LSN reset
	dir = opendir(DB_DIR);
	if (dir) {
		while ((entry = readdir(dir)) != nullptr) {
			String filename = entry->d_name;
			if (filename.beginsWith("__db.")) {
				String fullPath = String(DB_DIR) + "/" + filename;
				remove(fullPath.toCharArray());
			}
		}
		closedir(dir);
	}

	// Remove stale phase manifests from previous runs
	removeManifest(MANIFEST_PHASE1);
	removeManifest(MANIFEST_PHASE2);
	removeManifest(MANIFEST_PHASE3);

	// Remove stale per-database .converted manifests
	// NOTE: Can't use String::endsWith() due to a bug in engine3 where it returns
	// true when the suffix is longer than the string (both produce -1 comparison).
	// Use std::string instead for reliable suffix matching.
	dir = opendir(DB_DIR);
	if (dir) {
		while ((entry = readdir(dir)) != nullptr) {
			std::string filename(entry->d_name);
			const std::string suffix = ".converted";
			bool endsWithConverted = filename.size() >= suffix.size() &&
				filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0;
			if (endsWithConverted) {
				String fullPath = String(DB_DIR) + "/" + entry->d_name;
				remove(fullPath.toCharArray());
			}
		}
		closedir(dir);
	}

	writeManifest(MANIFEST_PHASE1, nullptr);

	System::out << endl << "  Phase 1 complete. Wrote " << MANIFEST_PHASE1 << endl;
	return 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// PHASE 2: HASHFIX
// Byte-level hash replacement for renamed fields.
// In smart mode, also extracts per-class metadata for Phase 3 probing.
// ═════════════════════════════════════════════════════════════════════════════

static int phaseHashfix(bool smartMode,
	std::unordered_map<std::string, std::unordered_map<std::string, ClassInfo>>* allClassMaps) {

	if (!requirePhase(MANIFEST_PHASE1, "Phase 1 (clean)")) return 1;

	System::out << "================================================================" << endl;
	System::out << "  Phase 2: Hash Fix (QuadTreeEntry -> TreeEntry)" << endl;
	if (smartMode)
		System::out << "  Mode:    Smart (collecting per-class metadata)" << endl;
	System::out << "================================================================" << endl << endl;

	// Initialize engine for database access
	initEngine();
	ObjectDatabaseManager* dbManager = ObjectDatabaseManager::instance();

	auto databases = discoverDatabases();
	if (databases.empty()) {
		System::out << "No databases found." << endl;
		return 1;
	}

	System::out << databases.size() << " databases to process" << endl << endl;

	Time overallStart;
	uint64 grandRecords = 0, grandFixed = 0;

	for (size_t dbIdx = 0; dbIdx < databases.size(); dbIdx++) {
		String currentDb = databases[dbIdx].name;

		System::out << "  [" << (dbIdx + 1) << "/" << databases.size() << "] " << currentDb << endl;

		ObjectDatabase* database = dbManager->loadObjectDatabase(currentDb, true);
		if (database == nullptr) {
			System::out << "    ERROR: Could not load database" << endl;
			continue;
		}

		// Per-database class map (smart mode)
		std::unordered_map<std::string, ClassInfo>* classMap = nullptr;
		if (smartMode && allClassMaps != nullptr) {
			classMap = &(*allClassMaps)[std::string(currentDb.toCharArray())];
		}

		uint64 records = 0, modified = 0;
		Time dbStart;

		{
			auto* txn = ObjectDatabaseManager::instance()->startTransaction();
			LocalDatabaseIterator iterator(txn, database);
			ObjectInputStream key;
			ObjectInputStream data;

			while (iterator.getNextKeyAndValue(&key, &data)) {
				records++;

				int dataSize = data.size();
				if (dataSize < 2) {
					key.clear();
					data.clear();
					continue;
				}

				// Extract OID from key for smart mode
				uint64 oid = 0;
				if (key.size() >= 8) {
					memcpy(&oid, key.getBuffer(), sizeof(uint64));
				}

				// Copy data for potential modification
				ObjectOutputStream output(dataSize);
				const char* buffer = data.getBuffer();
				output.writeStream(buffer, dataSize);

				// Parse field headers
				data.reset();
				uint16 varCount = data.readShort();
				bool hashModified = false;

				// Smart mode metadata
				String className;
				float posX = 0, posZ = 0;
				bool hasClassName = false;

				for (int v = 0; v < varCount; v++) {
					int hashOffset = data.getOffset();

					if (hashOffset + 8 > dataSize) break;

					uint32 nameHash = data.readInt();
					uint32 varSize = data.readInt();
					int fieldDataOffset = data.getOffset();

					if ((uint32)fieldDataOffset + varSize > (uint32)dataSize) break;

					// Hash replacement
					for (int h = 0; h < HASH_TABLE_SIZE; h++) {
						if (nameHash == HASH_TABLE[h].oldHash) {
							char* outBuf = (char*)output.getBuffer();
							uint32 newHash = HASH_TABLE[h].newHash;
							memcpy(outBuf + hashOffset, &newHash, sizeof(newHash));
							hashModified = true;
							break;
						}
					}

					// Smart mode: extract metadata from field data
					if (smartMode && classMap != nullptr) {
						if (nameHash == HASH_CLASSNAME) {
							int savedOffset = data.getOffset();
							String val;
							val.parseFromBinaryStream(&data);
							className = val;
							hasClassName = true;
							data.setOffset(fieldDataOffset);
						}

						if ((nameHash == HASH_NEW_COORD || nameHash == HASH_OLD_COORD) && varSize >= 8) {
							memcpy(&posX, buffer + fieldDataOffset, sizeof(float));
							memcpy(&posZ, buffer + fieldDataOffset + 4, sizeof(float));
						}
					}

					data.shiftOffset(varSize);
				}

				// Write back if hashes were modified
				if (hashModified) {
					modified++;
					ObjectOutputStream* outputCopy = new ObjectOutputStream(output.size());
					outputCopy->writeStream(output.getBuffer(), output.size());
					iterator.putCurrent(outputCopy);
				}

				// Update class map (smart mode)
				if (smartMode && classMap != nullptr && hasClassName) {
					std::string classKey(className.toCharArray());
					(*classMap)[classKey].addRecord(oid, fabs(posX), fabs(posZ), dataSize);
				}

				// Progress
				if (records % 50000 == 0) {
					double elapsed = dbStart.miliDifference() / 1000.0;
					double rate = elapsed > 0 ? records / elapsed : 0;
					printf("\r    %llu records, %llu fixed, %.0f/s   ",
						(unsigned long long)records, (unsigned long long)modified, rate);
					fflush(stdout);
				}

				key.clear();
				data.clear();
			}

			iterator.closeCursor();
			ObjectDatabaseManager::instance()->commitTransaction(txn);
		}

		double dbElapsed = dbStart.miliDifference() / 1000.0;
		printf("\r    %llu records, %llu fixed in %.0fs",
			(unsigned long long)records, (unsigned long long)modified, dbElapsed);

		if (smartMode && classMap != nullptr) {
			printf(" (%d classes)", (int)classMap->size());
		}
		printf("                    \n");

		grandRecords += records;
		grandFixed += modified;
	}

	// Summary
	double totalElapsed = overallStart.miliDifference() / 1000.0;
	char elapsed[64];
	formatElapsed(totalElapsed, elapsed, sizeof(elapsed));

	System::out << endl;
	System::out << "  Hash fix complete" << endl;
	System::out << "  Records:     " << grandRecords << endl;
	System::out << "  Fixed:       " << grandFixed << endl;
	System::out << "  Elapsed:     " << elapsed << endl;

	// Write manifest with stats
	char manifestContent[512];
	snprintf(manifestContent, sizeof(manifestContent),
		"records=%llu\nfixed=%llu\nsmart=%d\n",
		(unsigned long long)grandRecords, (unsigned long long)grandFixed, smartMode ? 1 : 0);
	writeManifest(MANIFEST_PHASE2, manifestContent);

	System::out << "  Wrote " << MANIFEST_PHASE2 << endl;
	return 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// PHASE 3: RESERIALIZE
// C++ object round-trip. Classic mode converts all records.
// Smart mode probes per class and only converts classes where bytes differ.
// ═════════════════════════════════════════════════════════════════════════════

// Probe a single OID: load, reserialize, compare with current BDB bytes.
// Returns true if the reserialized bytes differ from stored bytes.
static bool probeRecord(ObjectDatabase* database, ObjectManager* objManager, uint64 oid) {
	try {
		// Read current bytes from BDB
		ObjectInputStream currentData;
		if (database->getData(oid, &currentData) != 0)
			return false;  // can't read = can't probe, treat as clean

		// Load through C++ deserialization
		Reference<DistributedObjectStub*> object = objManager->loadPersistentObject(oid, true);
		if (object == nullptr)
			return false;  // unregistered class, can't reserialize

		ManagedObject* managed = dynamic_cast<ManagedObject*>(object.get());
		if (managed == nullptr) {
			// Clean up DOB
			DistributedObjectAdapter* adapter = objManager->removeObject(oid);
			if (adapter) delete adapter;
			return false;
		}

		// Reserialize
		ObjectOutputStream newData(8192, 0);
		managed->writeObject(&newData);

		// Clean up DOB
		DistributedObjectAdapter* adapter = objManager->removeObject(oid);
		if (adapter) delete adapter;

		// Compare
		if ((int)newData.size() != currentData.size())
			return true;  // different size = definitely changed

		return memcmp(newData.getBuffer(), currentData.getBuffer(), currentData.size()) != 0;

	} catch (...) {
		// Clean up DOB on exception
		DistributedObjectAdapter* adapter = objManager->removeObject(oid);
		if (adapter) delete adapter;
		return false;  // probe failure = treat as clean (conservative)
	}
}

static int phaseReserialize(bool smartMode,
	std::unordered_map<std::string, std::unordered_map<std::string, ClassInfo>>* allClassMaps) {

	if (!requirePhase(MANIFEST_PHASE2, "Phase 2 (hashfix)")) return 1;

	System::out << "================================================================" << endl;
	System::out << "  Phase 3: Reserialize (C++ round-trip)" << endl;
	if (smartMode)
		System::out << "  Mode:    Smart (quorum probe, selective reserialize)" << endl;
	else
		System::out << "  Mode:    Classic (all records)" << endl;
	System::out << "================================================================" << endl << endl;

	// Initialize engine if not already done
	initEngine();
	ObjectManager* objManager = ObjectManager::instance();
	ObjectDatabaseManager* dbManager = ObjectDatabaseManager::instance();

	openErrorLog();

	auto databases = discoverDatabases();
	if (databases.empty()) {
		System::out << "No databases found." << endl;
		return 1;
	}

	Time overallStart;
	uint64 grandReserialized = 0, grandSkipped = 0, grandErrors = 0;

	for (size_t dbIdx = 0; dbIdx < databases.size(); dbIdx++) {
		String currentDb = databases[dbIdx].name;

		// Check if already converted (resume support)
		String convertedPath = String(DB_DIR) + "/" + currentDb + ".converted";
		FILE* check = fopen(convertedPath.toCharArray(), "r");
		if (check != nullptr) {
			fclose(check);
			double mb = databases[dbIdx].size / (1024.0 * 1024.0);
			System::out << "  [SKIP] " << currentDb << " (" << (int)mb << " MB, already converted)" << endl;
			continue;
		}

		System::out << "================================================================" << endl;
		System::out << "  [" << (dbIdx + 1) << "/" << databases.size() << "] " << currentDb << endl;
		System::out << "================================================================" << endl;

		ObjectDatabase* database = dbManager->loadObjectDatabase(currentDb, true);
		if (database == nullptr) {
			System::out << "  ERROR: Could not load database" << endl;
			logError(currentDb.toCharArray(), 0, "N/A", "Could not load database");
			grandErrors++;
			continue;
		}

		// ── Smart mode: quorum probe (sceneobjects only) ──
		// Smart mode probing only makes sense for sceneobjects where skipping
		// unchanged classes saves significant time (90%+ of 1M+ records).
		// Small databases like guilds, chatrooms, etc. are always fully
		// reserialized — the probe's error handling (treat failures as CLEAN)
		// can silently skip records that actually need conversion.
		std::unordered_map<std::string, bool> classNeedsReserialize;

		if (smartMode && allClassMaps != nullptr && currentDb == "sceneobjects") {
			auto dbMapIt = allClassMaps->find(std::string(currentDb.toCharArray()));
			if (dbMapIt != allClassMaps->end()) {
				auto& classMap = dbMapIt->second;

				System::out << "  Probing " << classMap.size() << " classes (up to "
					<< (PROBE_SAMPLE_SIZE * 2) << " records each)..." << endl;

				uint64 totalRecords = 0;
				uint64 reserializeRecords = 0;

				for (auto& kv : classMap) {
					ClassInfo& info = kv.second;
					std::vector<uint64> probeOIDs = info.getProbeOIDs();
					bool needsIt = false;

					for (uint64 probeOID : probeOIDs) {
						if (probeRecord(database, objManager, probeOID)) {
							needsIt = true;
							break;  // one mismatch = whole class needs reserialize
						}
					}

					info.needsReserialize = needsIt;
					classNeedsReserialize[kv.first] = needsIt;
					totalRecords += info.count;
					if (needsIt) reserializeRecords += info.count;

					// Print class report line
					printf("    %-30s %8llu  maxCoord=%-8.1f  %s\n",
						kv.first.c_str(),
						(unsigned long long)info.count,
						(info.maxAbsX > info.maxAbsZ) ? info.maxAbsX : info.maxAbsZ,
						needsIt ? "RESERIALIZE" : "CLEAN");
				}

				System::out << "  ──────────────────────────────────────────────" << endl;
				if (totalRecords > 0) {
					printf("    Total to reserialize: %llu / %llu (%.1f%%)\n",
						(unsigned long long)reserializeRecords,
						(unsigned long long)totalRecords,
						100.0 * reserializeRecords / totalRecords);
				}
				System::out << endl;

				if (reserializeRecords == 0) {
					System::out << "  All classes clean — skipping reserialize for " << currentDb << endl;
					grandSkipped += totalRecords;

					// Write converted manifest
					String manifestPath = String(DB_DIR) + "/" + currentDb + ".converted";
					FILE* manifest = fopen(manifestPath.toCharArray(), "w");
					if (manifest) {
						Time now;
						fprintf(manifest, "converted=%llu\nreserialized=0\nskipped=%llu\nmode=smart\n",
							(unsigned long long)now.getMiliTime(), (unsigned long long)totalRecords);
						fclose(manifest);
					}
					System::out << endl;
					continue;
				}
			}
		}

		// ── Collect keys ──
		System::out << "  Collecting keys..." << endl;
		std::vector<uint64> allKeys;
		{
			ObjectDatabaseIterator keyIter(database);
			uint64 k;
			while (keyIter.getNextKey(k))
				allKeys.push_back(k);
		}

		uint64 totalObjects = allKeys.size();
		System::out << "  " << totalObjects << " records to process" << endl;

		// ── Reserialize ──
		uint64 converted = 0, skipped = 0, errors = 0;
		Time reserStart;
		Vector<uint64> batchOids;

		for (size_t ki = 0; ki < allKeys.size(); ki++) {
			uint64 oid = allKeys[ki];

			// Smart mode: check if this record's class needs reserialize
			if (smartMode && !classNeedsReserialize.empty()) {
				String className = extractClassName(database, oid);
				std::string classKey(className.toCharArray());
				auto it = classNeedsReserialize.find(classKey);
				if (it != classNeedsReserialize.end() && !it->second) {
					skipped++;
					// Progress (less frequent for skipped records)
					if (skipped % 10000 == 0) {
						uint64 processed = converted + skipped + errors;
						double elapsed = reserStart.miliDifference() / 1000.0;
						double rate = elapsed > 0 ? processed / elapsed : 0;
						char etaBuf[64];
						formatETA(totalObjects - processed, rate, etaBuf, sizeof(etaBuf));
						printf("\r    %llu/%llu :: %llu converted :: %llu skipped :: %llu errors :: ETA %s          ",
							(unsigned long long)processed, (unsigned long long)totalObjects,
							(unsigned long long)converted, (unsigned long long)skipped,
							(unsigned long long)errors, etaBuf);
						fflush(stdout);
					}
					continue;
				}
			}

			// Reserialize this record
			try {
				Reference<DistributedObjectStub*> object = objManager->loadPersistentObject(oid, true);

				if (object != nullptr) {
					ManagedObject* managed = dynamic_cast<ManagedObject*>(object.get());
					if (managed != nullptr) {
						ObjectOutputStream* objectData = new ObjectOutputStream(8192, 0);
						managed->writeObject(objectData);
						database->putData(oid, objectData, nullptr);
					}
					batchOids.add(oid);
					converted++;
				} else {
					String className = extractClassName(database, oid);
					logError(currentDb.toCharArray(), oid, className.toCharArray(),
						"loadPersistentObject returned null (unregistered class?)");
					errors++;
				}
			} catch (const Exception& e) {
				String className = extractClassName(database, oid);
				StringBuffer msg;
				msg << e.getMessage();
				logError(currentDb.toCharArray(), oid, className.toCharArray(),
					msg.toString().toCharArray());
				errors++;
			} catch (...) {
				String className = extractClassName(database, oid);
				logError(currentDb.toCharArray(), oid, className.toCharArray(),
					"unknown exception (possible corrupt data)");
				errors++;
			}

			// Batch commit + DOB eviction
			if (batchOids.size() >= BATCH_SIZE) {
				ObjectDatabaseManager::instance()->commitLocalTransaction();

				for (int b = 0; b < batchOids.size(); ++b) {
					DistributedObjectAdapter* adapter = objManager->removeObject(batchOids.get(b));
					if (adapter != nullptr)
						delete adapter;
				}
				batchOids.removeAll();
			}

			// Progress
			uint64 processed = converted + skipped + errors;
			if (processed % 500 == 0 || processed == totalObjects) {
				double elapsed = reserStart.miliDifference() / 1000.0;
				double rate = elapsed > 0 ? processed / elapsed : 0;
				char etaBuf[64];
				formatETA(totalObjects - processed, rate, etaBuf, sizeof(etaBuf));

				if (smartMode) {
					printf("\r    %llu/%llu :: %llu converted :: %llu skipped :: %llu errors :: ETA %s          ",
						(unsigned long long)processed, (unsigned long long)totalObjects,
						(unsigned long long)converted, (unsigned long long)skipped,
						(unsigned long long)errors, etaBuf);
				} else {
					printf("\r    %llu/%llu :: %.0f/s :: %llu errors :: ETA %s          ",
						(unsigned long long)(converted + errors), (unsigned long long)totalObjects,
						rate, (unsigned long long)errors, etaBuf);
				}
				fflush(stdout);
			}
		}

		// Final batch
		if (batchOids.size() > 0) {
			ObjectDatabaseManager::instance()->commitLocalTransaction();
			for (int b = 0; b < batchOids.size(); ++b) {
				DistributedObjectAdapter* adapter = objManager->removeObject(batchOids.get(b));
				if (adapter != nullptr)
					delete adapter;
			}
			batchOids.removeAll();
		}

		double reserElapsed = reserStart.miliDifference() / 1000.0;
		char elapsedBuf[64];
		formatElapsed(reserElapsed, elapsedBuf, sizeof(elapsedBuf));

		printf("\r    %llu converted, %llu skipped, %llu errors in %s                              \n",
			(unsigned long long)converted, (unsigned long long)skipped,
			(unsigned long long)errors, elapsedBuf);

		grandReserialized += converted;
		grandSkipped += skipped;
		grandErrors += errors;

		// Log summary
		{
			char buf[256];
			snprintf(buf, sizeof(buf), "[%s] reserialized=%llu skipped=%llu errors=%llu",
				currentDb.toCharArray(), (unsigned long long)converted,
				(unsigned long long)skipped, (unsigned long long)errors);
			logSummary(buf);
		}

		// Write per-database converted manifest
		{
			String mPath = String(DB_DIR) + "/" + currentDb + ".converted";
			FILE* manifest = fopen(mPath.toCharArray(), "w");
			if (manifest) {
				Time now;
				fprintf(manifest, "converted=%llu\nreserialized=%llu\nskipped=%llu\nerrors=%llu\nmode=%s\n",
					(unsigned long long)now.getMiliTime(),
					(unsigned long long)converted, (unsigned long long)skipped,
					(unsigned long long)errors, smartMode ? "smart" : "classic");
				fclose(manifest);
			}
		}

		System::out << endl;
	}

	// Overall summary
	double totalElapsed = overallStart.miliDifference() / 1000.0;
	char elapsed[64];
	formatElapsed(totalElapsed, elapsed, sizeof(elapsed));

	System::out << "================================================================" << endl;
	System::out << "  Reserialize complete" << endl;
	System::out << "  Converted:   " << grandReserialized << endl;
	System::out << "  Skipped:     " << grandSkipped << endl;
	System::out << "  Errors:      " << grandErrors << endl;
	System::out << "  Elapsed:     " << elapsed << endl;
	if (grandErrors > 0)
		System::out << "  Error log:   " << DB_DIR << "/dbconvert_errors.log" << endl;
	System::out << "================================================================" << endl;

	closeErrorLog(grandErrors);
	// Note: Do NOT call objManager->shutdown() here — Phase 4 needs
	// the ObjectDatabaseManager alive for checkpoint. _exit() in main
	// handles cleanup.

	// Write phase 3 manifest
	char manifestContent[512];
	snprintf(manifestContent, sizeof(manifestContent),
		"reserialized=%llu\nskipped=%llu\nerrors=%llu\nmode=%s\n",
		(unsigned long long)grandReserialized, (unsigned long long)grandSkipped,
		(unsigned long long)grandErrors, smartMode ? "smart" : "classic");
	writeManifest(MANIFEST_PHASE3, manifestContent);

	System::out << "  Wrote " << MANIFEST_PHASE3 << endl;
	return 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// PHASE 4: FINALIZE
// BDB checkpoint to flush dirty pages, then clean environment files.
// Does NOT remove log.* — core3's DB_RECOVER handles log reconciliation.
// ═════════════════════════════════════════════════════════════════════════════

static int phaseFinalize() {
	if (!requirePhase(MANIFEST_PHASE3, "Phase 3 (reserialize)")) return 1;

	System::out << "================================================================" << endl;
	System::out << "  Phase 4: Finalize (checkpoint + cleanup)" << endl;
	System::out << "================================================================" << endl << endl;

	// Initialize engine — this opens the BDB environment with full
	// transaction support (DB_INIT_LOG | DB_INIT_TXN | DB_RECOVER).
	// The environment must be open to checkpoint.
	initEngine();

	// Force the ObjectDatabaseManager to open its environment
	ObjectDatabaseManager* dbManager = ObjectDatabaseManager::instance();

	// Load at least one database to ensure the environment is fully initialized
	auto databases = discoverDatabases();
	if (!databases.empty()) {
		dbManager->loadObjectDatabase(databases[0].name, true);
	}

	// Checkpoint: flush all dirty pages from buffer pool to .db files.
	// After this, all data pages are self-consistent on disk.
	System::out << "  Forcing BDB checkpoint..." << endl;
	try {
		ObjectDatabaseManager::instance()->checkpoint();
		System::out << "  Checkpoint complete (all dirty pages flushed)" << endl;
	} catch (const Exception& e) {
		System::out << "  WARNING: Checkpoint failed: " << e.getMessage() << endl;
		System::out << "  Continuing with cleanup..." << endl;
	} catch (...) {
		System::out << "  WARNING: Checkpoint failed (unknown error)" << endl;
		System::out << "  Continuing with cleanup..." << endl;
	}

	// Note: We do NOT remove log.* files here.
	// The .db files contain LSNs that reference the log files. Removing logs
	// before core3 sees them causes "LSN past end of log" errors.
	// core3 opens with DB_RECOVER which reconciles the log state on startup.
	System::out << "  Log files preserved (core3 handles recovery on startup)" << endl;

	// Remove __db.* shared memory regions (per-process, always recreated)
	System::out << "  Removing shared memory regions (__db.*)..." << endl;
	DIR* dir = opendir(DB_DIR);
	if (dir) {
		struct dirent* entry;
		int removed = 0;
		while ((entry = readdir(dir)) != nullptr) {
			String filename = entry->d_name;
			if (filename.beginsWith("__db.")) {
				String fullPath = String(DB_DIR) + "/" + filename;
				remove(fullPath.toCharArray());
				removed++;
			}
		}
		closedir(dir);
		System::out << "  Removed " << removed << " __db.* files" << endl;
	}

	// Remove phase manifests (conversion is complete)
	removeManifest(MANIFEST_PHASE1);
	removeManifest(MANIFEST_PHASE2);
	removeManifest(MANIFEST_PHASE3);

	// Remove per-database .converted manifests
	// NOTE: Can't use String::endsWith() due to a bug in engine3 where it returns
	// true when the suffix is longer than the string (both produce -1 comparison).
	// Use std::string instead for reliable suffix matching.
	dir = opendir(DB_DIR);
	if (dir) {
		struct dirent* entry;
		while ((entry = readdir(dir)) != nullptr) {
			std::string filename(entry->d_name);
			const std::string suffix = ".converted";
			bool endsWithConverted = filename.size() >= suffix.size() &&
				filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0;
			if (endsWithConverted) {
				String fullPath = String(DB_DIR) + "/" + entry->d_name;
				remove(fullPath.toCharArray());
			}
		}
		closedir(dir);
	}

	System::out << endl;
	System::out << "  Phase 4 complete. Databases are ready for core3." << endl;
	System::out << "  Run './core3' to start the server." << endl;
	return 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// MAIN
// ═════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
	System::setStreamBuffer(stdout, nullptr);

	if (argc) {
		StackTrace::setBinaryName(argv[0]);
	}

	// Parse arguments
	String subCmd;
	bool smartMode = false;

	for (int i = 1; i < argc; ++i) {
		String arg = argv[i];
		if (arg == "--smart" || arg == "-s") {
			smartMode = true;
		} else if (subCmd.isEmpty()) {
			subCmd = arg.toLowerCase();
		}
	}

	if (subCmd.isEmpty()) {
		System::out << "SWGEmu Database Converter — 4-Phase Pipeline" << endl;
		System::out << endl;
		System::out << "Full pipeline:" << endl;
		System::out << "  ./dbconvert all                  Run all phases (classic)" << endl;
		System::out << "  ./dbconvert all --smart          Run all phases (smart, selective reserialize)" << endl;
		System::out << endl;
		System::out << "Individual phases (must run in order):" << endl;
		System::out << "  ./dbconvert clean                Phase 1: Strip BDB LSNs, clean environment" << endl;
		System::out << "  ./dbconvert hashfix              Phase 2: Byte-level hash replacement" << endl;
		System::out << "  ./dbconvert hashfix --smart      Phase 2: Hash replacement + metadata scan" << endl;
		System::out << "  ./dbconvert reserialize          Phase 3: C++ round-trip (all records)" << endl;
		System::out << "  ./dbconvert reserialize --smart  Phase 3: C++ round-trip (selective)" << endl;
		System::out << "  ./dbconvert finalize             Phase 4: BDB checkpoint, prepare for core3" << endl;
		System::out << endl;
		System::out << "Phase gates: each phase requires the previous phase to be complete." << endl;
		System::out << "Use 'all' to run the full pipeline automatically." << endl;
		System::out << endl;
		System::out << "The --smart flag enables per-class probing: Phase 2 collects metadata," << endl;
		System::out << "Phase 3 probes 40 records per class (first 20 + last 20), and only" << endl;
		System::out << "reserializes classes where bytes actually differ after hash fix." << endl;
		_exit(1);
	}

	int ret = 0;

	try {
		// Shared state for smart mode (Phase 2 populates, Phase 3 consumes)
		std::unordered_map<std::string, std::unordered_map<std::string, ClassInfo>> allClassMaps;

		if (subCmd == "all") {
			System::out << "================================================================" << endl;
			System::out << "  SWGEmu Database Converter" << endl;
			System::out << "  Mode: " << (smartMode ? "Smart (selective reserialize)" : "Classic (full reserialize)") << endl;
			System::out << "  Running all phases: clean → hashfix → reserialize → finalize" << endl;
			System::out << "================================================================" << endl;
			System::out << endl;

			Time pipelineStart;

			ret = phaseClean();
			if (ret != 0) { System::out << "Pipeline aborted at Phase 1." << endl; _exit(ret); }
			System::out << endl;

			ret = phaseHashfix(smartMode, smartMode ? &allClassMaps : nullptr);
			if (ret != 0) { System::out << "Pipeline aborted at Phase 2." << endl; _exit(ret); }
			System::out << endl;

			ret = phaseReserialize(smartMode, smartMode ? &allClassMaps : nullptr);
			if (ret != 0) { System::out << "Pipeline aborted at Phase 3." << endl; _exit(ret); }
			System::out << endl;

			ret = phaseFinalize();
			if (ret != 0) { System::out << "Pipeline aborted at Phase 4." << endl; _exit(ret); }

			double pipelineElapsed = pipelineStart.miliDifference() / 1000.0;
			char elapsed[64];
			formatElapsed(pipelineElapsed, elapsed, sizeof(elapsed));

			System::out << endl;
			System::out << "================================================================" << endl;
			System::out << "  Pipeline complete. Total elapsed: " << elapsed << endl;
			System::out << "  Run './core3' to start the server." << endl;
			System::out << "================================================================" << endl;

		} else if (subCmd == "clean") {
			ret = phaseClean();

		} else if (subCmd == "hashfix") {
			ret = phaseHashfix(smartMode, smartMode ? &allClassMaps : nullptr);

		} else if (subCmd == "reserialize") {
			// If running standalone in smart mode, we need to re-scan for metadata
			// since Phase 2 data isn't available. Re-run hashfix scan (no-op on
			// already-fixed hashes, but collects metadata).
			if (smartMode && allClassMaps.empty()) {
				System::out << "  Smart mode: scanning for class metadata (hashfix already applied)..." << endl;
				System::out << "  (Hash fix pass is idempotent — already-fixed hashes won't match old values)" << endl;
				System::out << endl;

				// Run hashfix in smart mode — idempotent on already-fixed hashes,
				// but we need the metadata scan. This won't re-write the phase2 manifest
				// since it already exists, just populates allClassMaps.
				initEngine();
				ObjectDatabaseManager* dbManager = ObjectDatabaseManager::instance();
				auto databases = discoverDatabases();

				for (size_t dbIdx = 0; dbIdx < databases.size(); dbIdx++) {
					String currentDb = databases[dbIdx].name;
					ObjectDatabase* database = dbManager->loadObjectDatabase(currentDb, true);
					if (!database) continue;

					auto& classMap = allClassMaps[std::string(currentDb.toCharArray())];

					ObjectDatabaseIterator iter(database);
					uint64 oid;
					ObjectInputStream data;

					while (iter.getNextKeyAndValue(oid, &data)) {
						const char* buf = data.getBuffer();
						int bufSize = data.size();

						String className = extractClassNameFromBytes(buf, bufSize);
						float posX = 0, posZ = 0;

						// Quick scan for coordinates
						if (bufSize >= 2) {
							ObjectInputStream scan;
							scan.writeStream(buf, bufSize);
							scan.reset();
							uint16 varCount = scan.readShort();

							for (int v = 0; v < varCount; v++) {
								if (scan.getOffset() + 8 > scan.size()) break;
								uint32 nameHash = scan.readInt();
								uint32 varSize = scan.readInt();
								int fieldOff = scan.getOffset();
								if ((uint32)fieldOff + varSize > (uint32)scan.size()) break;

								if ((nameHash == HASH_NEW_COORD) && varSize >= 8) {
									memcpy(&posX, buf + fieldOff, sizeof(float));
									memcpy(&posZ, buf + fieldOff + 4, sizeof(float));
								}
								scan.shiftOffset(varSize);
							}
						}

						std::string classKey(className.toCharArray());
						classMap[classKey].addRecord(oid, fabs(posX), fabs(posZ), bufSize);
						data.clear();
					}

					printf("  Scanned %s: %d classes\n", currentDb.toCharArray(), (int)classMap.size());
				}
				System::out << endl;
			}

			ret = phaseReserialize(smartMode, smartMode ? &allClassMaps : nullptr);

		} else if (subCmd == "finalize") {
			ret = phaseFinalize();

		} else {
			System::out << "Unknown command: " << subCmd << endl;
			System::out << "Run './dbconvert' without arguments for usage." << endl;
			ret = 1;
		}

	} catch (const Exception& e) {
		e.printStackTrace();
		ret = 1;
	} catch (...) {
		System::err << "unreported exception caught in dbconvert main()" << endl;
		ret = 1;
	}

	_exit(ret);
	return 0;
}
