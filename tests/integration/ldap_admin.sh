#!/usr/bin/env bash
# End-to-end test of the admin interface with LDAP users and groups: one
# confide-server with provider = "ldap" against the test directory of
# docker/docker-compose.yml, driven by ftc.
#
#   1. alice (LDAP group "admins") logs in as admin: status, list,
#      retention, sessions, kick, business log.
#   2. bob and carol (not in "admins") are refused; "admins" is no recipient.
#   3. Group changes in LDAP apply to running sessions: without alice her
#      admin token is refused, carol added gets in. Needs `docker exec` into
#      the container (LDAP_CONTAINER, default ldap-test); skipped otherwise.
#   4. alice back in the group deletes the transaction; the business log
#      holds every admin action.
#
#   docker compose -f docker/docker-compose.yml up -d
#   CONFIDE_LDAP_TEST_URI=ldap://localhost:1389 ldap_admin.sh <bin folder> [work folder]
#
# Exits 77 (skipped) when CONFIDE_LDAP_TEST_URI is not set. The work folder
# is deleted on success and kept (with logs) on failure.

set -euo pipefail

if [[ -z ${CONFIDE_LDAP_TEST_URI:-} ]]; then
  echo "CONFIDE_LDAP_TEST_URI not set: skipped" >&2
  exit 77
fi
LDAP_CONTAINER=${LDAP_CONTAINER:-ldap-test}
LDAP_ADMIN_DN="cn=admin,dc=example,dc=org"
LDAP_ADMIN_PW="adminpassword"
ADMINS_DN="cn=admins,ou=groups,dc=example,dc=org"

BIN=$(cd "${1:?usage: $0 <bin folder> [work folder]}" && pwd)
WORK=${2:-$(mktemp -d "${TMPDIR:-/tmp}/confide-ldap-it.XXXXXX")}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
source "$(dirname "$0")/lib.sh"

# ldap_group_swap ADD REMOVE : in "admins", adds uid=ADD and removes uid=REMOVE.
ldap_group_swap() {
  docker exec -i "$LDAP_CONTAINER" ldapmodify -x -H ldap://localhost:389 \
    -D "$LDAP_ADMIN_DN" -w "$LDAP_ADMIN_PW" >/dev/null <<EOF
dn: $ADMINS_DN
changetype: modify
add: member
member: uid=$1,ou=people,dc=example,dc=org
-
delete: member
member: uid=$2,ou=people,dc=example,dc=org
EOF
}

group_changed=false
ok=false
finish() {
  stop_all
  # Put the directory back as the seed has it, also after a failure.
  if $group_changed; then
    ldap_group_swap alice carol || log "could not restore $ADMINS_DN: fix it by hand"
  fi
  if $ok; then
    rm -rf "$WORK"
  else
    dump_logs
    log "work folder kept: $WORK"
  fi
}
trap finish EXIT

# Short cache, so the group changes in step 3 show up within seconds.
CACHE_S=2
make_cert
mkdir -p "$WORK/siteL"
SITE_PORT[siteL]=$(free_port)
cat >"$WORK/siteL/server.toml" <<EOF
instance_name = "siteL"

[storage]
root = "data"

[rest]
bind = "127.0.0.1"
port = ${SITE_PORT[siteL]}

[tls]
enabled = true
cert = "$WORK/cert.pem"
key = "$WORK/key.pem"

[auth]
provider = "ldap"
admin_group = "admins"
admin_token_ttl_s = 600

[auth.ldap]
preset = "openldap"
uri = "$CONFIDE_LDAP_TEST_URI"
bind_dn = "$LDAP_ADMIN_DN"
bind_password = "$LDAP_ADMIN_PW"
user_base_dn = "ou=people,dc=example,dc=org"
group_base_dn = "ou=groups,dc=example,dc=org"
cache_ttl_s = $CACHE_S

[log]
level = "debug"
dir = "logs"
EOF
site_start siteL
log "siteL on ${SITE_PORT[siteL]}, LDAP at $CONFIDE_LDAP_TEST_URI"

# ftc_ldap USER ARGS... : ftc as an LDAP user of the seed (password USER123).
ftc_ldap() {
  local user=$1
  shift
  FTC_PASSWORD="${user}123" XDG_STATE_HOME="$WORK/ftc-state" \
    "$BIN/ftc" -s "https://localhost:${SITE_PORT[siteL]}" -u "$user" \
    --ca-file "$WORK/cert.pem" "$@"
}

# expect_refused CODE USER ARGS... : the ftc call fails with error CODE.
expect_refused() {
  local code=$1
  shift
  if ftc_ldap "$@" 2>"$WORK/err.txt"; then
    die "'ftc $*' succeeded, want $code"
  fi
  grep -q "$code" "$WORK/err.txt" || die "'ftc $*': $(cat "$WORK/err.txt"), want $code"
}

src=$WORK/a.txt
echo "hello" >"$src"
id=$(ftc_ldap bob send --to carol "$src" | sent_id)
[[ -n $id ]] || die "no transaction id from ftc send"

# --- 1. Admin in the LDAP group --------------------------------------------
log "1. alice (in admins) uses the admin API"
ftc_ldap alice admin login | grep -q "admin session for alice" || die "admin login failed"
ftc_ldap alice admin status >"$WORK/out.txt"
grep -q "auth *ldap" "$WORK/out.txt" || die "status does not say ldap: $(cat "$WORK/out.txt")"
ftc_ldap alice admin list --who bob | grep -q "$id" || die "admin list misses $id"
ftc_ldap alice admin retention "$id" 3 | grep -q "now expires at" || die "retention change failed"
ftc_ldap alice admin sessions | grep -q "^bob .* user " || die "bob's session not listed"
ftc_ldap alice admin kick bob | grep -q "ended 1 session" || die "kick bob failed"
ftc_ldap alice admin sessions | grep -q "^bob " && die "bob still has a session"
# Right after the events: the default end of the range includes this second.
ftc_ldap alice admin log --from 1h >"$WORK/admin-log.txt"
grep -q '"event":"tx_created"' "$WORK/admin-log.txt" || die "admin log has no tx_created"

# --- 2. Others ---------------------------------------------------------------
log "2. users outside admins are refused"
expect_refused forbidden bob admin list
expect_refused forbidden carol admin login
expect_refused unknown_destination alice send --to admins "$src"

# --- 3. Group changes in LDAP ---------------------------------------------------
if docker exec "$LDAP_CONTAINER" true 2>/dev/null; then
  log "3. LDAP group changes apply to running sessions"
  group_changed=true
  ldap_group_swap carol alice
  sleep $((CACHE_S + 1))
  expect_refused forbidden alice admin status  # admin token still valid, rights gone
  ftc_ldap carol admin status | grep -q "instance *siteL" || die "carol (now in admins) refused"
  ldap_group_swap alice carol
  group_changed=false
  sleep $((CACHE_S + 1))
else
  log "3. skipped: no docker access to container $LDAP_CONTAINER"
fi

# --- 4. Delete and audit -------------------------------------------------------
log "4. alice deletes; the business log has every admin action"
ftc_ldap alice admin delete "$id" --yes | grep -q "deleted $id" || die "admin delete failed"
ftc_ldap alice admin list | grep -q "$id" && die "$id still listed after delete"
for ev in retention_changed sessions_revoked admin_deleted; do
  grep -q "\"event\":\"$ev\".*\"actor\":\"alice\"" "$WORK/siteL/logs/business.log" \
    || die "no $ev by alice in the business log"
done
ftc_ldap alice admin logout | grep -q "admin session ended" || die "admin logout failed"

ok=true
log "PASS"
