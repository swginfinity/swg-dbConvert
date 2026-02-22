#!/bin/bash
#
# Restore a database snapshot into a target databases/ directory.
# Wipes all existing .db, log.*, __db.*, .converted, and .progress files first.
#
# Usage:
#   ./restore.sh <snapshot_name> [target_dir]
#
# Examples:
#   ./restore.sh pre_convert                           # restores to default
#   ./restore.sh pre_convert /path/to/bin/databases

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEFAULT_TARGET="databases"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <snapshot_name> [target_dir]"
    echo ""
    echo "Available snapshots:"
    for d in "$SCRIPT_DIR"/*/; do
        [ -d "$d" ] || continue
        name="$(basename "$d")"
        count=$(ls "$d"/*.db 2>/dev/null | wc -l)
        size=$(du -sh "$d" 2>/dev/null | cut -f1)
        echo "  $name  ($count files, $size)"
    done
    exit 1
fi

SNAPSHOT="$1"
TARGET="${2:-$DEFAULT_TARGET}"
SOURCE="$SCRIPT_DIR/$SNAPSHOT"

if [ ! -d "$SOURCE" ]; then
    echo "ERROR: Snapshot '$SNAPSHOT' not found at $SOURCE"
    exit 1
fi

if [ ! -d "$TARGET" ]; then
    echo "ERROR: Target directory does not exist: $TARGET"
    exit 1
fi

DB_COUNT=$(ls "$SOURCE"/*.db 2>/dev/null | wc -l)
SOURCE_SIZE=$(du -sh "$SOURCE" | cut -f1)

echo "=== Database Restore ==="
echo "  Snapshot: $SNAPSHOT ($DB_COUNT files, $SOURCE_SIZE)"
echo "  Target:   $TARGET"
echo ""

# Safety check — make sure nothing is using the databases
if pgrep -f "core3" > /dev/null 2>&1; then
    echo "WARNING: core3 appears to be running. Stop it first!"
    echo "  pkill -f core3"
    exit 1
fi

if pgrep -f "dbconvert" > /dev/null 2>&1; then
    echo "WARNING: dbconvert appears to be running. Stop it first!"
    exit 1
fi

echo "Cleaning target directory..."
rm -f "$TARGET"/*.db
rm -f "$TARGET"/log.*
rm -f "$TARGET"/__db.*
rm -f "$TARGET"/*.converted
rm -f "$TARGET"/*.progress
rm -f "$TARGET"/dbconvert_errors.log
echo "  Removed .db, log.*, __db.*, .converted, .progress files"

echo "Copying snapshot..."
cp "$SOURCE"/*.db "$TARGET"/
echo "  Copied $DB_COUNT .db files ($SOURCE_SIZE)"

echo ""
echo "Done. Clean database environment ready — no logs, no __db env files."
echo "BDB will create a fresh environment on next open."
