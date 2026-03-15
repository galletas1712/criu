#!/bin/bash

# Test mixed-compression iterative checkpoint chains.
#
# Pre-dumps are done WITHOUT --compress, then the final dump
# uses --compress. On restore, the parent pagemap entries
# have no compressed_size array while the child entries do.
# This exercises the per-entry fallback in
# maybe_read_page_local_compressed().

# shellcheck source=test/others/env.sh
source ../env.sh || exit 1

function fail {
	echo "FAIL: $*"
	exit 1
}

set -x

IMGDIR="dump/"
rm -rf "$IMGDIR"
mkdir "$IMGDIR"

# Build and start the test process
(
	cd ../../zdtm/static/ || exit 1
	make cleanout
	make compress_pages00
	make compress_pages00.pid || exit 1
)
PID=$(cat ../../zdtm/static/compress_pages00.pid)
kill -0 "$PID" || fail "Test didn't start"

echo "=== Pre-dump 1 (no compression) ==="
mkdir "$IMGDIR/1/"
${CRIU} pre-dump -D "$IMGDIR/1/" -o dump.log -t "$PID" -v4 \
	--track-mem -R || fail "Pre-dump 1 failed"

sleep 1

echo "=== Pre-dump 2 (no compression) ==="
mkdir "$IMGDIR/2/"
${CRIU} pre-dump -D "$IMGDIR/2/" -o dump.log -t "$PID" -v4 \
	--prev-images-dir=../1/ --track-mem -R || fail "Pre-dump 2 failed"

sleep 1

echo "=== Final dump (WITH compression) ==="
mkdir "$IMGDIR/3/"
${CRIU} dump -D "$IMGDIR/3/" -o dump.log -t "$PID" -v4 \
	--prev-images-dir=../2/ --track-mem \
	-c || fail "Final dump failed"

echo "=== Restore ==="
${CRIU} restore -D "$IMGDIR/3/" -o restore.log -v4 -d \
	|| fail "Restore failed"

(
	cd ../../zdtm/static/ || exit 1
	make compress_pages00.stop
	grep PASS compress_pages00.out || exit 1
) || fail "Memory content verification failed"

echo "Test PASSED"
