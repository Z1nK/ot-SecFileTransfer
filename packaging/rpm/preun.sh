if [ "$1" -eq 0 ] && [ -d /run/systemd/system ]; then
  systemctl --no-reload disable --now confide-server.service || :
fi
exit 0
