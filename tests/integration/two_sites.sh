#!/usr/bin/env bash
# End-to-end test (NFR-9, Definition of Done 2-3): two confide-server
# instances on this machine with TLS, driven by ftc.
#
#   1. alice@siteA sends a folder to carol on the same server; carol gets it.
#   2. alice@siteA sends a folder to bob@siteB; the forwarder pushes it,
#      it turns `delivered` on siteA and bob gets the same bytes on siteB.
#   3. A committed transaction rejects new files.
#   4. Unknown destinations are refused up front.
#   5. With siteB down, a transaction waits `committed` and is delivered
#      once siteB is back.
#
#   two_sites.sh <bin folder> [work folder]
#
# The work folder is deleted on success and kept (with logs) on failure.

set -euo pipefail

BIN=$(cd "${1:?usage: $0 <bin folder> [work folder]}" && pwd)
WORK=${2:-$(mktemp -d "${TMPDIR:-/tmp}/confide-it.XXXXXX")}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
source "$(dirname "$0")/lib.sh"

ok=false
finish() {
  stop_all
  if $ok; then
    rm -rf "$WORK"
  else
    dump_logs
    log "work folder kept: $WORK"
  fi
}
trap finish EXIT

make_cert
site_init siteA alice carol
site_init siteB bob
site_config siteA siteB
site_config siteB siteA
site_start siteA
site_start siteB
log "siteA on ${SITE_PORT[siteA]}, siteB on ${SITE_PORT[siteB]}"

# A folder with nesting, an empty file, odd names and a few MB of random data.
src=$WORK/src/report
mkdir -p "$src/sub/deeper"
echo "hello" >"$src/a.txt"
: >"$src/empty.bin"
echo "spaces and #" >"$src/sub/my file #1.txt"
head -c $((5 * 1024 * 1024)) /dev/urandom >"$src/sub/deeper/random.bin"

# --- 1. Local transfer -------------------------------------------------------
log "1. alice -> carol@siteA"
id=$(ftc_as alice siteA send --to carol --comment "local" "$src" | sent_id)
[[ -n $id ]] || die "no transaction id from ftc send"
[[ $(show_field alice siteA "$id" state) == committed ]] || die "local transaction not committed"
ftc_as carol siteA inbox | grep -q "$id" || die "carol's inbox misses $id"
ftc_as carol siteA get "$id" "$WORK/carol" >/dev/null
diff -r "$WORK/src/report" "$WORK/carol/report" || die "carol's copy differs"

# --- 2. Forwarded transfer ---------------------------------------------------
log "2. alice -> bob@siteB"
id=$(ftc_as alice siteA send --to bob@siteB --comment "remote" "$src" | sent_id)
[[ -n $id ]] || die "no transaction id from ftc send"
is_delivered() { [[ $(show_field alice siteA "$1" state) == delivered ]]; }
wait_for 30 is_delivered "$id" || die "$id not delivered to siteB"
ftc_as bob siteB inbox | grep -q "$id" || die "bob's inbox misses $id"
[[ $(show_field bob siteB "$id" sender) == alice ]] || die "wrong sender on siteB"
[[ $(show_field bob siteB "$id" comment) == remote ]] || die "comment lost on the way"
ftc_as bob siteB get "$id" "$WORK/bob" >/dev/null
diff -r "$WORK/src/report" "$WORK/bob/report" || die "bob's copy differs"
grep -q '"event":"forwarded"' "$WORK/siteA/logs/business.log" || die "no forwarded event on siteA"
grep -q '"event":"received"' "$WORK/siteB/logs/business.log" || die "no received event on siteB"

# --- 3. Committed is immutable -----------------------------------------------
log "3. commit makes a transaction immutable"
token=$(cat "$WORK"/ftc-state/ftc/tokens/alice@localhost_"${SITE_PORT[siteA]}")
code=$(curl -s -o /dev/null -w '%{http_code}' --cacert "$WORK/cert.pem" \
  -H "Authorization: Bearer $token" -X PUT --data-binary "late" \
  "https://localhost:${SITE_PORT[siteA]}/transactions/$id/files/late.txt")
[[ $code == 409 ]] || die "upload into a committed transaction gave HTTP $code, want 409"

# --- 4. Unknown destinations -------------------------------------------------
log "4. unknown destinations are refused"
if ftc_as alice siteA send --to nobody "$src/a.txt" 2>"$WORK/err.txt"; then
  die "send to an unknown user succeeded"
fi
grep -q unknown_destination "$WORK/err.txt" || die "unexpected error: $(cat "$WORK/err.txt")"
if ftc_as alice siteA send --to bob@siteZ "$src/a.txt" 2>"$WORK/err.txt"; then
  die "send to an unknown server succeeded"
fi
grep -q unknown_destination "$WORK/err.txt" || die "unexpected error: $(cat "$WORK/err.txt")"

# --- 5. Peer down, then back -------------------------------------------------
log "5. siteB down: transaction waits, then is delivered"
site_stop siteB
id=$(ftc_as alice siteA send --to bob@siteB "$src/a.txt" | sent_id)
sleep 1
[[ $(show_field alice siteA "$id" state) == committed ]] || die "$id should wait while siteB is down"
site_start siteB
# First retries come after 5 s and 10 s.
wait_for 40 is_delivered "$id" || die "$id not delivered after siteB came back"
ftc_as bob siteB get "$id" "$WORK/bob2" >/dev/null
diff "$src/a.txt" "$WORK/bob2/a.txt" || die "late copy differs"

# --- 6. Step by step: create, add, rm, commit ------------------------------
log "6. alice -> carol@siteA step by step"
id=$(ftc_as alice siteA create --to carol --comment "steps" | sent_id)
[[ -n $id ]] || die "no transaction id from ftc create"
[[ $(show_field alice siteA "$id" state) == open ]] || die "$id should be open after create"
ftc_as alice siteA outbox | grep -q "$id" || die "alice's outbox misses open $id"
ftc_as carol siteA inbox | grep -q "$id" && die "carol sees $id before commit"
ftc_as alice siteA add "$id" "$src" >/dev/null
ftc_as alice siteA add "$id" "$src/a.txt" >/dev/null  # sent as a.txt, next to report/
ftc_as alice siteA files "$id" | grep -q ' a.txt$' || die "a.txt not added to $id"
ftc_as alice siteA rm "$id" a.txt >/dev/null
ftc_as alice siteA files "$id" | grep -q ' a.txt$' && die "a.txt still in $id after rm"
if ftc_as alice siteA rm "$id" a.txt 2>"$WORK/err.txt"; then
  die "rm of a missing file succeeded"
fi
grep -q notfound "$WORK/err.txt" || die "unexpected error: $(cat "$WORK/err.txt")"
ftc_as alice siteA commit "$id" | grep -q "^committed $id" || die "commit of $id failed"
[[ $(show_field alice siteA "$id" state) == committed ]] || die "$id not committed"
if ftc_as alice siteA add "$id" "$src/a.txt" 2>"$WORK/err.txt"; then
  die "add into a committed transaction succeeded"
fi
grep -q conflict "$WORK/err.txt" || die "unexpected error: $(cat "$WORK/err.txt")"
ftc_as carol siteA get "$id" "$WORK/carol-steps" >/dev/null
diff -r "$WORK/src/report" "$WORK/carol-steps/report" || die "carol's step-by-step copy differs"
[[ ! -e $WORK/carol-steps/a.txt ]] || die "removed a.txt was delivered"

ok=true
log "PASS"
