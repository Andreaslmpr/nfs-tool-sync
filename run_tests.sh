#!/usr/bin/env bash
#
# run_tests.sh - Functional test suite for the NFS Sync Tool.
#
# Usage:  ./run_tests.sh [--valgrind]
#
# Exercises every documented feature of nfs_manager / nfs_client / nfs_console:
# CLI validation, config parsing, the LIST/PULL/PUSH wire protocol, file
# integrity, concurrency, bounded-buffer back-pressure, console commands,
# graceful shutdown, error handling and log formats.
#
set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/nfs_tests.XXXXXX")"
PORT_BASE=${PORT_BASE:-7700}
RUN_VALGRIND=0
[ "${1:-}" = "--valgrind" ] && RUN_VALGRIND=1

PASS=0; FAIL=0; SECTION=""

cleanup() {
    pkill -9 -f "$ROOT/nfs_client"  >/dev/null 2>&1
    pkill -9 -f "$ROOT/nfs_manager" >/dev/null 2>&1
    rm -rf "$WORK"
}
trap cleanup EXIT

section() { SECTION="$1"; echo; echo "== $1 =="; }
ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); }
check_eq() { # desc, actual, expected
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 -- expected '$3', got '$2'"; fi
}
check_grep() { # desc, file, pattern
    if grep -qE "$3" "$2" 2>/dev/null; then ok "$1"; else bad "$1 -- pattern not found: $3"; fi
}
check_not_grep() {
    if grep -qE "$3" "$2" 2>/dev/null; then bad "$1 -- unexpected pattern: $3"; else ok "$1"; fi
}
alive() { kill -0 "$1" 2>/dev/null; }

# Waits until a TCP port accepts connections (max ~5s)
wait_port() {
    for _ in $(seq 1 50); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; then exec 3<&- 3>&-; return 0; fi
        sleep 0.1
    done
    return 1
}

echo "NFS Sync Tool - test suite"
echo "workdir: $WORK"

# ---------------------------------------------------------------- 1. BUILD --
section "1. Build"
BUILD_OUT="$WORK/build.log"
( cd "$ROOT" && make clean >/dev/null 2>&1 && make all ) >"$BUILD_OUT" 2>&1
check_eq "make all succeeds" "$?" "0"
check_eq "compiler warnings" "$(grep -ci 'warning' "$BUILD_OUT")" "0"
for b in nfs_manager nfs_client nfs_console; do
    [ -x "$ROOT/$b" ] && ok "$b built" || bad "$b missing"
done

# ------------------------------------------------------- 2. CLI VALIDATION --
section "2. Command line validation"
"$ROOT/nfs_manager" >/dev/null 2>&1;                        check_eq "nfs_manager without args exits 1" "$?" "1"
"$ROOT/nfs_manager" -l a -c b -n x -p 1 -b 1 >/dev/null 2>&1; check_eq "nfs_manager rejects non numeric -n" "$?" "1"
"$ROOT/nfs_manager" -l a -c b -n 1 -p 1 >/dev/null 2>&1;    check_eq "nfs_manager rejects missing -b" "$?" "1"
"$ROOT/nfs_client"  >/dev/null 2>&1;                        check_eq "nfs_client without args exits 1" "$?" "1"
"$ROOT/nfs_client"  -p abc >/dev/null 2>&1;                 check_eq "nfs_client rejects non numeric port" "$?" "1"
"$ROOT/nfs_console" -h 127.0.0.1 -p 1 >/dev/null 2>&1;      check_eq "nfs_console rejects missing -l" "$?" "1"
"$ROOT/nfs_console" -l a -h 127.0.0.1 -p 0 >/dev/null 2>&1; check_eq "nfs_console rejects port 0" "$?" "1"

