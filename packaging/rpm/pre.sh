getent group confide >/dev/null || groupadd --system confide
getent passwd confide >/dev/null || useradd --system --gid confide --home-dir /var/lib/confide \
  --no-create-home --shell /sbin/nologin --comment "Confide server" confide
exit 0
