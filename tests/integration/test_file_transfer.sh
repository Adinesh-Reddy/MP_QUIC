#!/bin/sh
# End-to-end test of the programs of apps/ (Phase 11): a real mpq_server process and real
# mpq_client processes exchange files over loopback, and SHA-256 proves that the files
# arrived intact. Run it from the project folder:  sh tests/integration/test_file_transfer.sh
#
# Loopback accepts every address of 127.0.0.0/8, so the server "has two interfaces"
# (127.0.0.1 and 127.0.0.2) and so does the client (127.0.1.1 and 127.0.1.2).

SERVER=./bin/mpq_server
CLIENT=./bin/mpq_client
REQRES=./bin/mpq_reqres
PORT=19501

if [ ! -x "$SERVER" ] || [ ! -x "$CLIENT" ] || [ ! -x "$REQRES" ]; then
    echo "The programs are not built: run 'make apps' first"
    exit 1
fi

DIR=$(mktemp -d)
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    rm -rf "$DIR"
}
trap cleanup EXIT

fail() {
    echo "  -> FAILED: $1"
    echo "---- client output ----"
    cat "$DIR/client.log" 2>/dev/null
    echo "---- server output ----"
    cat "$DIR/server.log" 2>/dev/null
    exit 1
}

echo "=== Running File Transfer Test (two processes, SHA-256 check) ==="

mkdir "$DIR/files"
head -c 3000000 /dev/urandom > "$DIR/files/data.bin"   # 3 MB of random bytes
: > "$DIR/files/empty.bin"                              # a file of 0 bytes
EXPECTED=$(sha256sum < "$DIR/files/data.bin" | cut -d' ' -f1)

"$SERVER" --listen 127.0.0.1:$PORT --listen 127.0.0.2:$PORT --dir "$DIR/files" \
          --stats "$DIR/server_stats.csv" > "$DIR/server.log" 2>&1 &
SERVER_PID=$!
sleep 0.3

# ---------------------------------------------------------------------------
echo "[TEST] 3 MB file over two paths..."
"$CLIENT" --server 127.0.0.1:$PORT --local 127.0.1.1 --local 127.0.1.2 \
          --get data.bin --out "$DIR/out_two_paths.bin" --stats "$DIR/client_stats.csv" \
          --timeout 60 > "$DIR/client.log" 2>&1 || fail "the client reported an error"

RECEIVED=$(sha256sum < "$DIR/out_two_paths.bin" | cut -d' ' -f1)
REPORTED=$(sed -n 's/.*sha256=\([0-9a-f]*\).*/\1/p' "$DIR/client.log")
[ "$RECEIVED" = "$EXPECTED" ] || fail "the received file differs from the original"
[ "$REPORTED" = "$EXPECTED" ] || fail "the SHA-256 printed by the client differs from sha256sum"
grep -q "RESULT status=ok .* paths=2" "$DIR/client.log" || fail "the transfer did not use two paths"
[ "$(wc -l < "$DIR/client_stats.csv")" -ge 2 ] || fail "the client wrote no statistics"
echo "  sha256 $RECEIVED"
echo "  -> PASSED"

# ---------------------------------------------------------------------------
echo "[TEST] The same file over a single path (one local address)..."
"$CLIENT" --server 127.0.0.2:$PORT --local 127.0.1.1 \
          --get data.bin --out "$DIR/out_one_path.bin" --timeout 60 > "$DIR/client.log" 2>&1 \
          || fail "the client reported an error"
RECEIVED=$(sha256sum < "$DIR/out_one_path.bin" | cut -d' ' -f1)
[ "$RECEIVED" = "$EXPECTED" ] || fail "the received file differs from the original"
grep -q "RESULT status=ok .* paths=1" "$DIR/client.log" || fail "the transfer did not stay on one path"
echo "  -> PASSED"

# ---------------------------------------------------------------------------
echo "[TEST] An empty file..."
"$CLIENT" --server 127.0.0.1:$PORT --local 127.0.1.1 --local 127.0.1.2 \
          --get empty.bin --out "$DIR/out_empty.bin" --timeout 60 > "$DIR/client.log" 2>&1 \
          || fail "the client reported an error"
[ ! -s "$DIR/out_empty.bin" ] || fail "the received file is not empty"
grep -q "bytes=0 .*sha256=e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" "$DIR/client.log" \
    || fail "wrong SHA-256 for an empty file"