# --------------------------------------------------------- 3. CONFIG FILE --
section "3. Config file parsing"
"$ROOT/nfs_manager" -l "$WORK/x.log" -c "$WORK/does_not_exist.cfg" -n 1 -p $((PORT_BASE)) -b 1 >/dev/null 2>&1
check_eq "missing config file exits 1" "$?" "1"
printf 'garbage line without at sign\n' > "$WORK/bad.cfg"
"$ROOT/nfs_manager" -l "$WORK/x.log" -c "$WORK/bad.cfg" -n 1 -p $((PORT_BASE)) -b 1 >/dev/null 2>&1
check_eq "malformed config exits 1" "$?" "1"
printf '/a@127.0.0.1:70000 /b@127.0.0.1:1\n' > "$WORK/badport.cfg"
"$ROOT/nfs_manager" -l "$WORK/x.log" -c "$WORK/badport.cfg" -n 1 -p $((PORT_BASE)) -b 1 >/dev/null 2>&1
check_eq "out of range port exits 1" "$?" "1"

# CRLF + comments + blank lines must parse cleanly
mkdir -p "$WORK/crlf_src" "$WORK/crlf_tgt"; echo hi > "$WORK/crlf_src/a.txt"
printf '# comment\r\n\r\n%s@127.0.0.1:%d %s@127.0.0.1:%d\r\n' \
    "$WORK/crlf_src" $((PORT_BASE+1)) "$WORK/crlf_tgt" $((PORT_BASE+2)) > "$WORK/crlf.cfg"
"$ROOT/nfs_client" -p $((PORT_BASE+1)) -l "$WORK/crlf_c1.log" >/dev/null 2>&1 &
"$ROOT/nfs_client" -p $((PORT_BASE+2)) -l "$WORK/crlf_c2.log" >/dev/null 2>&1 &
wait_port $((PORT_BASE+1)); wait_port $((PORT_BASE+2))
"$ROOT/nfs_manager" -l "$WORK/crlf_m.log" -c "$WORK/crlf.cfg" -n 2 -p $((PORT_BASE+3)) -b 4 >/dev/null 2>&1 &
MPID=$!; wait_port $((PORT_BASE+3)); sleep 1
printf 'shutdown\n' | "$ROOT/nfs_console" -l "$WORK/crlf_con.log" -h 127.0.0.1 -p $((PORT_BASE+3)) >/dev/null 2>&1
sleep 1
cmp -s "$WORK/crlf_src/a.txt" "$WORK/crlf_tgt/a.txt" && ok "CRLF config with comments parses and syncs" \
                                                     || bad "CRLF config with comments parses and syncs"
pkill -9 -f "$ROOT/nfs_client" >/dev/null 2>&1; wait 2>/dev/null

# ------------------------------------------------------------ 4. PROTOCOL --
section "4. Wire protocol (direct against nfs_client)"
PP=$((PORT_BASE+10))
mkdir -p "$WORK/proto"; printf 'abcdefghij' > "$WORK/proto/ten.txt"
"$ROOT/nfs_client" -p $PP -l "$WORK/proto.log" >/dev/null 2>&1 &
wait_port $PP
python3 - "$WORK" "$PP" > "$WORK/proto.out" <<'PY'
import socket, sys
W, P = sys.argv[1], int(sys.argv[2])
def conn():
    s = socket.socket(); s.settimeout(3); s.connect(("127.0.0.1", P)); return s
def drain(s):
    # The client keeps the connection open, so read until it goes quiet.
    out = b""
    try:
        while True:
            c = s.recv(65536)
            if not c: break
            out += c
    except socket.timeout:
        pass
    return out
