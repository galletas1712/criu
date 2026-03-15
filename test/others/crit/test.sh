#!/bin/bash
# shellcheck disable=SC2002

set -x

# shellcheck source=test/others/env.sh
source ../env.sh

images_list=()

function gen_imgs {
	PID=$(../loop)
	if ! $CRIU dump -v4 -o dump.log -D ./ -t "$PID"; then
		echo "Failed to checkpoint process $PID"
		cat dump.log
		kill -9 "$PID"
		exit 1
	fi

	images_list=(./*.img)
	if [ "${#images_list[@]}" -eq 0 ]; then
		echo "Failed to generate images"
		exit 1
	fi
}

function run_test1 {
	for x in "${images_list[@]}"
	do
		echo "=== $x"
		if [[ $x == *pages* ]]; then
			echo "skip"
			continue
		fi

		echo "  -- to json"
		$CRIT decode -o "$x"".json" --pretty < "$x" || exit $?
		echo "  -- to img"
		$CRIT encode -i "$x"".json" > "$x"".json.img" || exit $?
		echo "  -- cmp"
		cmp "$x" "$x"".json.img" || exit $?

		echo "=== done"
	done
}


function run_test2 {
	PROTO_IN="${images_list[0]}"
	JSON_IN=$(mktemp -p ./ tmp.XXXXXXXXXX.json)
	OUT=$(mktemp -p ./ tmp.XXXXXXXXXX.log)

	# prepare
	${CRIT} decode -i "${PROTO_IN}" -o "${JSON_IN}"

	# show info about image
	${CRIT} info "${PROTO_IN}"

	# proto in - json out decode
	cat "${PROTO_IN}" | ${CRIT} decode || exit 1
	cat "${PROTO_IN}" | ${CRIT} decode -o "${OUT}" || exit 1
	cat "${PROTO_IN}" | ${CRIT} decode > "${OUT}" || exit 1
	${CRIT} decode -i "${PROTO_IN}" || exit 1
	${CRIT} decode -i "${PROTO_IN}" -o "${OUT}" || exit 1
	${CRIT} decode -i "${PROTO_IN}" > "${OUT}" || exit 1
	${CRIT} decode < "${PROTO_IN}" || exit 1
	${CRIT} decode -o "${OUT}" < "${PROTO_IN}" || exit 1
	${CRIT} decode < "${PROTO_IN}" > "${OUT}" || exit 1

	# proto in - json out encode -> should fail
	cat "${PROTO_IN}" | ${CRIT} encode || true
	cat "${PROTO_IN}" | ${CRIT} encode -o "${OUT}" || true
	cat "${PROTO_IN}" | ${CRIT} encode > "${OUT}" || true
	${CRIT} encode -i "${PROTO_IN}" || true
	${CRIT} encode -i "${PROTO_IN}" -o "${OUT}" || true
	${CRIT} encode -i "${PROTO_IN}" > "${OUT}" || true

	# json in - proto out encode
	cat "${JSON_IN}" | ${CRIT} encode || exit 1
	cat "${JSON_IN}" | ${CRIT} encode -o "${OUT}" || exit 1
	cat "${JSON_IN}" | ${CRIT} encode > "${OUT}" || exit 1
	${CRIT} encode -i "${JSON_IN}" || exit 1
	${CRIT} encode -i "${JSON_IN}" -o "${OUT}" || exit 1
	${CRIT} encode -i "${JSON_IN}" > "${OUT}" || exit 1
	${CRIT} encode < "${JSON_IN}" || exit 1
	${CRIT} encode -o "${OUT}" < "${JSON_IN}" || exit 1
	${CRIT} encode < "${JSON_IN}" > "${OUT}" || exit 1

	# json in - proto out decode -> should fail
	cat "${JSON_IN}" | ${CRIT} decode || true
	cat "${JSON_IN}" | ${CRIT} decode -o "${OUT}" || true
	cat "${JSON_IN}" | ${CRIT} decode > "${OUT}" || true
	${CRIT} decode -i "${JSON_IN}" || true
	${CRIT} decode -i "${JSON_IN}" -o "${OUT}" || true
	${CRIT} decode -i "${JSON_IN}" > "${OUT}" || true

	# explore image directory
	${CRIT} x ./ ps || exit 1
	${CRIT} x ./ fds || exit 1
	${CRIT} x ./ mems || exit 1
	${CRIT} x ./ rss || exit 1
}

ZDTM_DIR="${BASE_DIR}/test/zdtm/static"

function dump_test_process {
	# Dump compress_pages02 (covers 8 mapping types: anonymous,
	# zero-filled, shared anonymous, file-backed private/shared,
	# memfd, read-only, PROT_NONE guard) into the given directory.
	# Prints the PID on success.
	local dir=$1; shift
	local pid

	make -C "$ZDTM_DIR" compress_pages02 > /dev/null 2>&1

	# Clean stale marker files and run from the test directory
	# so zdtm pidfile rename works (same filesystem).
	rm -f "$dir"/test.out* "$dir"/test.pid "$dir"/test.file

	(cd "$dir" && "$ZDTM_DIR/compress_pages02" \
		--pidfile=test.pid \
		--outfile=test.out \
		--filename=test.file)

	# Wait for pidfile
	local i=0
	while [ $i -lt 50 ]; do
		if [ -f "$dir/test.pid" ]; then
			break
		fi
		sleep 0.1
		i=$((i + 1))
	done

	pid=$(cat "$dir/test.pid" 2>/dev/null)
	if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
		echo "FAIL: test process didn't start"
		cat "$dir/test.out" 2>/dev/null
		exit 1
	fi

	if ! $CRIU dump -v4 -o dump.log -D "$dir" -t "$pid" --shell-job "$@"; then
		echo "FAIL: dump into $dir"
		cat "$dir/dump.log"
		kill -9 "$pid" 2>/dev/null
		exit 1
	fi
	echo "$pid"
}

function restore_and_verify {
	# Restore from directory, send SIGTERM so compress_pages02
	# verifies its own memory, then check the test output for PASS.
	local dir=$1
	local pid

	if ! $CRIU restore -v4 -o restore.log -D "$dir" --shell-job -d; then
		echo "FAIL: restore from $dir"
		cat "$dir/restore.log"
		exit 1
	fi

	# Get the root PID from the pstree image
	pid=$($CRIT decode -i "$dir/pstree.img" 2>/dev/null |
		grep -o '"pid": [0-9]*' | head -1 | grep -o '[0-9]*')

	if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
		echo "FAIL: restored process not running from $dir (pid=$pid)"
		exit 1
	fi

	# Send SIGTERM so compress_pages02 verifies all memory
	# regions (zero, pattern, shared, file, memfd, readonly,
	# guard) then writes PASS/FAIL to its output file.
	kill -TERM "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	sleep 0.5

	if ! grep -q PASS "$dir/test.out" 2>/dev/null; then
		echo "FAIL: memory verification failed after restore from $dir"
		cat "$dir/test.out" 2>/dev/null
		exit 1
	fi
}

function pages_checksum {
	# Print md5sum of all pages-*.img files in a directory.
	# This captures the raw page content for comparison.
	cat "$1"/pages-*.img | md5sum | awk '{print $1}'
}

function run_test_compress {
	echo "=== compress/decompress tests ==="

	# -------------------------------------------------------
	# Test 1: dump -c -> decompress -> restore
	# -------------------------------------------------------
	echo "  -- Test 1: compressed dump -> decompress -> restore"
	dump_test_process comp/ -c > /dev/null

	$CRIT decompress comp/ || exit 1

	# Verify backup files exist
	ls comp/pages-*.img.bak > /dev/null 2>&1 || { echo "FAIL: no pages backup"; exit 1; }
	ls comp/pagemap-*.img.bak > /dev/null 2>&1 || { echo "FAIL: no pagemap backup"; exit 1; }
	ls comp/inventory.img.bak > /dev/null 2>&1 || { echo "FAIL: no inventory backup"; exit 1; }

	restore_and_verify comp/
	echo "     PASS"

	# -------------------------------------------------------
	# Test 2: dump uncompressed -> compress -> restore
	# -------------------------------------------------------
	echo "  -- Test 2: uncompressed dump -> compress -> restore"
	dump_test_process uncomp/ > /dev/null

	$CRIT compress uncomp/ --in-place || exit 1

	# Verify no backup files with --in-place
	if ls uncomp/*.bak > /dev/null 2>&1; then
		echo "FAIL: backup files created with --in-place"
		exit 1
	fi

	restore_and_verify uncomp/
	echo "     PASS"

	# -------------------------------------------------------
	# Test 3: compress already compressed, decompress already decompressed
	# -------------------------------------------------------
	echo "  -- Test 3: compress already compressed, decompress already decompressed"
	$CRIT compress uncomp/ --in-place 2>&1 | grep -q "already compressed" || {
		echo "FAIL: compress should report already compressed"
		exit 1
	}
	$CRIT decompress comp/ 2>&1 | grep -q "already decompressed" || {
		echo "FAIL: decompress should report already decompressed"
		exit 1
	}
	echo "     PASS"

	# -------------------------------------------------------
	# Test 4: compress -> decompress -> compress produces same pages
	# -------------------------------------------------------
	echo "  -- Test 4: compress -> decompress -> compress stability"
	rm -rf cdc/
	mkdir -p cdc/
	dump_test_process cdc/ > /dev/null

	$CRIT compress cdc/ --in-place || exit 1
	local sum1
	sum1=$(pages_checksum cdc/)

	$CRIT decompress cdc/ --in-place || exit 1
	$CRIT compress cdc/ --in-place || exit 1
	local sum2
	sum2=$(pages_checksum cdc/)

	if [ "$sum1" != "$sum2" ]; then
		echo "FAIL: compress->decompress->compress changed pages data"
		echo "  first:  $sum1"
		echo "  second: $sum2"
		exit 1
	fi

	restore_and_verify cdc/
	echo "     PASS"

	# -------------------------------------------------------
	# Test 5: decompress -> compress -> decompress produces same pages
	# -------------------------------------------------------
	echo "  -- Test 5: decompress -> compress -> decompress stability"
	rm -rf dcd/
	mkdir -p dcd/
	dump_test_process dcd/ -c > /dev/null

	$CRIT decompress dcd/ --in-place || exit 1
	local sum3
	sum3=$(pages_checksum dcd/)

	$CRIT compress dcd/ --in-place || exit 1
	$CRIT decompress dcd/ --in-place || exit 1
	local sum4
	sum4=$(pages_checksum dcd/)

	if [ "$sum3" != "$sum4" ]; then
		echo "FAIL: decompress->compress->decompress changed pages data"
		echo "  first:  $sum3"
		echo "  second: $sum4"
		exit 1
	fi

	restore_and_verify dcd/
	echo "     PASS"

	echo "=== compress/decompress: ALL PASS ==="
}

${CRIT} --version

gen_imgs
run_test1
run_test2

# Skip compress/decompress tests if lz4 or CRIU compression is unavailable
if python3 -c "import lz4.block" 2>/dev/null && $CRIU check --feature compress 2>/dev/null; then
	mkdir -p comp/ uncomp/
	run_test_compress
else
	echo "=== Skipping compress/decompress tests (lz4 or CRIU compression not available) ==="
fi
