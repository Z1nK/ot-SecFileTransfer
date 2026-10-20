# Helpers for the scripts in this folder: local confide-server instances
# with TLS, users and peers, driven with ftc. Source it, then call
# site_init once per site.
#
#   BIN     folder with confide-server, confide-passwd and ftc (required)
#   WORK    scratch folder, one sub-folder per site (required)

: "${BIN:?BIN must be the bin folder of a build}"
: "${WORK:?WORK must be set}"

declare -A SITE_PORT SITE_PID

log() { printf '[%s] %s\n' "$(date +%T)" "$*" >&2; }
die() { log "FAIL: $*"; exit 1; }

# A TCP port on 127.0.0.1 that nothing listens on right now.
free_port() {
  local port
  for _ in $(seq 100); do
    # Below the Linux ephemeral range (32768+), so no outgoing connection
    # holds it, and below the Windows dynamic range (49152+), where WSL2 may
    # remap a bind to another port.
    port=$((20000 + RANDOM % 12000))
    if ! (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
      echo "$port"
      return
    fi
  done
  die "no free port found"
}

# Self-signed certificate for localhost / 127.0.0.1, shared by all sites and
# trusted by ftc and by the peers (tls.ca).
make_cert() {
  openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
    -keyout "$WORK/key.pem" -out "$WORK/cert.pem" 2>/dev/null \
    || die "openssl could not make a test certificate"
  chmod 600 "$WORK/key.pem"
}

# site_init NAME USER... : folder, port and users file (password = pw-USER).
site_init() {
  local name=$1
  shift
  mkdir -p "$WORK/$name"
  SITE_PORT[$name]=$(free_port)
  : >"$WORK/$name/users"
  for user in "$@"; do
    printf 'pw-%s\n' "$user" | "$BIN/confide-passwd" --user "$user" >>"$WORK/$name/users"
  done
  chmod 600 "$WORK/$name/users"
}

# site_config NAME [PEER...] : writes server.toml. Every pair of sites shares
# two tokens, one per direction (fixed per pair, so both configs agree; at
# least 32 characters, as the config requires).
site_config() {
  local name=$1
  shift
  cat >"$WORK/$name/server.toml" <<EOF
instance_name = "$name"

[storage]
root = "data"

[rest]
bind = "127.0.0.1"
port = ${SITE_PORT[$name]}

[tls]
enabled = true
cert = "$WORK/cert.pem"
key = "$WORK/key.pem"
ca = "$WORK/cert.pem"

[auth]
users_file = "users"

[log]
level = "debug"
dir = "logs"

[retention]
default_days = 1
max_days = 7
EOF
  for peer in "$@"; do
    cat >>"$WORK/$name/server.toml" <<EOF

[[peer]]
name = "$peer"
host = "localhost"
port = ${SITE_PORT[$peer]}
token = "test-token-$name-to-$peer-0123456789abcdef"
accept_token = "test-token-$peer-to-$name-0123456789abcdef"
EOF
  done
}

# Starts the server and waits until it accepts connections.
site_start() {
  local name=$1
  "$BIN/confide-server" "$WORK/$name/server.toml" >>"$WORK/$name/stdout.log" 2>&1 &
  SITE_PID[$name]=$!
  for _ in $(seq 100); do
    if (exec 3<>"/dev/tcp/127.0.0.1/${SITE_PORT[$name]}") 2>/dev/null; then
      return
    fi
    kill -0 "${SITE_PID[$name]}" 2>/dev/null || break
    sleep 0.1
  done
  cat "$WORK/$name/stdout.log" >&2
  die "$name did not start"
}

# Stops the server with SIGTERM, as systemd would.
site_stop() {
  local name=$1 pid=${SITE_PID[$1]:-}
  [[ -n $pid ]] || return 0
  kill -TERM "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  SITE_PID[$name]=
}

stop_all() {
  for name in "${!SITE_PID[@]}"; do
    site_stop "$name"
  done
}

# Peak resident memory of a running site, in KiB (VmHWM).
site_peak_rss_kib() {
  awk '/^VmHWM:/ { print $2 }' "/proc/${SITE_PID[$1]}/status"
}

# ftc_as USER SITE ARGS... : ftc against SITE, logged in as USER.
ftc_as() {
  local user=$1 site=$2
  shift 2
  FTC_PASSWORD="pw-$user" XDG_STATE_HOME="$WORK/ftc-state" \
    "$BIN/ftc" -s "https://localhost:${SITE_PORT[$site]}" -u "$user" \
    --ca-file "$WORK/cert.pem" "$@"
}

# The transaction id from `ftc send` output ("created <id> for ...").
sent_id() { awk '/^created / { print $2; exit }'; }

# One field of `ftc show`.
show_field() {
  local user=$1 site=$2 id=$3 field=$4
  ftc_as "$user" "$site" show "$id" | awk -v f="$field" '$1 == f { print $2 }'
}

# wait_for SECONDS COMMAND... : retries COMMAND every 0.2 s.
wait_for() {
  local deadline=$((SECONDS + $1))
  shift
  until "$@"; do
    ((SECONDS < deadline)) || return 1
    sleep 0.2
  done
}

# Dumps the server logs, for a failed run.
dump_logs() {
  for name in "${!SITE_PORT[@]}"; do
    for f in "$WORK/$name/logs/technical.log" "$WORK/$name/stdout.log"; do
      [[ -s $f ]] || continue
      echo "----- $f (last 40 lines)" >&2
      tail -n 40 "$f" >&2
    done
  done
}
