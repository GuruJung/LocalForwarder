#!/usr/bin/env python3
"""Record the build identity inside each package before signing/archiving."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from version import read_version

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--platform', choices=('macos', 'windows'), required=True)
parser.add_argument('--arch', choices=('x64', 'arm64'), required=True)
parser.add_argument('--executable', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
info = dict(version=read_version(), platform=args.platform, architecture=args.arch,
    revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip())
# The macOS bundle signature seals its manifest and executable together. Hashing the
# executable in a sealed resource would make the signature self-referential.
if args.platform == 'windows':
    info['executable_sha256'] = hashlib.sha256(args.executable.read_bytes()).hexdigest()
args.output.write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
