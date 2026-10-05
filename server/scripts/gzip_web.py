# The web UI is embedded gzip-compressed (platformio.ini: board_build.embed_files)
# and sent to the browser compressed, as is — the ~64 KB page is ~18 KB on the
# air. (Don't put the HTTP header name in the first two lines: Python reads
# "...coding: x" there as the file's source encoding.) Sending it uncompressed over the AP kept failing half-way (lwIP ran out
# of send buffers, the socket was closed, the browser got a cut-off page or
# nothing). Runs as a PlatformIO pre-build script; output goes to web/gz/
# (git-ignored) and is rewritten only when the source changes.
import gzip
import os

Import("env")  # noqa: F821 — injected by PlatformIO

PAGES = ("index.html", "info.html", "guide.html")


def gzip_web():
    web = os.path.join(env.get("PROJECT_DIR", "."), "web")  # noqa: F821
    out = os.path.join(web, "gz")
    os.makedirs(out, exist_ok=True)
    for name in PAGES:
        src = os.path.join(web, name)
        dst = os.path.join(out, name + ".gz")
        with open(src, "rb") as f:
            data = gzip.compress(f.read(), compresslevel=9, mtime=0)
        old = None
        if os.path.exists(dst):
            with open(dst, "rb") as f:
                old = f.read()
        if data != old:
            with open(dst, "wb") as f:
                f.write(data)
            print("[gzip_web] %s: %d -> %d bytes" % (name, os.path.getsize(src), len(data)))


gzip_web()
