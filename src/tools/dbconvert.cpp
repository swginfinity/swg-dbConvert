/*
 * Infinity Database Converter — Standalone Tool
 *
 * Converts 2.0.0 Berkeley DB databases to work with infinity_jtl code.
 * Two passes per database:
 *   Pass 1: Byte-level hash fix (QuadTreeEntry → TreeEntry field renames)
 *   Pass 2: C++ object round-trip (reserialize with current class definitions)
 * Then BDB cleanup: dump/reload to strip environment dependencies.
 *
 * Usage:
 *   ./dbconvert all              - convert all databases
 *   ./dbconvert <database>       - convert single database
 *   ./dbconvert sample <db>      - convert 1 record per class type (test)
 *   ./dbconvert hashonly [db]    - Pass 1 only (skip C++ round-trip)
 *   ./dbconvert clean            - reset BDB LSNs for cross-environment migration
 *
 * Build:
 *   make dbconvert -j24
 *
 * Logging:
 *   Success output: summary counts only (stdout)
 *   Error output: detailed per-error info (stdout + databases/dbconvert_errors.log)
 *     - OID, class name, database name, exception message
 */

#include "server/zone/objects/creature/CreatureObject.h"
#include "server/zone/objects/scene/SceneObject.h"
#include "server/zone/objects/building/BuildingObject.h"
#include "server/zone/objects/player/PlayerObject.h"
#include "server/zone/managers/object/ObjectManager.h"
#include "engine/db/ObjectDatabase.h"
#include "conf/ConfigManager.h"
#include "engine/orb/db/DOBObjectManager.h"

#include <db.h>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <algorithm>
#include <ctime>

// Rebuild all .db files via dump/reload to strip BDB environment dependencies.
static void cleanupBDBEnvironment(const char* dbDir) {
	System::out << endl << "=== Rebuilding databases for clean startup ===" << endl;

	DIR* dir = opendir(dbDir);
	if (!dir) {
		System::out << "WARNING: Could not open " << dbDir << " for cleanup" << endl;
		return;
	}

	struct dirent* entry;
	int cleaned = 0, failed = 0;

	while ((entry = readdir(dir)) != nullptr) {
		String filename = entry->d_name;
		if (entry->d_type == DT_DIR || !filename.endsWith(".db"))
			continue;
		if (filename.endsWith(".converted") || filename.endsWith(".progress"))
			continue;

		String dbPath = String(dbDir) + "/" + filename;
		String tmpPath = dbPath + ".reload.tmp";

		char cmd[512];
		snprintf(cmd, sizeof(cmd),
			"db5.3_dump '%s' 2>/dev/null | db5.3_load '%s' 2>/dev/null",
			dbPath.toCharArray(), tmpPath.toCharArray());

		int ret = system(cmd);
		if (ret == 0) {
			snprintf(cmd, sizeof(cmd), "mv '%s' '%s'",
				tmpPath.toCharArray(), dbPath.toCharArray());
			ret = system(cmd);
			cleaned++;
			System::out << "  " << filename << " OK" << endl;
		} else {
			snprintf(cmd, sizeof(cmd), "rm -f '%s'", tmpPath.toCharArray());
			ret = system(cmd);
			failed++;
			System::out << "  " << filename << " FAILED" << endl;
		}
	}
	closedir(dir);

	char cmd[256];
	snprintf(cmd, sizeof(cmd), "rm -f %s/log.* %s/__db.*", dbDir, dbDir);
	int ret = system(cmd);

	System::out << "Rebuilt " << cleaned << " databases";
	if (failed > 0) System::out << ", " << failed << " failed";
	System::out << endl;
	System::out << "Removed BDB log and environment files." << endl;
	System::out << "Databases are ready for core3." << endl;
}

// ─── Hash replacement table (QuadTreeEntry → TreeEntry) ─────────────────────
struct HashReplacement {
	uint32 oldHash;
	uint32 newHash;
	const char* name;
};

static const HashReplacement HASH_TABLE[] = {
	{ 0x2970c5d9, 0x763502f7, "coordinates" },
	{ 0x5a47d37d, 0xb1649cd1, "bounding"    },
	{ 0x5284d7c8, 0x256e90b3, "parent"      },
	{ 0xac3d85f4, 0xdbd7c28f, "radius"      },
};
static const int HASH_TABLE_SIZE = 4;

// ─── Error log helper ─────────────────────────────────────────────────────
static FILE* errorLog = nullptr;

