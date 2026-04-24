#!/usr/bin/env bash

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR" || exit 1

TMP_DIR="$(mktemp -d /tmp/webserv-eval.XXXXXX)"
SERVER_PID=""
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

report() {
	local status="$1"
	local message="$2"
	if [[ "$status" == "PASS" ]]; then
		PASS_COUNT=$((PASS_COUNT + 1))
	elif [[ "$status" == "FAIL" ]]; then
		FAIL_COUNT=$((FAIL_COUNT + 1))
	else
		SKIP_COUNT=$((SKIP_COUNT + 1))
	fi
	printf "[%s] %s\n" "$status" "$message"
}

stop_server() {
	if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
		kill -INT "$SERVER_PID" 2>/dev/null || true
		wait "$SERVER_PID" 2>/dev/null || true
	fi
	SERVER_PID=""
}

cleanup() {
	stop_server
	pkill -x webserv >/dev/null 2>&1 || true
	rm -rf "$TMP_DIR"
}

trap cleanup EXIT

wait_http_ready() {
	local url="$1"
	local attempt
	for attempt in $(seq 1 40); do
		if curl -s --max-time 1 "$url" >/dev/null 2>&1; then
			return 0
		fi
		if [[ -n "$SERVER_PID" ]] && ! kill -0 "$SERVER_PID" 2>/dev/null; then
			return 1
		fi
		sleep 0.2
	done
	return 1
}

start_server() {
	local config_file="$1"
	local probe_port="$2"

	stop_server
	pkill -x webserv >/dev/null 2>&1 || true

	./webserv "$config_file" >"$TMP_DIR/server.log" 2>&1 &
	SERVER_PID=$!
	wait_http_ready "http://127.0.0.1:${probe_port}/"
}

expect_code() {
	local label="$1"
	local expected="$2"
	local code="$3"
	if [[ "$code" == "$expected" ]]; then
		report "PASS" "$label -> HTTP $code"
	else
		report "FAIL" "$label -> expected HTTP $expected, got HTTP $code"
	fi
}

printf "Running webserv evaluation from %s\n" "$ROOT_DIR"

# Build checks
if make clean >"$TMP_DIR/make_clean.log" 2>&1 && make >"$TMP_DIR/make.log" 2>&1; then
	report "PASS" "Build succeeds with make"
else
	report "FAIL" "Build failed (see $TMP_DIR/make.log)"
fi

if make >"$TMP_DIR/make_norelink.log" 2>&1; then
	if grep -q "Nothing to be done" "$TMP_DIR/make_norelink.log" || [[ ! -s "$TMP_DIR/make_norelink.log" ]]; then
		report "PASS" "No unnecessary relink on second make"
	else
		report "FAIL" "Second make produced unexpected output"
	fi
else
	report "FAIL" "Second make failed"
fi

# Main config checks
if start_server "./webserv.conf" "8080"; then
	report "PASS" "Server starts with webserv.conf"
else
	report "FAIL" "Server failed to start with webserv.conf"
	report "FAIL" "Startup log: $(tr '\n' ' ' < "$TMP_DIR/server.log")"
fi

