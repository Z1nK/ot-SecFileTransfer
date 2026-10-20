# Not started on install: server.toml must be edited first. preset applies the
# distribution policy, which leaves unknown services disabled.
if [ -d /run/systemd/system ]; then
  systemctl daemon-reload || :
  if [ "$1" -eq 1 ]; then
    systemctl preset confide-server.service || :
  fi
fi
exit 0
