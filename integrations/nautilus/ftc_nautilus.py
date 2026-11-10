# Nautilus extension: ftc in the right-click menu of local files and folders.
#
#   "Send with ftc…"            on selected files and folders -> ftc-send-dialog
#   "Get with ftc…"             on the background of a folder  -> ftc-get-dialog
#   "Get with ftc into folder…" on a single selected folder    -> ftc-get-dialog
#
# It only starts the helpers with the paths; the dialogs, login, sending and
# downloading are all there.
#
# Needs nautilus-python (Debian/Ubuntu: python3-nautilus). Goes in
#   ~/.local/share/nautilus-python/extensions/      (one user)
#   /usr/share/nautilus-python/extensions/          (everyone; the package)
# then `nautilus -q` to reload. FTC_SEND_DIALOG / FTC_GET_DIALOG pick other
# helpers.

import os
import shutil
import subprocess

from gi.repository import GObject, Nautilus


def _helper(name):
    env = name.upper().replace("-", "_")  # ftc-send-dialog -> FTC_SEND_DIALOG
    return os.environ.get(env) or shutil.which(name)


def _local_path(f):
    return f.get_location().get_path() if f.get_uri_scheme() == "file" else None


def _start(helper, *args):
    # Own session: the dialog outlives a Nautilus restart.
    subprocess.Popen([helper, *args], start_new_session=True)


class FtcMenu(GObject.GObject, Nautilus.MenuProvider):
    # Nautilus 43+ (API 4.0) passes (files); older ones (window, files).
    def get_file_items(self, *args):
        files = args[-1]
        paths = [_local_path(f) for f in files]
        # All or nothing: a selection with remote files is not offered.
        if not paths or None in paths:
            return []
        items = []

        send = _helper("ftc-send-dialog")
        if send is not None:
            item = Nautilus.MenuItem(
                name="FtcMenu::send",
                label="Send with ftc…",
                tip="Send the selected files and folders with ftc",
            )
            item.connect("activate", lambda _i: _start(send, *paths))
            items.append(item)

        get = _helper("ftc-get-dialog")
        if get is not None and len(files) == 1 and files[0].is_directory():
            item = Nautilus.MenuItem(
                name="FtcMenu::get_into",
                label="Get with ftc into folder…",
                tip="Download received ftc transactions into this folder",
            )
            item.connect("activate", lambda _i: _start(get, paths[0]))
            items.append(item)
        return items

    # Nautilus 43+ passes (folder); older ones (window, folder).
    def get_background_items(self, *args):
        path = _local_path(args[-1])
        get = _helper("ftc-get-dialog")
        if path is None or get is None:
            return []
        item = Nautilus.MenuItem(
            name="FtcMenu::get",
            label="Get with ftc…",
            tip="Download received ftc transactions into this folder",
        )
        item.connect("activate", lambda _i: _start(get, path))
        return [item]