code=$(curl -s -o "$TMP_DIR/root.body" -w "%{http_code}" http://127.0.0.1:8080/)
expect_code "GET /" "200" "$code"

curl -s -D "$TMP_DIR/redirect.headers" -o /dev/null http://127.0.0.1:8080/old_site
redirect_code=$(awk 'NR==1 {print $2}' "$TMP_DIR/redirect.headers")
redirect_loc=$(awk 'BEGIN{IGNORECASE=1} /^Location:/ {print $2}' "$TMP_DIR/redirect.headers" | tr -d '\r')
if [[ "$redirect_code" == "301" && "$redirect_loc" == "/" ]]; then
	report "PASS" "Redirect /old_site returns 301 with Location /"
else
	report "FAIL" "Redirect check failed (code=$redirect_code location=$redirect_loc)"
fi

code=$(curl -s -o "$TMP_DIR/missing.body" -w "%{http_code}" http://127.0.0.1:8080/does-not-exist)
if [[ "$code" == "404" ]] && grep -q "404 Custom Error Page" "$TMP_DIR/missing.body"; then
	report "PASS" "Custom 404 page is served"
else
	report "FAIL" "Custom 404 page is not served correctly"
fi

code=$(curl -s -o "$TMP_DIR/cgi.body" -w "%{http_code}" "http://127.0.0.1:8080/cgi-bin/test.py?x=42")
if [[ "$code" == "200" ]] && grep -q "query=x=42" "$TMP_DIR/cgi.body"; then
	report "PASS" "CGI GET works"
else
	report "FAIL" "CGI GET failed"
fi

code=$(curl -s -o "$TMP_DIR/8081.body" -w "%{http_code}" http://127.0.0.1:8081/)
expect_code "GET on second port 8081" "200" "$code"

code=$(curl -s -o "$TMP_DIR/unknown.body" -w "%{http_code}" -X FOO http://127.0.0.1:8080/)
if [[ "$code" == "501" || "$code" == "405" ]]; then
	report "PASS" "Unknown method returns valid error status ($code)"
else
	report "FAIL" "Unknown method returned unexpected status ($code)"
fi

code=$(curl -s -o "$TMP_DIR/followup.body" -w "%{http_code}" http://127.0.0.1:8080/)
expect_code "Follow-up GET after unknown method" "200" "$code"

printf 'hello-eval-script' > "$TMP_DIR/upload.bin"
code=$(curl -s -o "$TMP_DIR/upload_post.body" -w "%{http_code}" -X POST --data-binary @"$TMP_DIR/upload.bin" http://127.0.0.1:8080/uploads/eval_script.txt)
expect_code "Upload POST" "201" "$code"

code=$(curl -s -o "$TMP_DIR/upload_get.body" -w "%{http_code}" http://127.0.0.1:8080/uploads/eval_script.txt)
if [[ "$code" == "200" ]] && grep -q "hello-eval-script" "$TMP_DIR/upload_get.body"; then
	report "PASS" "Upload GET returns uploaded content"
else
	report "FAIL" "Upload GET content mismatch (code=$code)"
fi

code=$(curl -s -o "$TMP_DIR/upload_del.body" -w "%{http_code}" -X DELETE http://127.0.0.1:8080/uploads/eval_script.txt)
expect_code "Upload DELETE" "204" "$code"

code=$(curl -s -o "$TMP_DIR/upload_missing.body" -w "%{http_code}" http://127.0.0.1:8080/uploads/eval_script.txt)
expect_code "GET after DELETE" "404" "$code"

# Body limit
if dd if=/dev/zero of="$TMP_DIR/small.bin" bs=1024 count=1 >/dev/null 2>&1 && dd if=/dev/zero of="$TMP_DIR/big.bin" bs=1024 count=11000 >/dev/null 2>&1; then
	code=$(curl -s -o "$TMP_DIR/body_small.body" -w "%{http_code}" -X POST --data-binary @"$TMP_DIR/small.bin" http://127.0.0.1:8080/uploads/bodylimit_small.txt)
	expect_code "Small body under limit" "201" "$code"

	code=$(curl -s -o "$TMP_DIR/body_big.body" -w "%{http_code}" -X POST --data-binary @"$TMP_DIR/big.bin" http://127.0.0.1:8080/uploads/bodylimit_big.txt)
	expect_code "Big body over limit" "413" "$code"

	curl -s -o /dev/null -X DELETE http://127.0.0.1:8080/uploads/bodylimit_small.txt
else
	report "FAIL" "Could not create body-limit test files"
fi

code=$(curl -s -o "$TMP_DIR/autoindex.body" -w "%{http_code}" http://127.0.0.1:8080/errors/)
if [[ "$code" == "200" ]] && grep -q "Index of /errors/" "$TMP_DIR/autoindex.body"; then
	report "PASS" "Autoindex works on /errors/"
else
	report "FAIL" "Autoindex check failed"
fi

code=$(curl -s -o "$TMP_DIR/del_root.body" -w "%{http_code}" -X DELETE http://127.0.0.1:8080/)
expect_code "DELETE on / blocked by allow_methods" "405" "$code"

stop_server

# Host-header routing on same port
mkdir -p "$TMP_DIR/site_main" "$TMP_DIR/site_alt"
printf 'MAIN_SITE\n' > "$TMP_DIR/site_main/index.html"
printf 'ALT_SITE\n' > "$TMP_DIR/site_alt/index.html"

cat > "$TMP_DIR/vhost.conf" <<EOF
server {
    listen 8092;
    host 127.0.0.1;
    server_name main.local;
    root $TMP_DIR/site_main;
    index index.html;
    location / {
        allow_methods GET;
    }
}
server {
    listen 8092;
    host 127.0.0.1;
    server_name alt.local;
    root $TMP_DIR/site_alt;
    index index.html;
    location / {
        allow_methods GET;
    }
}
EOF

if start_server "$TMP_DIR/vhost.conf" "8092"; then
	report "PASS" "Same-port vhost config starts"
else
	report "FAIL" "Same-port vhost config failed to start"
fi

main_code=$(curl --noproxy '*' --resolve main.local:8092:127.0.0.1 -s -o "$TMP_DIR/main_host.body" -w "%{http_code}" http://main.local:8092/)
alt_code=$(curl --noproxy '*' --resolve alt.local:8092:127.0.0.1 -s -o "$TMP_DIR/alt_host.body" -w "%{http_code}" http://alt.local:8092/)

if [[ "$main_code" == "200" && "$alt_code" == "200" ]] && ! diff "$TMP_DIR/main_host.body" "$TMP_DIR/alt_host.body" >/dev/null 2>&1; then
	report "PASS" "Host-header routing on same port returns different content"
else
	report "FAIL" "Host-header routing failed (main=$main_code alt=$alt_code)"
fi

if grep -Eq "srv \[Bind\]|failed to bind|No valid listening sockets" "$TMP_DIR/server.log"; then
	report "FAIL" "Same-port config produced bind errors"
else
	report "PASS" "Same-port config avoids duplicate bind errors"
fi

stop_server

# Conflict test: second instance should fail
if start_server "./webserv.conf" "8080"; then
	report "PASS" "Primary instance for conflict test started"
else
	report "FAIL" "Primary instance for conflict test did not start"
fi

cat > "$TMP_DIR/conflict.conf" <<EOF
server {
    listen 8080;
    host 127.0.0.1;
    root ./www;
    index index.html;
    location / {
        allow_methods GET;
    }
}
EOF

if timeout 3s ./webserv "$TMP_DIR/conflict.conf" > "$TMP_DIR/conflict.log" 2>&1; then
	report "FAIL" "Second instance unexpectedly started on conflicting port"
else
	status=$?
	if [[ "$status" == "124" ]]; then
		report "FAIL" "Second instance hung instead of failing on port conflict"
	elif grep -Eq "srv \[Bind\]|failed to bind|No valid listening sockets" "$TMP_DIR/conflict.log"; then
		report "PASS" "Second instance fails on conflicting port as expected"
	else
		report "FAIL" "Second instance failed but conflict reason was unclear"
	fi
fi

stop_server

# Optional stress/leak tools
if command -v siege >/dev/null 2>&1; then
	if start_server "./webserv.conf" "8080"; then
		siege_out=$(siege -b -t5S http://127.0.0.1:8080/ 2>&1)
		availability=$(printf "%s\n" "$siege_out" | awk -F: '/Availability/ {gsub(/^[ \t]+/, "", $2); print $2; exit}')
		failed_tx=$(printf "%s\n" "$siege_out" | awk -F: '/Failed transactions/ {gsub(/^[ \t]+/, "", $2); print $2; exit}')
		if [[ -n "$availability" ]]; then
			report "PASS" "Siege ran (Availability=$availability, Failed=$failed_tx)"
		else
			report "FAIL" "Siege output did not include expected summary"
		fi
	else
		report "FAIL" "Could not start server for siege test"
	fi
	stop_server
else
	report "SKIP" "siege not installed"
fi

if command -v valgrind >/dev/null 2>&1; then
	pkill -x webserv >/dev/null 2>&1 || true
	valgrind --leak-check=full --show-leak-kinds=all ./webserv ./webserv.conf > "$TMP_DIR/valgrind.log" 2>&1 &
	SERVER_PID=$!
	if wait_http_ready "http://127.0.0.1:8080/"; then
		curl -s http://127.0.0.1:8080/ >/dev/null
	fi
	stop_server
	if grep -q "definitely lost: 0 bytes" "$TMP_DIR/valgrind.log" || grep -q "All heap blocks were freed -- no leaks are possible" "$TMP_DIR/valgrind.log"; then
		report "PASS" "Valgrind: definitely lost is 0 bytes"
	else
		report "FAIL" "Valgrind leak summary is not clean"
	fi
else
	report "SKIP" "valgrind not installed"
fi

printf "\nSummary: PASS=%d FAIL=%d SKIP=%d\n" "$PASS_COUNT" "$FAIL_COUNT" "$SKIP_COUNT"
if [[ "$FAIL_COUNT" -gt 0 ]]; then
	exit 1
fi
exit 0