s = conn(); s.sendall(("LIST %s/proto\n" % W).encode()); print("LIST:%r" % drain(s)); s.close()
s = conn(); s.sendall(("PULL %s/proto/ten.txt\n" % W).encode()); print("PULL:%r" % drain(s)); s.close()
s = conn(); s.sendall(("PULL %s/proto/nope\n" % W).encode()); print("ERR:%r" % drain(s)); s.close()
s = conn()
s.sendall(("PUSH %s/proto/out.txt -1\n" % W).encode())
s.sendall(("PUSH %s/proto/out.txt 5 hello" % W).encode())
s.sendall(("PUSH %s/proto/out.txt 6 world!" % W).encode())
s.sendall(("PUSH %s/proto/out.txt 0\n" % W).encode())
s.close()
s = conn(); s.sendall(("PUSH %s/proto/deep/nested/n.txt -1\n" % W).encode())
s.sendall(("PUSH %s/proto/deep/nested/n.txt 2 ok" % W).encode())
s.sendall(("PUSH %s/proto/deep/nested/n.txt 0\n" % W).encode()); s.close()
PY
sleep 1
check_grep "LIST returns filenames terminated by '.'" "$WORK/proto.out" "^LIST:b.ten[.]txt.n[.].n.$"
check_grep "PULL returns <filesize><space><data>"     "$WORK/proto.out" "^PULL:b.10 abcdefghij.$"
check_grep "PULL of missing file returns -1 + reason" "$WORK/proto.out" "ERR:b'-1 No such file or directory"
check_eq   "PUSH writes chunked data in order" "$(cat "$WORK/proto/out.txt" 2>/dev/null)" "helloworld!"
check_eq   "PUSH creates missing directories"  "$(cat "$WORK/proto/deep/nested/n.txt" 2>/dev/null)" "ok"
check_grep "client logs LIST"  "$WORK/proto.log" "Received LIST"
check_grep "client logs PUSH EOF" "$WORK/proto.log" "Received PUSH EOF"
pkill -9 -f "$ROOT/nfs_client" >/dev/null 2>&1; wait 2>/dev/null

# ------------------------------------------------------- 5. SYNC INTEGRITY --
section "5. Synchronisation integrity"
SP=$((PORT_BASE+20)); TP=$((PORT_BASE+21)); MP=$((PORT_BASE+22))
mkdir -p "$WORK/s1" "$WORK/t1" "$WORK/s2" "$WORK/t2" "$WORK/add_s" "$WORK/add_t"
: > "$WORK/s1/empty.txt"
printf 'Hello from syspro2\n'        > "$WORK/s1/small.txt"
head -c 4095   /dev/urandom          > "$WORK/s1/just_under_chunk.bin"
head -c 4096   /dev/urandom          > "$WORK/s1/exact_chunk.bin"
head -c 4097   /dev/urandom          > "$WORK/s1/just_over_chunk.bin"
head -c 3300000 /dev/urandom         > "$WORK/s1/large.bin"
printf 'line with spaces and \t tabs\n' > "$WORK/s1/text.txt"
mkdir -p "$WORK/s1/subdir"; echo nope > "$WORK/s1/subdir/ignored.txt"   # LIST must skip dirs
echo two > "$WORK/s2/second.txt"
echo added1 > "$WORK/add_s/a1.txt"; head -c 50000 /dev/urandom > "$WORK/add_s/a2.bin"

printf '%s@127.0.0.1:%d %s@127.0.0.1:%d\n%s@127.0.0.1:%d %s@127.0.0.1:%d\n' \
    "$WORK/s1" $SP "$WORK/t1" $TP "$WORK/s2" $SP "$WORK/t2" $TP > "$WORK/main.cfg"

"$ROOT/nfs_client" -p $SP -l "$WORK/src_client.log" >/dev/null 2>&1 &
"$ROOT/nfs_client" -p $TP -l "$WORK/tgt_client.log" >/dev/null 2>&1 &
wait_port $SP; wait_port $TP
"$ROOT/nfs_manager" -l "$WORK/manager.log" -c "$WORK/main.cfg" -n 5 -p $MP -b 7 >/dev/null 2>&1 &
MPID=$!
wait_port $MP; sleep 3

