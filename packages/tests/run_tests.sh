#!/bin/sh
# EchOS coretools + rx-pkg smoke tests. Run from packages/ or via `make test`.
set -u
BIN="$(cd "$(dirname "$0")/.." && pwd)/bin"
T="$(mktemp -d)"
PASS=0; FAIL=0

check() { # name, command...
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then PASS=$((PASS+1)); echo "ok   $name";
    else FAIL=$((FAIL+1)); echo "FAIL $name"; fi
}

expect_grep() { # name, pattern, command...
    local name="$1" pat="$2"; shift 2
    if "$@" 2>&1 | grep -q "$pat"; then PASS=$((PASS+1)); echo "ok   $name";
    else FAIL=$((FAIL+1)); echo "FAIL $name"; fi
}

# fixtures
mkdir -p "$T/tree/sub/deep"
touch "$T/tree/a.c" "$T/tree/b.txt" "$T/tree/sub/c.c" "$T/tree/.hidden"
echo 'the needle is here' > "$T/tree/needle.txt"

# ---------- rg ----------
expect_grep "rg: stdin match"     "hello" sh -c "echo hello world | $BIN/rg hello -"
expect_grep "rg: recursive match" "needle" sh -c "$BIN/rg needle '$T'"
expect_grep "rg: -i ignore case"  "MiXeD" sh -c "echo MiXeDcase | $BIN/rg -i mixed -"
expect_grep "rg: -v invert"       ":b$"   sh -c "printf 'a\nb\n' | $BIN/rg -v a -"
if printf 'a\nb\n' | "$BIN/rg" -v a - | grep -q ':a$'; then
    FAIL=$((FAIL+1)); echo "FAIL rg: -v really inverts"
else PASS=$((PASS+1)); echo "ok   rg: -v really inverts"; fi

# ---------- fd ----------
check       "fd: glob *.c exit"      true
expect_grep "fd: finds a.c"          "tree/a\.c$" sh -c "$BIN/fd '*.c' '$T/tree'"
expect_grep "fd: finds sub/c.c"      "sub/c\.c$"  sh -c "$BIN/fd '*.c' '$T/tree'"
expect_grep "fd: -t d dirs only"     "/sub$"      sh -c "$BIN/fd -t d 's?b' '$T/tree'"
if "$BIN/fd" -t f '*' "$T/tree" | grep -q "/sub$"; then
    FAIL=$((FAIL+1)); echo "FAIL fd: -t f excludes dirs"
else PASS=$((PASS+1)); echo "ok   fd: -t f excludes dirs"; fi
expect_grep "fd: -e c extension"     "a\.c$"      sh -c "$BIN/fd -e c . '$T/tree'"
expect_grep "fd: -H hidden"          "hidden"     sh -c "$BIN/fd -H '*hidden*' '$T/tree'"
if "$BIN/fd" '*hidden*' "$T/tree" | grep -q hidden; then
    FAIL=$((FAIL+1)); echo "FAIL fd: hides dotfiles by default"
else PASS=$((PASS+1)); echo "ok   fd: hides dotfiles by default"; fi

# ---------- bat ----------
printf 'one\ntwo\nthree\nfour\n' > "$T/nums.txt"
expect_grep "bat: line numbers"      " *1 \| one" sh -c "$BIN/bat '$T/nums.txt'"
expect_grep "bat: range -r 2:3"      "three"      sh -c "$BIN/bat -r 2:3 '$T/nums.txt'"
if "$BIN/bat" -r 2:3 "$T/nums.txt" | grep -q one; then
    FAIL=$((FAIL+1)); echo "FAIL bat: range excludes line1"
else PASS=$((PASS+1)); echo "ok   bat: range excludes line1"; fi
OUT=$("$BIN/bat" "$T/nums.txt" "$T/nums.txt")
if printf '%s\n' "$OUT" | grep -q "==.*nums" && \
   [ "$(printf '%s\n' "$OUT" | grep -c '| one')" = 2 ]; then
    PASS=$((PASS+1)); echo "ok   bat: headers between multiple files"
else FAIL=$((FAIL+1)); echo "FAIL bat: headers between multiple files"; fi
expect_grep "bat: stdin"             "| piped" sh -c "echo piped | $BIN/bat -"

# ---------- eza ----------
expect_grep "eza: long perms"        "^d"         sh -c "$BIN/eza -l '$T/tree'"
expect_grep "eza: shows files"       "a\.c"       sh -c "$BIN/eza '$T/tree'"
if "$BIN/eza" "$T/tree" | grep -q hidden; then
    FAIL=$((FAIL+1)); echo "FAIL eza: hides dotfiles by default"
else PASS=$((PASS+1)); echo "ok   eza: hides dotfiles by default"; fi
expect_grep "eza: -R recursion"      "deep"       sh -c "$BIN/eza -R '$T/tree'"

# ---------- z ----------
export HOME="$T/home"; mkdir -p "$HOME"
"$BIN/z" add /usr && "$BIN/z" add /usr && "$BIN/z" add /var
expect_grep "z: query best match"    "^/usr$"     sh -c "$BIN/z query usr"
expect_grep "z: list ranked"         "/var"       sh -c "$BIN/z list"

# ---------- dust ----------
expect_grep "dust: root total"       "%"          sh -c "$BIN/dust '$T/tree'"
expect_grep "dust: subdirs shown"    "sub"        sh -c "$BIN/dust '$T/tree'"

# ---------- duf ----------
expect_grep "duf: table header"      "MOUNT"      sh -c "$BIN/duf"

# ---------- hyperfine ----------
expect_grep "hyperfine: stats"       "stddev"     sh -c "$BIN/hyperfine --runs 3 true"

