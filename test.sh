#!/bin/bash
# tabli smoke-test suite: builds nothing, expects ./tabli to exist (make first).
set -u
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
cp tabli "$DIR/seed"
cd "$DIR"

PASS=0
FAIL=0
ok()   { PASS=$((PASS+1)); }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; }
check() { # check <desc> <expected-substring> <actual>
  case "$3" in *"$2"*) ok ;; *) fail "$1 — expected '$2' in: $3" ;; esac
}
check_rc() { # check_rc <desc> <expected-rc> <actual-rc>
  [ "$2" = "$3" ] && ok || fail "$1 — expected rc=$2 got rc=$3"
}

# --- seed & init ---
out=$(./seed); check "seed help" "creates self-contained tables" "$out"
out=$(./seed a x=1 2>&1); rc=$?
check_rc "seed refuses verbs" 2 $rc
out=$(./seed init t.tbl)
check "init message" "created t.tbl" "$out"
[ -x t.tbl ] && ok || fail "table is executable"
out=$(./seed init t.tbl 2>&1); rc=$?
check_rc "init refuses overwrite" 2 $rc

# --- help ---
out=$(./t.tbl); check "table help" "data lives inside this file" "$out"

# --- a: echo, ids, timestamps ---
out=$(./t.tbl a title='fix lock' status=open prio=2)
check "a echo id" "id=1 title='fix lock' status=open prio=2" "$out"
check "a echo created_at" "created_at=" "$out"
out=$(./t.tbl a title=second status=open prio=1 by=agent-a)
check "a by= becomes created_by" "created_by=agent-a" "$out"
out=$(./t.tbl a title=third status=done prio=5)
check "a third id" "id=3" "$out"

# --- reserved keys teach ---
out=$(./t.tbl a id=9 x=1 2>&1); rc=$?
check_rc "a rejects id=" 2 $rc
check "id error teaches" "assigned by the engine" "$out"
out=$(./t.tbl a created_at=2020-01-01 x=1 2>&1); rc=$?
check_rc "a rejects created_at" 2 $rc
check "ts error teaches" "engine-owned" "$out"

# --- q: filters, ops, limit, count, ts hiding ---
out=$(./t.tbl q status=open)
check "q eq matches 2" "# 2 records" "$out"
case "$out" in *created_at*) fail "q hides ts by default" ;; *) ok ;; esac
out=$(./t.tbl q status=open ts)
check "q ts flag shows ts" "created_at=" "$out"
out=$(./t.tbl q status!=open)
check "q neq" "id=3" "$out"
out=$(./t.tbl q prio\>1)
check "q gt matches 2" "# 2 records" "$out"
out=$(./t.tbl q prio\>=2)
check "q ge matches 2" "# 2 records" "$out"
out=$(./t.tbl q title~lock)
check "q contains" "id=1" "$out"
out=$(./t.tbl q status=open prio\>1)
check "q AND" "# 1 record" "$out"
out=$(./t.tbl q limit=1)
check "q limit truncation notice" "showing 1 of 3" "$out"
out=$(./t.tbl q count)
check "q count mode" "# 3 records" "$out"
out=$(./t.tbl q status=missing)
check "q empty explicit" "# 0 records match" "$out"
out=$(./t.tbl q staus 2>&1); rc=$?
check_rc "bad filter rc" 2 $rc
check "bad filter teaches" "filters look like" "$out"

# --- quoting round-trip (quotes, $(), spaces) ---
./t.tbl a "title=has 'quote' and \$(rm -rf /) inside" >/dev/null
out=$(./t.tbl g 4)
check "quote round-trip" "'\\''" "$out"
check "dollar preserved inside single quotes" '$(rm -rf /)' "$out"
out=$(./t.tbl q title~quote)
check "q finds quoted value" "id=4" "$out"

# --- g ---
out=$(./t.tbl g 2)
check "g by id" "title=second" "$out"
out=$(./t.tbl g 99 2>&1); rc=$?
check_rc "g never-existed rc" 1 $rc
check "g never-existed teaches" "never existed" "$out"