static void openErrorLog(const char* dbDir) {
	char logPath[512];
	snprintf(logPath, sizeof(logPath), "%s/dbconvert_errors.log", dbDir);
	errorLog = fopen(logPath, "w");
	if (errorLog) {
		time_t now = time(nullptr);
		fprintf(errorLog, "# Infinity Database Converter - Error Log\n");
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
		fprintf(errorLog, "#\n");
		fprintf(errorLog, "# Total errors: %llu\n", (unsigned long long)totalErrors);
		fprintf(errorLog, "# Finished: %s", ctime(&now));
		fclose(errorLog);
		errorLog = nullptr;
	}
}

// ─── Extract _className from raw BDB data ──────────────────────────────────
static const uint32 CLASSNAME_HASH = STRING_HASHCODE("_className");

static String extractClassName(ObjectDatabase* database, uint64 oid) {
	String className = "unknown";

	try {
		ObjectInputStream data;
		if (database->getData(oid, &data) != 0)
			return className;

		if (data.size() < 2)
			return className;

		uint16 varCount = data.readShort();

		for (int v = 0; v < varCount; v++) {
			if (data.getOffset() + 8 > data.size())
				break;

			uint32 nameHash = data.readInt();
			uint32 varSize = data.readInt();

			if ((uint32)data.getOffset() + varSize > (uint32)data.size())
				break;

			if (nameHash == CLASSNAME_HASH) {
				String val;
				val.parseFromBinaryStream(&data);
				className = val;
				break;
			}

			data.shiftOffset(varSize);
		}
	} catch (...) {
	}

	return className;
}

