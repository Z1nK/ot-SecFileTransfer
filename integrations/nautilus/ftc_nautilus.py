# Nautilus extension: "Send with ftc…" in the right-click menu of local files
# and folders. It only starts ftc-send-dialog with the selected paths; the
# dialog, login and sending are all there.
#
# Needs nautilus-python (Debian/Ubuntu: python3-nautilus). Goes in
#   ~/.local/share/nautilus-python/extensions/      (one user)
#   /usr/share/nautilus-python/extensions/          (everyone; the package)
# then `nautilus -q` to reload. FTC_SEND_DIALOG picks another helper.

import os
import shutil
import subprocess

from gi.repository import GObject, Nautilus


def _helper():
    return os.environ.get("FTC_SEND_DIALOG") or shutil.which("ftc-send-dialog")


class FtcSendMenu(GObject.GObject, Nautilus.MenuProvider):
    # Nautilus 43+ (API 4.0) passes (files); older ones (window, files).
    def get_file_items(self, *args):
        files = args[-1]
        paths = [f.get_location().get_path() for f in files if f.get_uri_scheme() == "file"]
        # All or nothing: a selection with remote files is not offered.
        if not paths or len(paths) != len(files) or _helper() is None:
            return []
        item = Nautilus.MenuItem(
            name="FtcSendMenu::send",
            label="Send with ftc…",
            tip="Send the selected files and folders with ftc",
        )
        item.connect("activate", self._send, paths)
        return [item]

    def _send(self, _item, paths):
        # Own session: the dialog outlives a Nautilus restart.
        subprocess.Popen([_helper(), *paths], start_new_session=True)