# --- s: update, CAS ---
out=$(./t.tbl s 1 status=done by=agent-b)
check "s echo" "status=done" "$out"
check "s sets updated_at" "updated_at=" "$out"
check "s sets updated_by" "updated_by=agent-b" "$out"
out=$(./t.tbl s 1 status=open if status=missingval 2>&1); rc=$?
check_rc "CAS fail rc" 4 $rc
check "CAS teaches stale view" "stale" "$out"
out=$(./t.tbl s 1 prio=9 if status=done)
check "CAS success applies" "prio=9" "$out"
out=$(./t.tbl s 1 2>&1); rc=$?
check_rc "s without assignments" 2 $rc

# --- d: delete, ids never reused ---
out=$(./t.tbl d 2)
check "d echoes record" "title=second" "$out"
out=$(./t.tbl g 2 2>&1); rc=$?
check_rc "deleted g rc" 1 $rc
check "deleted teaches no-reuse" "never reused" "$out"
out=$(./t.tbl a title=fifth status=open)
check "ids monotonic after delete" "id=5" "$out"

# --- i ---
out=$(./t.tbl i)
check "i counts" "4 records" "$out"
check "i last id" "last id=5" "$out"
check "i fields histogram" "title(4)" "$out"

# --- next: atomic take (fresh queue so state is known) ---
./seed init queue.tbl >/dev/null
./queue.tbl a job=one status=open >/dev/null
./queue.tbl a job=two status=open >/dev/null
out=$(./queue.tbl next status=open set status=claimed claimed_by=w1 by=w1)
check "next takes lowest match" "id=1" "$out"
check "next applies set" "status=claimed" "$out"
out=$(./queue.tbl next status=open set status=claimed claimed_by=w2)
check "next second take differs" "id=2" "$out"
out=$(./queue.tbl next status=open set status=claimed 2>&1); rc=$?
check_rc "next empty queue rc" 1 $rc
check "next empty queue message" "queue empty" "$out"
out=$(./queue.tbl next status=claimed 2>&1); rc=$?
check_rc "next without set" 2 $rc

# --- cursor / diff ---
sleep 1 # step past the second of the writes above: diff counts same-second updates
cur=$(./t.tbl cursor | head -1 | sed 's/# cursor=//')
out=$(./t.tbl diff "$cur")
check "diff no changes" "no changes since cursor" "$out"
sleep 1
./t.tbl a title=after status=open >/dev/null
./t.tbl s 3 prio=7 >/dev/null
./t.tbl d 4 >/dev/null
out=$(./t.tbl diff "$cur")
check "diff new record listed" "title=after" "$out"
check "diff summary" "1 new, 1 modified, 1 deleted" "$out"
out=$(./t.tbl diff nonsense 2>&1); rc=$?
check_rc "diff bad cursor rc" 2 $rc

# --- no-ts table ---
./seed init n.tbl no-ts >/dev/null
out=$(./n.tbl a x=1 by=me)
case "$out" in *created_at*|*created_by*) fail "no-ts writes no timestamps" ;; *) ok ;; esac
out=$(./n.tbl a created_at=x 2>&1); rc=$?
check_rc "no-ts still reserves ts keys" 2 $rc
out=$(./n.tbl i)
check "i shows no-ts" "no-ts" "$out"

# --- corruption detection ---
cp t.tbl c.tbl
sz=$(stat -c%s c.tbl)
printf 'X' | dd of=c.tbl bs=1 seek=$((sz - 2)) conv=notrunc 2>/dev/null
out=$(./c.tbl q 2>&1); rc=$?
check_rc "corrupt table rc" 5 $rc
check "corrupt table teaches" "crc mismatch" "$out"

# --- concurrent writers under lock (10 parallel appends, no losses) ---
./seed init p.tbl >/dev/null
for i in $(seq 1 10); do ./p.tbl a n=$i >/dev/null & done
wait
out=$(./p.tbl q count)
check "10 parallel appends all land" "# 10 records" "$out"
out=$(./p.tbl i)
check "parallel last id" "last id=10" "$out"

echo
echo "== $PASS passed, $FAIL failed =="
[ "$FAIL" = 0 ]