echo "  -> PASSED"

# ---------------------------------------------------------------------------
echo "[TEST] A file that does not exist: the client reports the error..."
if "$CLIENT" --server 127.0.0.1:$PORT --local 127.0.1.1 --get missing.bin \
             --out "$DIR/out_missing.bin" --timeout 60 > "$DIR/client.log" 2>&1; then
    fail "the client should have failed"
fi
grep -q "RESULT status=failed .*file not found" "$DIR/client.log" || fail "the error was not reported"
echo "  -> PASSED"

# ---------------------------------------------------------------------------
echo "[TEST] A name outside the folder is refused..."
if "$CLIENT" --server 127.0.0.1:$PORT --local 127.0.1.1 --get ../server.log \
             --out "$DIR/out_bad.bin" --timeout 60 > "$DIR/client.log" 2>&1; then
    fail "the client should have failed"
fi
grep -q "RESULT status=failed .*bad file name" "$DIR/client.log" || fail "the request was not refused"
echo "  -> PASSED"

# ---------------------------------------------------------------------------
echo "[TEST] Request/response traffic: 10 requests of 750 bytes, 50 ms apart..."
"$REQRES" --server 127.0.0.1:$PORT --local 127.0.1.1 --local 127.0.1.2 \
          --count 10 --interval-ms 50 --out "$DIR/delays.csv" > "$DIR/client.log" 2>&1 \
          || fail "not every request was answered"
grep -q "RESULT status=ok requests=10 answered=10" "$DIR/client.log" || fail "wrong summary"
[ "$(wc -l < "$DIR/delays.csv")" -eq 11 ] || fail "the CSV file should have a header and 10 lines"
echo "  -> PASSED"

# ---------------------------------------------------------------------------
# A large file must flow without stalls: the server reads it piece by piece, so the
# event loop keeps answering and no path's retransmission timer runs out. A path flagged
# "may be broken" on a healthy loopback link would mean the sender stopped responding.
echo "[TEST] 64 MB file over two paths, without any path flagged as broken..."
dd if=/dev/urandom of="$DIR/files/test_64m.bin" bs=1M count=64 2>/dev/null \
    || fail "could not create the 64 MB test file"
EXPECTED_64M=$(sha256sum < "$DIR/files/test_64m.bin" | cut -d' ' -f1)
SERVER_LINES_BEFORE=$(wc -l < "$DIR/server.log")

"$CLIENT" --server 127.0.0.1:$PORT --local 127.0.1.1 --local 127.0.1.2 \
          --get test_64m.bin --out "$DIR/out_64m.bin" --timeout 120 > "$DIR/client.log" 2>&1 \
          || fail "the client reported an error"
sleep 0.2 # the server prints its CONNECTION line once the client has closed

RECEIVED=$(sha256sum < "$DIR/out_64m.bin" | cut -d' ' -f1)
[ "$RECEIVED" = "$EXPECTED_64M" ] || fail "the received 64 MB file differs from the original"
grep -q "RESULT status=ok .* paths=2" "$DIR/client.log" || fail "the 64 MB transfer did not use two paths"
if grep -q "Path .* may be broken" "$DIR/client.log"; then
    fail "the client flagged a path as broken during the 64 MB transfer"
fi
if tail -n +"$((SERVER_LINES_BEFORE + 1))" "$DIR/server.log" | grep -q "Path .* may be broken"; then
    fail "the server flagged a path as broken during the 64 MB transfer"
fi
rm -f "$DIR/files/test_64m.bin" "$DIR/out_64m.bin" # 128 MB less in the temporary folder
echo "  sha256 $RECEIVED"
echo "  -> PASSED"

# ---------------------------------------------------------------------------
echo "[TEST] The server served all of them as separate connections and is still running..."
kill -0 "$SERVER_PID" 2>/dev/null || fail "the server stopped"
sleep 0.2
CONNECTIONS=$(grep -c "^CONNECTION " "$DIR/server.log")
[ "$CONNECTIONS" -eq 7 ] || fail "the server saw $CONNECTIONS connections, expected 7"
[ "$(wc -l < "$DIR/server_stats.csv")" -ge 2 ] || fail "the server wrote no statistics"
echo "  -> PASSED"

echo "=== File Transfer Test Passed! ==="
exit 0