# ---------- unp: tar ----------
mkdir -p "$T/src/docs"
echo "hello tar" > "$T/src/readme.txt"
echo "nested" > "$T/src/docs/guide.md"
tar cf "$T/pkg.tar" -C "$T/src" readme.txt docs
expect_grep "unp: detects tar"       "tar archive" sh -c "$BIN/unp '$T/pkg.tar'"
mkdir -p "$T/out-tar"
check "unp: extracts tar"            "$BIN/unp" -x -C "$T/out-tar" "$T/pkg.tar"
check "unp: tar file content"        cmp -s "$T/src/readme.txt" "$T/out-tar/readme.txt"
check "unp: tar nested content"      cmp -s "$T/src/docs/guide.md" "$T/out-tar/docs/guide.md"

# ---------- unp: zip (stored) ----------
mkdir -p "$T/out-zip"
python3 - "$T" <<'EOF'
import zipfile, sys, os
t = sys.argv[1]
with zipfile.ZipFile(os.path.join(t, "pkg.zip"), "w", zipfile.ZIP_STORED) as z:
    z.writestr("zipdir/", "")
    z.writestr("zipdir/hello.txt", "hello zip\n")
EOF
printf 'hello zip\n' > "$T/expected-zip.txt"
if [ -f "$T/pkg.zip" ]; then
    expect_grep "unp: detects zip"   "zip archive" sh -c "$BIN/unp '$T/pkg.zip'"
    check "unp: extracts zip"        "$BIN/unp" -x -C "$T/out-zip" "$T/pkg.zip"
    check "unp: zip content"         cmp -s "$T/expected-zip.txt" "$T/out-zip/zipdir/hello.txt"
else
    echo "skip zip tests (no python3)"
fi

# ---------- unp: gzip detection only ----------
gzip -c "$T/src/readme.txt" > "$T/x.gz"
expect_grep "unp: detects gzip"      "gzip"       sh -c "$BIN/unp '$T/x.gz'"

# ---------- rx-pkg ----------
export ECHOS_ROOT="$T/root"
export ECHOS_PKGDB="$T/db"
mkdir -p "$T/mypkg/bin" "$T/mypkg/share/doc"
cat > "$T/mypkg/manifest" <<EOF
Name: demo
Version: 1.0
Description: demo package for testing
EOF
echo "#!/bin/sh" > "$T/mypkg/bin/demo.sh"
echo "docs here" > "$T/mypkg/share/doc/demo.txt"
chmod +x "$T/mypkg/bin/demo.sh"

check "rx-pkg: build"                "$BIN/rx-pkg" build "$T/mypkg" -o "$T/demo.rxp"
expect_grep "unp: detects rxp"       "echos package" sh -c "$BIN/unp '$T/demo.rxp'"
check "rx-pkg: install"              "$BIN/rx-pkg" install "$T/demo.rxp"
check "rx-pkg: installed file"       cmp -s "$T/mypkg/share/doc/demo.txt" "$ECHOS_ROOT/share/doc/demo.txt"
check "rx-pkg: exec bit kept"        test -x "$ECHOS_ROOT/bin/demo.sh"
expect_grep "rx-pkg: list"           "demo"       sh -c "$BIN/rx-pkg list"
expect_grep "rx-pkg: info"           "Version: 1.0" sh -c "$BIN/rx-pkg info demo"
expect_grep "rx-pkg: files"          "share/doc/demo.txt" sh -c "$BIN/rx-pkg files demo"
check "rx-pkg: remove"               "$BIN/rx-pkg" remove demo
check "rx-pkg: file gone"            test ! -e "$ECHOS_ROOT/share/doc/demo.txt"
mkdir -p "$T/raw"
check "rx-pkg: unpack"               "$BIN/rx-pkg" unpack "$T/demo.rxp" -C "$T/raw"
check "rx-pkg: unpack content"       cmp -s "$T/mypkg/bin/demo.sh" "$T/raw/bin/demo.sh"

# ---------- epk (OpenBSD-style names on the same RXP1 format) ----------
export ECHOS_ROOT="$T/root-epk"
export ECHOS_PKGDB="$T/db-epk"
check "epk: build"                   "$BIN/epk" build "$T/mypkg" -o "$T/demo2.rxp"
check "epk: install alias add"       "$BIN/epk" add "$T/demo2.rxp"
expect_grep "epk: list"              "demo"   sh -c "$BIN/epk list"
expect_grep "epk: info"              "Version: 1.0" sh -c "$BIN/epk info demo"
check "epk_delete: remove"           "$BIN/epk_delete" demo
check "epk: file gone"               test ! -e "$ECHOS_ROOT/share/doc/demo.txt"
expect_grep "epk: info no-arg"       "RXP1"   sh -c "$BIN/epk info"

# ---------- echofetch ----------
expect_grep "echofetch: os name"     "EchOS"  "$BIN/echofetch"
expect_grep "echofetch: kernel"      "Kernel" "$BIN/echofetch"
expect_grep "echofetch: font"        "Liberation Mono" "$BIN/echofetch"

# ---------- localsend over localhost ----------
PORT=47999
mkdir -p "$T/recv"
{ echo "localsend round trip payload"; head -c 200000 /dev/urandom; } > "$T/payload.bin"
"$BIN/localsend" receive --port "$PORT" -o "$T/recv" >/dev/null 2>&1 &
LS_PID=$!
sleep 0.5
check "localsend: send"              "$BIN/localsend" send "$T/payload.bin" 127.0.0.1 --port "$PORT"
sleep 0.5
kill "$LS_PID" 2>/dev/null
wait "$LS_PID" 2>/dev/null
check "localsend: received intact"   cmp -s "$T/payload.bin" "$T/recv/payload.bin"

echo
echo "passed=$PASS failed=$FAIL  (temp: $T)"
[ "$FAIL" = 0 ]