for f in empty.txt small.txt just_under_chunk.bin exact_chunk.bin just_over_chunk.bin large.bin text.txt; do
    cmp -s "$WORK/s1/$f" "$WORK/t1/$f" && ok "byte-identical copy: $f" || bad "byte-identical copy: $f"
done
check_eq "second config pair synced" "$(ls "$WORK/t2" | tr '\n' ' ')" "second.txt "
[ -e "$WORK/t1/subdir" ] && bad "subdirectories are not synced" || ok "subdirectories are not synced"
check_eq "empty file stays empty" "$(stat -c%s "$WORK/t1/empty.txt")" "0"

# --------------------------------------------------------- 6. CONCURRENCY --
section "6. Concurrency"
WORKERS_SEEN=$(grep -oE '\] \[[0-9]+\] \[(PULL|PUSH)\]' "$WORK/manager.log" | grep -oE '[0-9]+' | sort -u | wc -l)
[ "$WORKERS_SEEN" -gt 1 ] && ok "multiple worker threads did work ($WORKERS_SEEN distinct thread ids)" \
                          || bad "only $WORKERS_SEEN worker thread id in log"

# ------------------------------------------------------ 7. CONSOLE: add/cancel --
section "7. Console commands"
CON_OUT="$WORK/console.out"
{
  printf 'cancel %s\n' "$WORK/s2";  sleep 1     # plain directory, as in the assignment
  printf 'cancel /nope/nope\n';     sleep 1     # never synchronised
  printf 'add %s@127.0.0.1:%d %s@127.0.0.1:%d\n' "$WORK/add_s" $SP "$WORK/add_t" $TP; sleep 2
  printf 'add %s@127.0.0.1:%d %s@127.0.0.1:%d\n' "$WORK/add_s" $SP "$WORK/add_t" $TP; sleep 1
  printf 'add broken_syntax\n';     sleep 1
  printf 'bogus_command\n';         sleep 1
  printf 'shutdown\n';              sleep 3
} | "$ROOT/nfs_console" -l "$WORK/console.log" -h 127.0.0.1 -p $MP > "$CON_OUT" 2>&1
sleep 2

check_grep "cancel <dir> stops synchronisation"      "$CON_OUT" "Synchronization stopped for $WORK/s2@127.0.0.1:$SP"
check_grep "cancel of unknown dir is reported"       "$CON_OUT" "Directory not being synchronized: /nope/nope"
check_grep "add reports each queued file"            "$CON_OUT" "Added file: $WORK/add_s/a1.txt@127.0.0.1:$SP -> $WORK/add_t/a1.txt@127.0.0.1:$TP"
check_grep "duplicate add is rejected"               "$CON_OUT" "Already in queue: $WORK/add_s@127.0.0.1:$SP"
check_grep "invalid add syntax is reported"          "$CON_OUT" "(Invalid add syntax|Usage: add)"
check_grep "unknown command is reported"             "$CON_OUT" "Unknown command: bogus_command"
cmp -s "$WORK/add_s/a1.txt" "$WORK/add_t/a1.txt" && cmp -s "$WORK/add_s/a2.bin" "$WORK/add_t/a2.bin" \
    && ok "files added at runtime are synced" || bad "files added at runtime are synced"

# ----------------------------------------------------------- 8. SHUTDOWN --
section "8. Graceful shutdown"
check_grep "shutdown msg 1" "$CON_OUT" "Shutting down manager\.\.\."
check_grep "shutdown msg 2" "$CON_OUT" "Waiting for all active workers to finish\."
check_grep "shutdown msg 3" "$CON_OUT" "Processing remaining queued tasks\."
check_grep "shutdown msg 4" "$CON_OUT" "Manager shutdown complete\."
alive $MPID && { bad "manager process exits after shutdown"; kill -9 $MPID 2>/dev/null; } \
             || ok "manager process exits after shutdown"
pkill -9 -f "$ROOT/nfs_client" >/dev/null 2>&1; wait 2>/dev/null

