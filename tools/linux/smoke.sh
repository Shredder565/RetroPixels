#!/bin/sh
# Smoke test of the Linux download, as a player would start it:
#
#   sh tools/linux/smoke.sh dist/RetroPixels-2.21.0-linux-test12.tar.gz
#
# Unpacks the tar.gz into a fresh folder and, with HOME and the XDG folders
# pointed inside it (nothing is written to the real ~/.config or
# ~/.local), checks that:
#   - every program and library in it finds what it loads (ldd);
#   - Supermodel, when it is in it, starts (-help, -print-games, OpenGL);
#   - ./RetroPixels starts, answers /ping and opens its window: a
#     Chromium-based browser in --app mode on the page, with its profile in
#     $XDG_DATA_HOME/RetroPixels/BrowserProfile;
#   - the page loads without script errors and checks in (/api/heartbeat);
#   - a second ./RetroPixels opens another window on the first copy and ends;
#   - closing one window leaves the app running, closing the last one ends it.
#
# Without $DISPLAY it runs itself under xvfb-run. The browser is the first of
# google-chrome, google-chrome-stable, chromium, chromium-browser, or
# $SMOKE_BROWSER; it is started through a small wrapper (RETROPIXELS_BROWSER)
# that adds --remote-debugging-port=0, so the test can look into the page
# through the DevTools protocol. Needs curl and Node.js 22 or later (its
# built-in WebSocket).
set -eu

