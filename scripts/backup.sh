#!/bin/bash
#
# Back up SWGEmu databases to a named snapshot.
#
# Usage:
#   ./backup.sh <snapshot_name> [source_dir]
#
# Examples:
#   ./backup.sh pre_convert                    # backs up from default databases dir
#   ./backup.sh pre_convert /path/to/bin/databases
#
# Snapshots are stored in the same directory as this script.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEFAULT_SOURCE="databases"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <snapshot_name> [source_dir]"
    echo ""
    echo "Creates a snapshot of the databases directory."
    echo "Run this from your bin/ directory, or specify the full path."
    echo ""
    echo "Existing snapshots:"
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
SOURCE="${2:-$DEFAULT_SOURCE}"
TARGET="$SCRIPT_DIR/$SNAPSHOT"

if [ ! -d "$SOURCE" ]; then
    echo "ERROR: Source directory does not exist: $SOURCE"
    echo "Run this from your bin/ directory, or specify the full path."
    exit 1
fi

if [ -d "$TARGET" ]; then
    echo "ERROR: Snapshot '$SNAPSHOT' already exists at $TARGET"
    echo "Delete it first or choose a different name."
    exit 1
fi

DB_COUNT=$(ls "$SOURCE"/*.db 2>/dev/null | wc -l)
SOURCE_SIZE=$(du -sh "$SOURCE" | cut -f1)

if [ "$DB_COUNT" -eq 0 ]; then
    echo "ERROR: No .db files found in $SOURCE"
    exit 1
fi

echo "=== Database Backup ==="
echo "  Source:   $SOURCE ($DB_COUNT files, $SOURCE_SIZE)"
echo "  Snapshot: $TARGET"
echo ""

mkdir -p "$TARGET"
cp "$SOURCE"/*.db "$TARGET"/

DEST_SIZE=$(du -sh "$TARGET" | cut -f1)
echo "Done. Copied $DB_COUNT .db files ($DEST_SIZE)"
echo ""
echo "To restore later:"
echo "  ./restore.sh $SNAPSHOT /path/to/bin/databases"