int main(int argc, char* argv[]) {
	System::setStreamBuffer(stdout, nullptr);

	if (argc) {
		StackTrace::setBinaryName(argv[0]);
	}

	static int ret = 0;

	try {
		// Parse arguments
		String subCmd;
		String extraArg;
		for (int i = 1; i < argc; ++i) {
			String arg = argv[i];
			String lower = arg.toLowerCase();
			if (subCmd.isEmpty()) {
				subCmd = lower;
			} else if (extraArg.isEmpty()) {
				extraArg = lower;
			}
		}

		if (subCmd.isEmpty()) {
			System::out << "Infinity Database Converter" << endl;
			System::out << endl;
			System::out << "Usage: ./dbconvert all              - convert all databases" << endl;
			System::out << "       ./dbconvert <database>       - convert single database" << endl;
			System::out << "       ./dbconvert sample <db>      - convert 1 record per class type" << endl;
			System::out << "       ./dbconvert hashonly [db]    - hash fix only (skip round-trip)" << endl;
			System::out << "       ./dbconvert clean            - reset BDB LSNs" << endl;
			_exit(1);
		}

		// ── CLEAN — reset BDB LSNs for cross-environment migration ──
		if (subCmd == "clean") {
			System::out << "=== Berkeley DB Environment Clean ===" << endl;

			const char* dbDir = "databases";
			DIR* dir = opendir(dbDir);
			if (!dir) {
				System::out << "ERROR: Could not open databases/ directory" << endl;
				_exit(1);
			}

			Vector<String> envFiles;
			Vector<String> dbFiles;
			struct dirent* entry;

			while ((entry = readdir(dir)) != nullptr) {
				String filename = entry->d_name;
				if (filename == "." || filename == "..")
					continue;
				if (filename.beginsWith("__db.") || filename.beginsWith("log."))
					envFiles.add(filename);
				else if (filename.endsWith(".db"))
					dbFiles.add(filename);
			}
			closedir(dir);

			for (int i = 0; i < envFiles.size(); ++i) {
				String fullPath = String(dbDir) + "/" + envFiles.get(i);
				remove(fullPath.toCharArray());
			}
			System::out << "Removed " << envFiles.size() << " environment files" << endl;

			DB_ENV* dbenv = nullptr;
			if (db_env_create(&dbenv, 0) == 0) {
				if (dbenv->open(dbenv, dbDir, DB_CREATE | DB_INIT_MPOOL | DB_PRIVATE, 0) == 0) {
					for (int i = 0; i < dbFiles.size(); ++i)
						dbenv->lsn_reset(dbenv, dbFiles.get(i).toCharArray(), 0);
					dbenv->close(dbenv, 0);
					System::out << "Reset LSNs in " << dbFiles.size() << " databases" << endl;
				} else {
					dbenv->close(dbenv, 0);
				}
			}

			char cmd[256];
			snprintf(cmd, sizeof(cmd), "rm -f %s/__db.* %s/log.*", dbDir, dbDir);
			ret = system(cmd);

			System::out << "Clean complete." << endl;
			_exit(0);
		}

		// ── SAMPLE mode ──
		bool sampleMode = false;
		if (subCmd == "sample") {
			if (extraArg.isEmpty()) {
				System::out << "Usage: ./dbconvert sample <dbname>" << endl;
				_exit(1);
			}
			sampleMode = true;
			subCmd = extraArg;
			extraArg = "";
		}

		// ── HASHONLY mode ──
		bool hashOnlyMode = false;
		if (subCmd == "hashonly") {
			hashOnlyMode = true;
			if (!extraArg.isEmpty()) {
				subCmd = extraArg;
				extraArg = "";
			} else {
				subCmd = "all";
			}
		}

		// ── CONVERT ALL / CONVERT <dbname> ──

		const char* dbDir = "databases";
		const int BATCH_SIZE = 500;

		System::out << "================================================================" << endl;
		System::out << "  Infinity Database Converter (standalone)" << endl;
		System::out << "  Pass 1: Hash fix (QuadTreeEntry -> TreeEntry)" << endl;
		System::out << "  Pass 2: Reserialize (C++ round-trip with current code)" << endl;
		if (hashOnlyMode)
			System::out << "  Mode:   Hash fix only (Pass 2 skipped)" << endl;
		if (sampleMode)
			System::out << "  Mode:   Sample (1 record per class type)" << endl;
		System::out << "================================================================" << endl;
		System::out << endl;

		// Initialize engine — minimal, no server, no managers
		Core::MANAGED_REFERENCE_LOAD = false;
		ConfigManager::instance()->loadConfigData();
		Core::initializeProperties("Core3");

		// Increase BDB lock limits for hash fix pass
		Core::setIntProperty("BerkeleyDB.envMaxLocks", 15000000);
		Core::setIntProperty("BerkeleyDB.envMaxLockers", 15000000);
		Core::setIntProperty("BerkeleyDB.envMaxLockObjects", 15000000);

		ObjectManager* objManager = ObjectManager::instance();
		ObjectDatabaseManager* dbManager = ObjectDatabaseManager::instance();

		// Open error log file
		openErrorLog(dbDir);

		// Build list of databases to convert
		Vector<String> toConvert;

		if (subCmd == "all") {
			struct DbEntry { String name; off_t size; };
			std::vector<DbEntry> allDbs;

			DIR* dir = opendir(dbDir);
			if (!dir) {
				System::out << "ERROR: Could not open databases/ directory" << endl;
				_exit(1);
			}

			struct dirent* entry;
			while ((entry = readdir(dir)) != nullptr) {
				String filename = entry->d_name;
				if (!filename.endsWith(".db") || filename.length() <= 3)
					continue;
				String name = filename.subString(0, filename.length() - 3);
				if (name.beginsWith("__") || name.contains("index"))
					continue;

				struct stat st;
				char path[512];
				snprintf(path, sizeof(path), "%s/%s", dbDir, filename.toCharArray());
				off_t fileSize = 0;
				if (stat(path, &st) == 0)
					fileSize = st.st_size;
				allDbs.push_back({name, fileSize});
			}
			closedir(dir);

			std::sort(allDbs.begin(), allDbs.end(), [](const DbEntry& a, const DbEntry& b) {
				return a.size < b.size;
			});

			for (size_t i = 0; i < allDbs.size(); i++) {
				String manifestPath = String(dbDir) + "/" + allDbs[i].name + ".converted";
				FILE* check = fopen(manifestPath.toCharArray(), "r");
				if (check != nullptr) {
					fclose(check);
					double mb = allDbs[i].size / (1024.0 * 1024.0);
					System::out << "  [SKIP] " << allDbs[i].name << " (" << (int)mb << " MB, already converted)" << endl;
					continue;
				}
				toConvert.add(allDbs[i].name);
			}

			if (toConvert.size() == 0) {
				System::out << "All databases already converted." << endl;
				_exit(0);
			}

			System::out << toConvert.size() << " databases to convert" << endl << endl;
		} else {
			toConvert.add(subCmd);
		}

		// ── Process each database ──
		Time overallStart;
		uint64 grandHashFixed = 0, grandReserialized = 0, grandErrors = 0;

		for (int dbIndex = 0; dbIndex < toConvert.size(); ++dbIndex) {
			String currentDb = toConvert.get(dbIndex);

			System::out << "================================================================" << endl;
			System::out << "  [" << (dbIndex + 1) << "/" << toConvert.size() << "] " << currentDb << endl;
			System::out << "================================================================" << endl;

			ObjectDatabase* database = dbManager->loadObjectDatabase(currentDb, false);
			if (database == nullptr) {
				System::out << "ERROR: Could not load database: " << currentDb << endl;
				logError(currentDb.toCharArray(), 0, "N/A", "Could not load database");
				grandErrors++;
				continue;
			}

			// ──────────────────────────────────────────────────────────
			// PASS 1: Hash fix (byte-level, constant memory)
			// ──────────────────────────────────────────────────────────
			uint64 hashRecords = 0, hashModified = 0;

			if (sampleMode) {
				System::out << "  Pass 1: Hash fix (skipped in sample mode)" << endl;
			} else {
			System::out << "  Pass 1: Hash fix..." << endl;

			Time hashStart;

			{
				auto* txn = ObjectDatabaseManager::instance()->startTransaction();
				LocalDatabaseIterator iterator(txn, database);
				ObjectInputStream key;
				ObjectInputStream data;

				while (iterator.getNextKeyAndValue(&key, &data)) {
					hashRecords++;

					int dataSize = data.size();
					if (dataSize < 2) {
						key.clear();
						data.clear();
						continue;
					}

					ObjectOutputStream output(dataSize);
					const char* buffer = data.getBuffer();
					output.writeStream(buffer, dataSize);

					data.reset();
					uint16 varCount = data.readShort();
					bool modified = false;

					for (int v = 0; v < varCount; v++) {
						int hashOffset = data.getOffset();

						if (hashOffset + 8 > dataSize)
							break;

						uint32 nameHash = data.readInt();
						uint32 varSize = data.readInt();

						if ((uint32)data.getOffset() + varSize > (uint32)dataSize)
							break;

						for (int h = 0; h < HASH_TABLE_SIZE; h++) {
							if (nameHash == HASH_TABLE[h].oldHash) {
								char* outBuf = (char*)output.getBuffer();
								uint32 newHash = HASH_TABLE[h].newHash;
								// Write in native byte order (memcpy) to match readInt()
								memcpy(outBuf + hashOffset, &newHash, sizeof(newHash));
								modified = true;
								break;
							}
						}

						data.shiftOffset(varSize);
					}

					if (modified) {
						hashModified++;
						ObjectOutputStream* outputCopy = new ObjectOutputStream(output.size());
						outputCopy->writeStream(output.getBuffer(), output.size());
						iterator.putCurrent(outputCopy);
					}

					if (hashRecords % 50000 == 0) {
						double elapsed = hashStart.miliDifference() / 1000.0;
						double rate = elapsed > 0 ? hashRecords / elapsed : 0;
						printf("\r    %llu records, %llu fixed, %.0f/s   ",
							(unsigned long long)hashRecords, (unsigned long long)hashModified, rate);
						fflush(stdout);
					}

					key.clear();
					data.clear();
				}

				iterator.closeCursor();
				ObjectDatabaseManager::instance()->commitTransaction(txn);
			}

			double hashElapsed = hashStart.miliDifference() / 1000.0;
			printf("\r    %llu records, %llu fixed in %.0fs                    \n",
				(unsigned long long)hashRecords, (unsigned long long)hashModified, hashElapsed);
			grandHashFixed += hashModified;
			} // end else (!sampleMode) for Pass 1

			// ──────────────────────────────────────────────────────────
			// PASS 2: Reserialize (C++ round-trip, bounded memory)
			// ──────────────────────────────────────────────────────────
			if (hashOnlyMode) {
				System::out << "  Pass 2: SKIPPED (hashonly mode)" << endl;
				System::out << endl;
				continue;
			}
			System::out << "  Pass 2: Reserialize..." << endl;

			const uint32 classNameHash = STRING_HASHCODE("_className");
			HashSet<String> sampleSeenClasses;
			std::vector<uint64> allKeys;

			if (sampleMode) {
				System::out << "    (sample mode: scanning for unique class types...)" << endl;
				ObjectDatabaseIterator scanIter(database);
				uint64 scanKey;
				ObjectInputStream scanData;
				uint64 scanned = 0;

				while (scanIter.getNextKeyAndValue(scanKey, &scanData)) {
					scanned++;
					String className;
					if (Serializable::getVariable<String>(classNameHash, &className, &scanData)) {
						if (!sampleSeenClasses.contains(className)) {
							sampleSeenClasses.add(className);
							allKeys.push_back(scanKey);
						}
					}
					scanData.clear();

					if (scanned % 100000 == 0) {
						printf("\r    %llu scanned, %d types, %d keys   ",
							(unsigned long long)scanned, sampleSeenClasses.size(), (int)allKeys.size());
						fflush(stdout);
					}
				}

				printf("\r    %llu scanned -> %d unique types to convert                    \n",
					(unsigned long long)scanned, (int)allKeys.size());
			} else {
				ObjectDatabaseIterator keyIter(database);
				uint64 k;
				while (keyIter.getNextKey(k))
					allKeys.push_back(k);
			}

			uint64 totalObjects = allKeys.size();
			uint64 converted = 0, errors = 0;
			Time reserStart;
			Vector<uint64> batchOids;

			for (size_t ki = 0; ki < allKeys.size(); ki++) {
				uint64 oid = allKeys[ki];

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
				uint64 processed = converted + errors;
				if (processed % 500 == 0 || processed == totalObjects) {
					double elapsed = reserStart.miliDifference() / 1000.0;
					double rate = elapsed > 0 ? processed / elapsed : 0;
					uint64 remaining = totalObjects - processed;
					uint64 etaSec = rate > 0 ? (uint64)(remaining / rate) : 0;

					char etaBuf[64];
					if (etaSec > 3600)
						snprintf(etaBuf, sizeof(etaBuf), "%lluh %llum",
							(unsigned long long)(etaSec / 3600), (unsigned long long)((etaSec % 3600) / 60));
					else if (etaSec > 60)
						snprintf(etaBuf, sizeof(etaBuf), "%llum %llus",
							(unsigned long long)(etaSec / 60), (unsigned long long)(etaSec % 60));
					else
						snprintf(etaBuf, sizeof(etaBuf), "%llus", (unsigned long long)etaSec);

					printf("\r    %llu/%llu :: %.0f/s :: %llu errors :: ETA %s          ",
						(unsigned long long)processed, (unsigned long long)totalObjects,
						rate, (unsigned long long)errors, etaBuf);
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
			printf("\r    %llu/%llu :: %llu errors :: %.0fs                              \n",
				(unsigned long long)converted, (unsigned long long)totalObjects,
				(unsigned long long)errors, reserElapsed);

			grandReserialized += converted;
			grandErrors += errors;

			// Per-database summary to error log
			{
				char buf[256];
				snprintf(buf, sizeof(buf), "[%s] hash_fixed=%llu reserialized=%llu errors=%llu",
					currentDb.toCharArray(), (unsigned long long)hashModified,
					(unsigned long long)converted, (unsigned long long)errors);
				logSummary(buf);
			}

			// Write manifest
			if (!sampleMode) {
				String manifestPath = String(dbDir) + "/" + currentDb + ".converted";
				FILE* manifest = fopen(manifestPath.toCharArray(), "w");
				if (manifest != nullptr) {
					Time now;
					fprintf(manifest, "converted=%llu\n", (unsigned long long)now.getMiliTime());
					fprintf(manifest, "hash_fixed=%llu\n", (unsigned long long)hashModified);
					fprintf(manifest, "reserialized=%llu\n", (unsigned long long)converted);
					fprintf(manifest, "errors=%llu\n", (unsigned long long)errors);
					fclose(manifest);
				}
			}

			if (sampleMode && sampleSeenClasses.size() > 0) {
				System::out << "  Class types converted:" << endl;
				HashSetIterator<String> typeIter = sampleSeenClasses.iterator();
				while (typeIter.hasNext()) {
					System::out << "    " << typeIter.getNextKey() << endl;
				}
			}

			System::out << endl;
		}

		// ── Summary ──
		double totalElapsed = overallStart.miliDifference() / 1000.0;
		int hours = (int)totalElapsed / 3600;
		int minutes = ((int)totalElapsed % 3600) / 60;
		int secs = (int)totalElapsed % 60;

		System::out << "================================================================" << endl;
		System::out << "  COMPLETE" << endl;
		System::out << "  Hash fixes:    " << grandHashFixed << endl;
		System::out << "  Reserialized:  " << grandReserialized << endl;
		System::out << "  Errors:        " << grandErrors << endl;
		System::out << "  Elapsed:       " << hours << "h " << minutes << "m " << secs << "s" << endl;
		if (grandErrors > 0)
			System::out << "  Error log:     databases/dbconvert_errors.log" << endl;
		System::out << "================================================================" << endl;

		closeErrorLog(grandErrors);
		objManager->shutdown();

		if (sampleMode) {
			System::out << endl;
			System::out << "Sample complete. Fix any errors, then run full convert:" << endl;
			System::out << "  ./dbconvert " << toConvert.get(0) << endl;
		} else {
			cleanupBDBEnvironment(dbDir);

			System::out << endl;
			System::out << "Done. Run './core3' to start the server." << endl;
		}

	} catch (const Exception& e) {
		e.printStackTrace();
	} catch (...) {
		System::err << "unreported exception caught in dbconvert main()" << endl;
	}

	_exit(ret);
	return 0;
}