[ $# -eq 1 ] && [ -f "$1" ] || { sed -n '2,24p' "$0" >&2; exit 2; }
tarball=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")

if [ -z "${DISPLAY:-}" ]; then
	command -v xvfb-run >/dev/null || { echo "smoke.sh: no DISPLAY and no xvfb-run" >&2; exit 1; }
	exec xvfb-run -a -s '-screen 0 1280x1024x24' sh "$self" "$tarball"
fi

port=47821
log() { printf '== %s\n' "$*"; }
fail() {
	printf 'SMOKE TEST FAILED: %s\n' "$*" >&2
	for f in "$t/app.log" "$t/app2.log" "$t/browser.log"; do
		if [ -s "$f" ]; then
			echo "--- $f" >&2
			tail -n 40 "$f" >&2
		fi
	done
	exit 1
}

browser=${SMOKE_BROWSER:-}
if [ -z "$browser" ]; then
	for b in google-chrome google-chrome-stable chromium chromium-browser; do
		if command -v "$b" >/dev/null; then
			browser=$(command -v "$b")
			break
		fi
	done
fi
[ -n "$browser" ] || { echo "smoke.sh: no Chromium-based browser (set SMOKE_BROWSER)" >&2; exit 1; }
command -v node >/dev/null || { echo "smoke.sh: needs Node.js 22+" >&2; exit 1; }
node -e 'if (typeof WebSocket !== "function") process.exit(1)' || { echo "smoke.sh: Node.js has no WebSocket (needs 22+)" >&2; exit 1; }
if curl -fs -m 2 "http://127.0.0.1:$port/ping" >/dev/null 2>&1; then
	echo "smoke.sh: something already answers on port $port (a RetroPixels running?)" >&2
	exit 1
fi

t=$(mktemp -d)
app=""
cleanup() {
	# Only what this test started, by pid.
	if [ -n "$app" ] && kill -0 "$app" 2>/dev/null; then kill "$app" 2>/dev/null || true; fi
	if [ -f "$t/browser.pid" ]; then
		bp=$(cat "$t/browser.pid")
		if kill -0 "$bp" 2>/dev/null; then kill "$bp" 2>/dev/null || true; fi
	fi
	sleep 1
	rm -rf "$t"
}
trap cleanup EXIT

log "unpacking $(basename "$tarball")"
mkdir -p "$t/pkg" "$t/home" "$t/bin"
tar -xzf "$tarball" -C "$t/pkg"
dir=$(find "$t/pkg" -mindepth 1 -maxdepth 1 -type d)
[ -x "$dir/RetroPixels" ] || fail "no executable RetroPixels in the tar.gz"
[ -f "$dir/VERSION.txt" ] || fail "no VERSION.txt in the tar.gz"
export HOME="$t/home" XDG_CONFIG_HOME="$t/home/.config" XDG_DATA_HOME="$t/home/.local/share" XDG_CACHE_HOME="$t/home/.cache"
unset XDG_CURRENT_DESKTOP

log "libraries"
find "$dir" -type f -perm -u+x | while IFS= read -r f; do
	if [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ] && ldd "$f" >"$t/ldd" 2>&1; then
		if grep 'not found' "$t/ldd"; then
			echo "${f#"$dir"/}" >>"$t/missing"
		fi
	fi
done
find "$dir" -type f -name '*.so*' | while IFS= read -r f; do
	if ldd "$f" 2>/dev/null | grep 'not found'; then
		echo "${f#"$dir"/}" >>"$t/missing"
	fi
done
[ ! -s "$t/missing" ] || fail "missing libraries for: $(tr '\n' ' ' <"$t/missing")"

if [ -x "$dir/Supermodel/supermodel" ]; then
	log "Supermodel"
	(
		cd "$dir/Supermodel"
		export SUPERMODEL_HOME="$t/smhome"
		./supermodel -help >"$t/sm-help.txt"
		grep -q -- '-rp-record' "$t/sm-help.txt"
		./supermodel -print-games | grep -q swtrilgya
		grep -q 'retropixels-1' "$SUPERMODEL_HOME/Log/Supermodel.log"
		LIBGL_ALWAYS_SOFTWARE=1 ./supermodel -print-gl-info >"$t/sm-gl.txt" 2>&1
		grep -q 'Version' "$t/sm-gl.txt"
	) || fail "Supermodel doesn't start"
	head -n 3 "$t/sm-gl.txt"
fi

# The browser wrapper: logs each start, adds the DevTools port (written by
# the browser to DevToolsActivePort in its profile) and, as root (in a
# container), --no-sandbox.
cat >"$t/bin/smoke-browser" <<EOF
#!/bin/sh
echo "start: \$*" >>"$t/browser.log"
extra=""
[ "\$(id -u)" = 0 ] && extra="--no-sandbox"
[ -f "$t/browser.pid" ] || echo \$\$ >"$t/browser.pid"
exec "$browser" --remote-debugging-port=0 --disable-gpu --disable-dev-shm-usage --password-store=basic \$extra "\$@" 2>>"$t/browser.log"
EOF
chmod +x "$t/bin/smoke-browser"
export RETROPIXELS_BROWSER="$t/bin/smoke-browser"

# The DevTools helper: node cdp.js <devtools port> <command>.
cat >"$t/cdp.js" <<'EOF'
const [port, cmd] = process.argv.slice(2);
const base = `http://127.0.0.1:${port}`;
const page = "http://127.0.0.1:47821/";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function pages() {
  const list = await (await fetch(`${base}/json/list`)).json();
  return list.filter((t) => t.type === "page" && t.url.startsWith(page));
}
function session(wsUrl) {
  const ws = new WebSocket(wsUrl);
  let id = 0;
  const waiting = new Map();
  const events = [];
  ws.onmessage = (m) => {
    const msg = JSON.parse(m.data);
    if (msg.id && waiting.has(msg.id)) {
      waiting.get(msg.id)(msg);
      waiting.delete(msg.id);
    } else if (msg.method) events.push(msg);
  };
  const ready = new Promise((ok, bad) => { ws.onopen = ok; ws.onerror = () => bad(new Error("DevTools connection failed")); });
  const send = (method, params = {}) => new Promise((ok) => { const i = ++id; waiting.set(i, ok); ws.send(JSON.stringify({ id: i, method, params })); });
  return { ready, send, events, close: () => ws.close() };
}
async function main() {
  if (cmd === "count") { console.log((await pages()).length); return; }
  if (cmd === "close-one") {
    const p = (await pages())[0];
    if (!p) throw new Error("no window to close");
    await fetch(`${base}/json/close/${p.id}`);
    console.log("closed", p.id);
    return;
  }
  if (cmd === "check") {
    const p = (await pages())[0];
    if (!p) throw new Error("no RetroPixels page in the browser");
    const s = session(p.webSocketDebuggerUrl);
    await s.ready;
    await s.send("Runtime.enable");
    await s.send("Network.enable");
    await s.send("Page.enable");
    // Load it again with everything watched from the start.
    await s.send("Page.reload", { ignoreCache: true });
    let beat = null;
    for (let i = 0; i < 100 && !beat; i++) {
      await sleep(200);
      beat = s.events.find((e) => e.method === "Network.responseReceived" && e.params.response.url.endsWith("/api/heartbeat"));
    }
    if (!beat) throw new Error("the page didn't check in (/api/heartbeat) within 20 s");
    if (beat.params.response.status !== 204) throw new Error(`/api/heartbeat answered ${beat.params.response.status}`);
    await sleep(1500);
    const r = await s.send("Runtime.evaluate", {
      expression: `JSON.stringify({ title: document.title, ready: document.readyState, desktop: !!window.RETRO_DESKTOP, version: window.RETRO_DESKTOP && window.RETRO_DESKTOP.version, os: window.RETRO_DESKTOP && window.RETRO_DESKTOP.os, text: document.body.innerText.length })`,
      returnByValue: true,
    });
    const info = JSON.parse(r.result.result.value);
    console.log("page:", JSON.stringify(info));
    const errors = s.events.filter((e) => e.method === "Runtime.exceptionThrown").map((e) => {
      const d = e.params.exceptionDetails;
      return `${(d.exception && d.exception.description) || d.text} (${d.url || ""}:${d.lineNumber})`;
    });
    const failed = s.events.filter((e) => e.method === "Network.responseReceived" && e.params.response.url.startsWith(page) && e.params.response.status >= 400)
      .map((e) => `${e.params.response.status} ${e.params.response.url}`);
    s.close();
    if (errors.length) throw new Error("script errors in the page:\n  " + errors.join("\n  "));
    if (failed.length) throw new Error("page requests that failed:\n  " + failed.join("\n  "));
    if (!info.desktop || !info.version) throw new Error("the page has no window.RETRO_DESKTOP (not served by RetroPixels?)");
    if (info.ready !== "complete" || info.text < 20) throw new Error("the page didn't finish loading or shows nothing");
    if (!/RetroPixels/.test(info.title)) throw new Error(`unexpected page title "${info.title}"`);
    return;
  }
  throw new Error("unknown command " + cmd);
}
main().then(() => process.exit(0), (e) => { console.error(e.message); process.exit(1); });
EOF

# wait_for <seconds> <command...>: until the command succeeds.
wait_for() {
	n=$(($1 * 5))
	shift
	while [ "$n" -gt 0 ]; do
		if "$@" >/dev/null 2>&1; then return 0; fi
		n=$((n - 1))
		sleep 0.2
	done
	return 1
}
pinged() { [ "$(curl -fs -m 2 "http://127.0.0.1:$port/ping")" = RetroPixels ]; }
profile="$XDG_DATA_HOME/RetroPixels/BrowserProfile"
devtools() { head -n 1 "$profile/DevToolsActivePort"; }
windows() { node "$t/cdp.js" "$(devtools)" count; }
windows_are() { [ "$(windows)" = "$1" ]; }
alive() { kill -0 "$app" 2>/dev/null; }

log "starting RetroPixels (browser: $browser)"
(cd "$t/home" && exec "$dir/RetroPixels" >"$t/app.log" 2>&1) &
app=$!
wait_for 20 pinged || fail "RetroPixels didn't answer on port $port"
wait_for 40 test -s "$profile/DevToolsActivePort" || fail "the app window didn't open (no browser profile in \$XDG_DATA_HOME/RetroPixels)"
wait_for 30 windows_are 1 || fail "the window doesn't show the RetroPixels page"
grep -q -- "--app=http://127.0.0.1:$port/" "$t/browser.log" || fail "the browser wasn't started in app mode on the page"
grep -q -- "--class=RetroPixels" "$t/browser.log" || fail "the window has no RetroPixels class"

log "the page"
node "$t/cdp.js" "$(devtools)" check || fail "the page check failed"

log "a second start opens another window"
"$dir/RetroPixels" >"$t/app2.log" 2>&1 &
second=$!
wait_for 15 sh -c "! kill -0 $second 2>/dev/null" || fail "the second RetroPixels didn't end"
wait "$second" || fail "the second RetroPixels ended with an error"
[ "$(grep -c '^start:' "$t/browser.log")" = 2 ] || fail "the second start didn't open a window"
wait_for 20 windows_are 2 || fail "there is no second window on the page"
alive || fail "the first RetroPixels ended"

log "closing the windows"
node "$t/cdp.js" "$(devtools)" close-one
wait_for 10 windows_are 1 || fail "the window didn't close"
sleep 14 # longer than the app waits after a goodbye from the page
alive || fail "RetroPixels ended while a window was still open"
pinged || fail "RetroPixels stopped answering while a window was still open"
node "$t/cdp.js" "$(devtools)" close-one
wait_for 40 sh -c "! kill -0 $app 2>/dev/null" || fail "RetroPixels didn't end after its last window closed"
code=0
wait "$app" || code=$?
app=""
[ "$code" = 0 ] || fail "RetroPixels ended with exit code $code"
if pinged; then fail "something still answers on port $port"; fi
log "smoke test passed"
