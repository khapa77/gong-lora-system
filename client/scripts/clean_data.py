# M-19: data/ picks up OS junk (.DS_Store on macOS, Thumbs.db on Windows)
# just from being browsed in a file manager — none of it belongs on the
# device's flash, and 6KB of .DS_Store was actually observed uploaded here.
# Runs as a PlatformIO pre-build script (see platformio.ini extra_scripts).
import glob
import os

Import("env")  # noqa: F821 — injected by PlatformIO

JUNK_PATTERNS = (".DS_Store", "Thumbs.db", "._*")


def clean_data(*_args, **_kwargs):
    data_dir = os.path.join(env.get("PROJECT_DIR", "."), "data")  # noqa: F821
    for pattern in JUNK_PATTERNS:
        for path in glob.glob(os.path.join(data_dir, "**", pattern), recursive=True):
            try:
                os.remove(path)
                print("[clean_data] removed", path)
            except OSError as e:
                print("[clean_data] could not remove", path, e)


clean_data()