# ------------------------------------------------------- 9. LOG FORMATS --
section "9. Log formats"
TS='\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\]'
check_grep "manager transfer log has 7 bracketed fields" "$WORK/manager.log" \
    "^$TS \[[^]]+@[^]]+:[0-9]+\] \[[^]]+@[^]]+:[0-9]+\] \[[0-9]+\] \[(PULL|PUSH)\] \[(SUCCESS|ERROR)\] \[[^]]*\]$"
check_grep "PULL SUCCESS details"   "$WORK/manager.log" "\[PULL\] \[SUCCESS\] \[[0-9]+ bytes pulled\]"
check_grep "PUSH SUCCESS details"   "$WORK/manager.log" "\[PUSH\] \[SUCCESS\] \[[0-9]+ bytes pushed\]"
check_grep "manager logs Added file" "$WORK/manager.log" "^$TS Added file: .+@.+:[0-9]+ -> .+@.+:[0-9]+$"
check_grep "console logs add with arrow" "$WORK/console.log" "^$TS Command add .+@.+:[0-9]+ -> .+@.+:[0-9]+$"
check_grep "console logs cancel"    "$WORK/console.log" "^$TS Command cancel "
check_grep "console logs shutdown"  "$WORK/console.log" "^$TS Command shutdown$"

