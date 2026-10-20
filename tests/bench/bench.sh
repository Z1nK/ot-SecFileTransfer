#!/usr/bin/env bash
# Benchmark and memory check (NFR-4, NFR-5, Definition of Done 4), with TLS,
# on two local servers:
#
#   small   COUNT files of 4 KiB: ftc send to a local user, then ftc get
#   large   one file of SIZE MiB: ftc send, forward siteA -> siteB, ftc get
#
# Reports times and throughput, and the peak memory (VmHWM / max RSS) of
# both servers and of ftc. Fails if a download differs from the source, or
# if a process peaks above MAX_RSS_MIB (default 100, the NFR-4 target).
# LOG_LEVEL sets the servers' log level (default info).
#
#   bench.sh <bin folder> [SIZE MiB, default 2048] [COUNT, default 1000]
#
# Needs about 4 x SIZE of free disk in TMPDIR (source, two server copies,
# download). Use a Release build: Debug is several times slower.

set -euo pipefail

BIN=$(cd "${1:?usage: $0 <bin folder> [size MiB] [count]}" && pwd)
SIZE_MIB=${2:-2048}
COUNT=${3:-1000}
MAX_RSS_MIB=${MAX_RSS_MIB:-100}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/confide-bench.XXXXXX")
source "$(dirname "$0")/../integration/lib.sh"

ok=false
finish() {
  stop_all
  $ok || dump_logs
  rm -rf "$WORK"
}
trap finish EXIT

command -v /usr/bin/time >/dev/null || die "needs /usr/bin/time (package 'time')"

need_kib=$((SIZE_MIB * 1024 * 4 + 200 * 1024))
free_kib=$(df -Pk "$WORK" | awk 'NR == 2 { print $4 }')
((free_kib > need_kib)) || die "needs $((need_kib / 1024)) MiB free in $WORK, has $((free_kib / 1024))"

make_cert
site_init siteA alice carol
site_init siteB bob
site_config siteA siteB
site_config siteB siteA
sed -i "s/^level = \"debug\"/level = \"${LOG_LEVEL:-info}\"/" "$WORK"/site{A,B}/server.toml
site_start siteA
site_start siteB
ftc_as alice siteA login >/dev/null
ftc_as carol siteA login >/dev/null
ftc_as bob siteB login >/dev/null

now() { date +%s.%N; }
elapsed() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", b - a }'; }
rate() { awk -v mib="$1" -v s="$2" 'BEGIN { printf "%.0f", (s > 0 ? mib / s : 0) }'; }
mib() { awk -v k="$1" 'BEGIN { printf "%.1f", k / 1024 }'; }

# timed_ftc OUT_VAR USER SITE ARGS... : runs ftc, sets OUT_VAR to "seconds max_rss_kib".
timed_ftc() {
  local -n out=$1
  shift
  local stats=$WORK/time.txt
  ftc_time() { /usr/bin/time -f "%e %M" -o "$stats" "$@"; }
  local user=$1 site=$2
  shift 2
  FTC_PASSWORD="pw-$user" XDG_STATE_HOME="$WORK/ftc-state" ftc_time \
    "$BIN/ftc" -s "https://localhost:${SITE_PORT[$site]}" -u "$user" \
    --ca-file "$WORK/cert.pem" "$@" >"$WORK/ftc.out"
  out=$(cat "$stats")
}

results=()
report() { results+=("$(printf '%-34s %10s' "$1" "$2")"); }
peak_ftc=0
note_ftc() { ((${1#* } > peak_ftc)) && peak_ftc=${1#* }; true; }

# --- Small files -------------------------------------------------------------
log "small: $COUNT files of 4 KiB"
small=$WORK/src/small
mkdir -p "$small"
for i in $(seq "$COUNT"); do
  head -c 4096 /dev/urandom >"$small/f$i.bin"
done

timed_ftc t alice siteA send --to carol "$small"
id=$(sent_id <"$WORK/ftc.out")
note_ftc "$t"
report "small send ($COUNT files)" "${t% *} s"
report "small send rate" "$(awk -v n="$COUNT" -v s="${t% *}" 'BEGIN { printf "%.0f", n / s }') files/s"

timed_ftc t carol siteA get "$id" "$WORK/small-out"
note_ftc "$t"
report "small get ($COUNT files)" "${t% *} s"
diff -r "$small" "$WORK/small-out/small" >/dev/null || die "small files differ after download"

# --- Large file --------------------------------------------------------------
log "large: one file of $SIZE_MIB MiB"
large=$WORK/src/large.bin
openssl rand -out "$large" $((SIZE_MIB * 1024 * 1024))
want=$(sha256sum "$large" | cut -d' ' -f1)

timed_ftc t alice siteA send --to bob@siteB "$large"
id=$(sent_id <"$WORK/ftc.out")
note_ftc "$t"
report "large send (incl. commit)" "${t% *} s"
report "large send rate" "$(rate "$SIZE_MIB" "${t% *}") MiB/s"

start=$(now)
is_delivered() { [[ $(show_field alice siteA "$1" state) == delivered ]]; }
wait_for $((60 + SIZE_MIB / 10)) is_delivered "$id" || die "$id not delivered to siteB"
took=$(elapsed "$start" "$(now)")
report "large forward siteA -> siteB" "$took s"
report "large forward rate" "$(rate "$SIZE_MIB" "$took") MiB/s"
rm -f "$large"  # room for the download

timed_ftc t bob siteB get "$id" "$WORK/large-out"
note_ftc "$t"
report "large get" "${t% *} s"
report "large get rate" "$(rate "$SIZE_MIB" "${t% *}") MiB/s"
got=$(sha256sum "$WORK/large-out/large.bin" | cut -d' ' -f1)
[[ $got == "$want" ]] || die "large file differs after download"

# --- Memory ------------------------------------------------------------------
peak_a=$(site_peak_rss_kib siteA)
peak_b=$(site_peak_rss_kib siteB)
report "peak RSS confide-server siteA" "$(mib "$peak_a") MiB"
report "peak RSS confide-server siteB" "$(mib "$peak_b") MiB"
report "peak RSS ftc" "$(mib "$peak_ftc") MiB"

printf '\nconfide benchmark: %s MiB file, %s small files, TLS on\n' "$SIZE_MIB" "$COUNT"
printf '%s\n' "${results[@]}"
printf '\nchecksums match: yes\n'

limit=$((MAX_RSS_MIB * 1024))
for kib in "$peak_a" "$peak_b" "$peak_ftc"; do
  ((kib <= limit)) || die "peak RSS $(mib "$kib") MiB is over the $MAX_RSS_MIB MiB limit"
done
ok=true
log "PASS"
