#!/usr/bin/env python3
"""Install the built app for the current user, preserving identity and settings."""
import os
from pathlib import Path
import plistlib
import signal
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[1]
archive = root / 'outputs/Window Sweaters.zip'
apps = Path.home() / 'Applications'
destination = apps / 'Window Sweaters.app'
legacy = apps / 'Knit Borders.app'
identifier = 'local.knitborders.app'

def info(app):
    return plistlib.loads((app / 'Contents/Info.plist').read_bytes())

if not archive.exists():
    raise SystemExit('Build first: ./scripts/build-app.sh')
stage = Path(tempfile.mkdtemp(prefix='window-sweaters-extract-'))
subprocess.run(['ditto', '-x', '-k', str(archive), str(stage)], check=True)
source = stage / destination.name
subprocess.run(['xattr', '-cr', str(source)], check=True)
assert info(source)['CFBundleIdentifier'] == identifier
subprocess.run(['codesign', '--verify', '--deep', '--strict', str(source)], check=True)
existing = [p for p in (destination, legacy) if p.exists()]
for app in existing:
    if info(app).get('CFBundleIdentifier') != identifier:
        raise SystemExit(f'Refusing to replace an unrelated app: {app}')

# Archive before replacing; never include user preferences in an installation.
backup = Path(tempfile.mkdtemp(prefix='window-sweaters-install-'))
for app in existing:
    subprocess.run(['ditto', '-c', '-k', '--keepParent', str(app), str(backup / (app.name + '.zip'))], check=True)
executables = {str(p / 'Contents/MacOS' / info(p)['CFBundleExecutable']) for p in existing + [source]}
# A previously installed copy may run from this repository's build folder.
executables.add(str(root / 'outputs/Knit Borders.app/Contents/MacOS/KnitBorders'))
executables.add(str(root / 'outputs/Window Sweaters.app/Contents/MacOS/WindowSweaters'))

def running():
    lines = subprocess.check_output(['ps', '-axo', 'pid=,comm='], text=True).splitlines()
    return [int(parts[0]) for line in lines
            if len(parts := line.strip().split(None, 1)) == 2 and parts[1] in executables]

for pid in running():
    os.kill(pid, signal.SIGTERM)
deadline = time.monotonic() + 3
while running() and time.monotonic() < deadline:
    time.sleep(.1)
if running():
    raise SystemExit('The app did not stop; installation left untouched.')

apps.mkdir(exist_ok=True)
lsregister = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
for app in existing:
    subprocess.run([lsregister, '-u', str(app)], check=True)
    # Move rather than delete the previous bundle, retaining a recoverable copy.
    app.rename(backup / app.name)
subprocess.run(['ditto', str(source), str(destination)], check=True)
subprocess.run(['xattr', '-cr', str(destination)], check=True)
subprocess.run(['codesign', '--verify', '--deep', '--strict', str(destination)], check=True)
subprocess.run([lsregister, '-f', str(destination)], check=True)
subprocess.run(['open', str(destination)], check=True)
print(f'Installed: {destination}\nPrevious app backup: {backup}')
subprocess.run(['trash', str(stage)], check=True)
