# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
"""Normalize localized MSVC /showIncludes without installing language packs.

Ninja expects UTF-8 paths; CMake's locale detection on the baseline Windows host
misdecoded the Chinese prefix and silently recorded zero header dependencies.
This build-only adapter does not modify compiler arguments or source files.
"""
import ctypes
import os
import subprocess
import sys


def normalize(line, code_page="cp936"):
    try:
        text = line.decode("utf-8")
    except UnicodeDecodeError:
        text = line.decode(code_page, errors="replace")
    for prefix in ("注意: 包含文件:", "Note: including file:"):
        if text.startswith(prefix):
            return ("Note: including file: " + text[len(prefix):].lstrip()).encode("utf-8")
    return text.encode("utf-8")


def main():
    if len(sys.argv) < 2:
        raise SystemExit("MSVC compiler command required")
    code_page = f"cp{ctypes.windll.kernel32.GetACP()}" if os.name == "nt" else "utf-8"
    with subprocess.Popen(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
        for line in process.stdout:
            sys.stdout.buffer.write(normalize(line, code_page))
            sys.stdout.buffer.flush()
        return process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
