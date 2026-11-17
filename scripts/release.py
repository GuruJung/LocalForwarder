#!/usr/bin/env python3
"""Publish only the three verified packages of an existing matching version tag."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess
import zipfile
from version import check_tag, read_version

TARGETS = (('macos', 'arm64'), ('windows', 'x64'), ('windows', 'arm64'))


def validate_packages(directory, version, revision):
    packages = []
    for platform, arch in TARGETS:
        path = directory / f'LocalForwarder-{version}-{platform}-{arch}.zip'
        with zipfile.ZipFile(path) as archive:
            if archive.testzip() is not None:
                raise ValueError(f'Damaged package: {path.name}')
            prefix = 'LocalForwarder.app/Contents/' if platform == 'macos' else ''
            resource = prefix + 'Resources/' if platform == 'macos' else ''
            info = json.loads(archive.read(resource + 'build-info.json'))
            for key, expected in (('version', version), ('platform', platform),
                                  ('architecture', arch), ('revision', revision)):
                if info.get(key) != expected:
                    raise ValueError(f'{path.name}: {key} mismatch')
            executable = archive.read(prefix + 'MacOS/LocalForwarder' if platform == 'macos' else 'LocalForwarder.exe')
            if not executable:
                raise ValueError(f'{path.name}: empty executable')
            for document in ('LICENSE', 'README.md', 'README_ko.md'):
                if not archive.read(resource + document):
                    raise ValueError(f'{path.name}: missing {document}')
            if platform == 'macos':
                plist = plistlib.loads(archive.read(prefix + 'Info.plist'))
                if any(plist.get(key) != version for key in ('CFBundleVersion', 'CFBundleShortVersionString')):
                    raise ValueError(f'{path.name}: bundle version mismatch')
            elif info.get('executable_sha256') != hashlib.sha256(executable).hexdigest():
                raise ValueError(f'{path.name}: executable hash mismatch')
        packages.append(path)
    return packages


def run(*args):
    return subprocess.check_output(args, text=True)


def find_release(repo, tag, runner):
    pages = json.loads(runner('gh', 'api', '--paginate', '--slurp', f'repos/{repo}/releases'))
    matches = [release for page in pages for release in page if release['tag_name'] == tag]
    if len(matches) > 1:
        raise ValueError('Multiple releases have the same tag')
    return matches[0] if matches else None


def require_draft(release):
    if release is None or not release.get('draft'):
        raise ValueError('Refusing to modify an already published or missing release')


def publish(directory, version, tag, revision, repo, runner=run):
    check_tag(tag, version)
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo):
        raise ValueError('Invalid repository name')
    packages = validate_packages(directory, version, revision)
    # Verify remote tag presence even when recovering an existing draft.
    remote = json.loads(runner('gh', 'api', f'repos/{repo}/git/ref/tags/{tag}'))['object']
    # Annotated tags point to tag objects; lightweight tags point directly to commits.
    for _ in range(10):
        if remote['type'] != 'tag':
            break
        remote = json.loads(runner('gh', 'api', f"repos/{repo}/git/tags/{remote['sha']}"))['object']
    if remote['type'] != 'commit' or remote['sha'] != revision:
        raise ValueError('Remote tag no longer identifies the verified build revision')
    release = find_release(repo, tag, runner)
    if release is not None:
        require_draft(release)
    else:
        runner('gh', 'release', 'create', tag, '--repo', repo, '--draft', '--verify-tag',
               '--generate-notes', '--title', f'LocalForwarder {version}')
    require_draft(find_release(repo, tag, runner))
    runner('gh', 'release', 'upload', tag, *map(str, packages), '--repo', repo, '--clobber')
    release = find_release(repo, tag, runner)
    require_draft(release)
    actual = {asset['name']: asset['size'] for asset in release.get('assets', [])}
    expected = {path.name: path.stat().st_size for path in packages}
    if actual != expected:
        raise ValueError('Draft assets do not exactly match all three verified packages')
    runner('gh', 'release', 'edit', tag, '--repo', repo, '--draft=false')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--repo', required=True)
    args = parser.parse_args()
    try:
        publish(args.artifacts, read_version(), args.tag, args.revision, args.repo)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Release was not published: {error}\n')


if __name__ == '__main__':
    main()