# -------------------------------------------- 10. BACK PRESSURE / STRESS --
section "10. Bounded buffer back-pressure (40 files, 1 worker, buffer size 1)"
SP2=$((PORT_BASE+30)); TP2=$((PORT_BASE+31)); MP2=$((PORT_BASE+32))
mkdir -p "$WORK/stress_s" "$WORK/stress_t"
for i in $(seq 1 40); do head -c $((i * 1000)) /dev/urandom > "$WORK/stress_s/f$i.bin"; done
printf '%s@127.0.0.1:%d %s@127.0.0.1:%d\n' "$WORK/stress_s" $SP2 "$WORK/stress_t" $TP2 > "$WORK/stress.cfg"
"$ROOT/nfs_client" -p $SP2 -l "$WORK/st1.log" >/dev/null 2>&1 &
"$ROOT/nfs_client" -p $TP2 -l "$WORK/st2.log" >/dev/null 2>&1 &
wait_port $SP2; wait_port $TP2
"$ROOT/nfs_manager" -l "$WORK/stress_m.log" -c "$WORK/stress.cfg" -n 1 -p $MP2 -b 1 >/dev/null 2>&1 &
MPID2=$!
wait_port $MP2; sleep 12
printf 'shutdown\n' | "$ROOT/nfs_console" -l "$WORK/stress_con.log" -h 127.0.0.1 -p $MP2 >/dev/null 2>&1
sleep 3
BADF=0
for f in "$WORK/stress_s"/*.bin; do cmp -s "$f" "$WORK/stress_t/$(basename "$f")" || BADF=$((BADF+1)); done
check_eq "all 40 files copied correctly with buffer size 1" "$BADF" "0"
alive $MPID2 && { bad "manager exits after stress shutdown"; kill -9 $MPID2 2>/dev/null; } \
              || ok "manager exits after stress shutdown"
pkill -9 -f "$ROOT/nfs_client" >/dev/null 2>&1; wait 2>/dev/null

# ---------------------------------------------------- 11. ERROR HANDLING --
section "11. Error handling"
SP3=$((PORT_BASE+40)); TP3=$((PORT_BASE+41)); MP3=$((PORT_BASE+42))
mkdir -p "$WORK/e_s" "$WORK/e_t"; echo data > "$WORK/e_s/f.txt"
printf '%s@127.0.0.1:%d %s@127.0.0.1:%d\n' "$WORK/e_s" $SP3 "$WORK/e_t" $TP3 > "$WORK/e.cfg"

# 11a - target client down => PUSH ERROR, manager survives
"$ROOT/nfs_client" -p $SP3 -l "$WORK/e_src.log" >/dev/null 2>&1 &
wait_port $SP3
"$ROOT/nfs_manager" -l "$WORK/e_push.log" -c "$WORK/e.cfg" -n 2 -p $MP3 -b 4 >/dev/null 2>&1 &
MPID3=$!; wait_port $MP3; sleep 2
alive $MPID3 && ok "manager survives unreachable target" || bad "manager survives unreachable target"
check_grep "PUSH failure logged as ERROR" "$WORK/e_push.log" "\[PUSH\] \[ERROR\] \[File: f.txt Cannot connect to"
printf 'shutdown\n' | "$ROOT/nfs_console" -l "$WORK/e_con1.log" -h 127.0.0.1 -p $MP3 >/dev/null 2>&1
sleep 2; kill -9 $MPID3 2>/dev/null
pkill -9 -f "$ROOT/nfs_client" >/dev/null 2>&1; wait 2>/dev/null

# 11b - source client down => LIST error, manager survives
"$ROOT/nfs_manager" -l "$WORK/e_pull.log" -c "$WORK/e.cfg" -n 2 -p $MP3 -b 4 >/dev/null 2>&1 &
MPID3=$!; wait_port $MP3; sleep 2
alive $MPID3 && ok "manager survives unreachable source" || bad "manager survives unreachable source"
check_grep "LIST failure logged" "$WORK/e_pull.log" "Error connecting to source .*Connection refused"
printf 'shutdown\n' | "$ROOT/nfs_console" -l "$WORK/e_con2.log" -h 127.0.0.1 -p $MP3 >/dev/null 2>&1
sleep 2
alive $MPID3 && { bad "manager exits with no clients running"; kill -9 $MPID3 2>/dev/null; } \
              || ok "manager exits with no clients running"

# 11c - source answers "-1 <reason>" => PULL ERROR with the reason
FAKE_PORT=$((PORT_BASE+43))
python3 - $FAKE_PORT >/dev/null 2>&1 <<'PY' &
import socket, sys, threading
p = int(sys.argv[1])
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", p)); s.listen(5)
def handle(c):
    buf = b""
    while True:
        b1 = c.recv(1)
        if not b1: break
        buf += b1
        if b1 == b"\n":
            if buf.startswith(b"LIST"): c.sendall(b"secret.txt\n.\n")
            elif buf.startswith(b"PULL"): c.sendall(b"-1 Permission denied\n")
            buf = b""
    c.close()
while True:
    c, _ = s.accept(); threading.Thread(target=handle, args=(c,), daemon=True).start()
PY
FAKE_PID=$!
disown $FAKE_PID 2>/dev/null || true
"$ROOT/nfs_client" -p $TP3 -l "$WORK/e_tgt.log" >/dev/null 2>&1 &
wait_port $FAKE_PORT; wait_port $TP3
printf '/fake@127.0.0.1:%d %s@127.0.0.1:%d\n' $FAKE_PORT "$WORK/e_t" $TP3 > "$WORK/fake.cfg"
"$ROOT/nfs_manager" -l "$WORK/e_perm.log" -c "$WORK/fake.cfg" -n 2 -p $MP3 -b 4 >/dev/null 2>&1 &
MPID3=$!; wait_port $MP3; sleep 2
check_grep "PULL error from source propagated to log" "$WORK/e_perm.log" \
    "\[PULL\] \[ERROR\] \[File: secret.txt Permission denied\]"
alive $MPID3 && ok "manager survives source-side errors" || bad "manager survives source-side errors"
printf 'shutdown\n' | "$ROOT/nfs_console" -l "$WORK/e_con3.log" -h 127.0.0.1 -p $MP3 >/dev/null 2>&1
sleep 2; kill -9 $MPID3 $FAKE_PID 2>/dev/null
pkill -9 -f "$ROOT/nfs_client" >/dev/null 2>&1; wait 2>/dev/null

# 11d - console against a manager that is not running
printf 'add /a@1.2.3.4:1 /b@1.2.3.4:2\n' | \
    "$ROOT/nfs_console" -l "$WORK/e_con4.log" -h 127.0.0.1 -p $((PORT_BASE+49)) > "$WORK/e_con4.out" 2>&1
check_grep "console reports unreachable manager" "$WORK/e_con4.out" "cannot connect to manager"

# 11e - two managers on the same port: the second must fail, not hang
SP5=$((PORT_BASE+50)); MP5=$((PORT_BASE+51))
mkdir -p "$WORK/p_s" "$WORK/p_t"
printf '%s@127.0.0.1:%d %s@127.0.0.1:%d\n' "$WORK/p_s" $SP5 "$WORK/p_t" $((SP5+1)) > "$WORK/p.cfg"
"$ROOT/nfs_manager" -l "$WORK/p1.log" -c "$WORK/p.cfg" -n 1 -p $MP5 -b 2 >/dev/null 2>&1 &
P1=$!; wait_port $MP5
"$ROOT/nfs_manager" -l "$WORK/p2.log" -c "$WORK/p.cfg" -n 1 -p $MP5 -b 2 >/dev/null 2>&1
check_eq "second manager on busy port exits instead of hanging" "$?" "0"
check_grep "busy port is logged" "$WORK/p2.log" "cannot bind command port"
kill -9 $P1 2>/dev/null; wait 2>/dev/null

# ------------------------------------------------------------ 12. MEMORY --
if [ "$RUN_VALGRIND" = "1" ] && command -v valgrind >/dev/null 2>&1; then
section "12. Memory (valgrind)"
SP6=$((PORT_BASE+60)); TP6=$((PORT_BASE+61)); MP6=$((PORT_BASE+62))
mkdir -p "$WORK/v_s" "$WORK/v_t"; head -c 100000 /dev/urandom > "$WORK/v_s/v.bin"
printf '%s@127.0.0.1:%d %s@127.0.0.1:%d\n' "$WORK/v_s" $SP6 "$WORK/v_t" $TP6 > "$WORK/v.cfg"
valgrind --leak-check=full --error-exitcode=42 --log-file="$WORK/vg_client.txt" \
    "$ROOT/nfs_client" -p $SP6 -l "$WORK/v1.log" >/dev/null 2>&1 &
"$ROOT/nfs_client" -p $TP6 -l "$WORK/v2.log" >/dev/null 2>&1 &
wait_port $SP6; wait_port $TP6
valgrind --leak-check=full --error-exitcode=42 --log-file="$WORK/vg_manager.txt" \
    "$ROOT/nfs_manager" -l "$WORK/v_m.log" -c "$WORK/v.cfg" -n 3 -p $MP6 -b 4 >/dev/null 2>&1 &
wait_port $MP6; sleep 6
printf 'shutdown\n' | "$ROOT/nfs_console" -l "$WORK/v_con.log" -h 127.0.0.1 -p $MP6 >/dev/null 2>&1
sleep 6
cmp -s "$WORK/v_s/v.bin" "$WORK/v_t/v.bin" && ok "file synced under valgrind" || bad "file synced under valgrind"
check_grep "manager: no definite leaks" "$WORK/vg_manager.txt" "(definitely lost: 0 bytes in 0 blocks|All heap blocks were freed)"
check_grep "manager: no invalid memory access" "$WORK/vg_manager.txt" "ERROR SUMMARY: 0 errors"
pkill -9 -f "$ROOT/nfs_client" >/dev/null 2>&1; wait 2>/dev/null
fi

# ------------------------------------------------------------- SUMMARY --
echo
echo "=================================================="
printf 'Total: %d   Passed: %d   Failed: %d\n' $((PASS+FAIL)) "$PASS" "$FAIL"
echo "=================================================="
[ "$FAIL" -eq 0 ] || exit 1
