# The confide user and /var/lib/confide (transferred files) are kept.
if [ -d /run/systemd/system ]; then
  systemctl daemon-reload || :
  if [ "$1" -ge 1 ]; then
    systemctl try-restart confide-server.service || :
  fi
fi
exit 0
