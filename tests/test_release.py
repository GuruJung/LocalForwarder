"""Version and publication contract checks; never call an external service."""
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
import zipfile

SCRIPTS = Path(__file__).resolve().parent.parent / 'scripts'
sys.path.insert(0, str(SCRIPTS))
import version
import release


class VersionTests(unittest.TestCase):
    def test_generated_source_matches_and_tag_is_exact(self):
        value = version.read_version()
        self.assertEqual((version.ROOT / version.SWIFT).read_text(), version.swift_source(value))
        version.check_tag('v' + value, value)
        for tag in (value, 'v' + value + '-rc1', 'v99.0.0'):
            with self.assertRaises(ValueError):
                version.check_tag(tag, value)

    def test_format_and_native_ranges(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for value in ('0.1.0', '9999.99.99', '1.0.0'):
                (root / 'VERSION').write_text(value + '\n')
                self.assertEqual(version.read_version(root), value)
            for value in ('01.2.3', '1.2', '1.2.3-beta', '10000.0.0', '1.100.0', '1.0.100', 'a.b.c', '1.2.3\n4.5.6'):
                (root / 'VERSION').write_text(value)
                with self.assertRaises(ValueError):
                    version.read_version(root)


class FakeGitHub:
    def __init__(self, existing=None, fail_upload=False, omit_asset=False):
        self.existing = existing
        self.calls = []
        self.fail_upload = fail_upload
        self.omit_asset = omit_asset

    def __call__(self, *args):
        self.calls.append(args)
        if args[1:4] == ('api', '--paginate', '--slurp'):
            return json.dumps([[self.existing] if self.existing else []])
        if args[1] == 'api':
            return json.dumps(dict(object=dict(type='commit', sha='a' * 40)))
        action = args[2]
        if action == 'create':
            assert '--draft' in args and '--verify-tag' in args and '--generate-notes' in args
            self.existing = dict(tag_name=args[3], draft=True, assets=[])
        elif action == 'upload':
            assert self.existing['draft']
            if self.fail_upload:
                raise subprocess.CalledProcessError(1, args)
            paths = [Path(arg) for arg in args[4:] if arg.endswith('.zip')]
            self.existing['assets'] = [dict(name=p.name, size=p.stat().st_size) for p in paths]
            if self.omit_asset:
                self.existing['assets'].pop()
        elif action == 'edit':
            assert self.existing['draft']
            self.existing['draft'] = False
        else:
            raise AssertionError(args)
        return ''


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.value = '0.1.0'
        self.revision = 'a' * 40
        self.create_packages()

    def create_packages(self, bad_version=None):
        for platform, arch in release.TARGETS:
            path = self.root / f'LocalForwarder-{self.value}-{platform}-{arch}.zip'
            mac = platform == 'macos'
            prefix = 'LocalForwarder.app/Contents/' if mac else ''
            resource = prefix + 'Resources/' if mac else ''
            binary = b'fixture executable'
            info = dict(version=bad_version or self.value, platform=platform, architecture=arch,
                        revision=self.revision, executable_sha256=hashlib.sha256(binary).hexdigest())
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr(resource + 'build-info.json', json.dumps(info))
                archive.writestr(prefix + 'MacOS/LocalForwarder' if mac else 'LocalForwarder.exe', binary)
                for name in ('LICENSE', 'README.md', 'README_ko.md'):
                    archive.writestr(resource + name, 'fixture')
                if mac:
                    archive.writestr(prefix + 'Info.plist', plistlib.dumps(dict(CFBundleVersion=self.value, CFBundleShortVersionString=self.value)))

    def publish(self, fake, tag='v0.1.0', revision=None):
        release.publish(self.root, self.value, tag, revision or self.revision, 'GuruJung/LocalForwarder', fake)

    def test_create_upload_verify_then_publish(self):
        fake = FakeGitHub()
        self.publish(fake)
        self.assertFalse(fake.existing['draft'])
        self.assertEqual([c[2] for c in fake.calls if c[1] == 'release'], ['create', 'upload', 'edit'])
        self.assertEqual(len(fake.existing['assets']), 3)

    def test_tag_mismatch_missing_package_and_revision_rejected_before_service(self):
        fake = FakeGitHub()
        with self.assertRaises(ValueError):
            self.publish(fake, tag='v0.2.0')
        with self.assertRaises(ValueError):
            self.publish(fake, revision='b' * 40)
        next(self.root.glob('*.zip')).unlink()
        with self.assertRaises(FileNotFoundError):
            self.publish(fake)
        self.assertEqual(fake.calls, [])

    def test_changed_remote_tag_cannot_publish(self):
        fake = FakeGitHub()
        def changed(*args):
            if args[1] == 'api' and '/git/ref/' in args[-1]:
                return json.dumps(dict(object=dict(type='commit', sha='b' * 40)))
            return fake(*args)
        with self.assertRaises(ValueError):
            self.publish(changed)
        self.assertEqual(fake.calls, [])

    def test_annotated_tag_resolves_to_verified_commit(self):
        fake = FakeGitHub()
        def annotated(*args):
            if args[1] == 'api' and '/git/ref/' in args[-1]:
                return json.dumps(dict(object=dict(type='tag', sha='c' * 40)))
            return fake(*args)
        self.publish(annotated)
        self.assertFalse(fake.existing['draft'])

    def test_package_version_mismatch_rejected(self):
        self.create_packages(bad_version='0.2.0')
        fake = FakeGitHub()
        with self.assertRaises(ValueError):
            self.publish(fake)
        self.assertEqual(fake.calls, [])

    def test_upload_failure_leaves_draft_and_retry_recovers(self):
        fake = FakeGitHub(fail_upload=True)
        with self.assertRaises(subprocess.CalledProcessError):
            self.publish(fake)
        self.assertTrue(fake.existing['draft'])
        fake.fail_upload = False
        self.publish(fake)
        self.assertFalse(fake.existing['draft'])
        self.assertEqual(sum(c[1:3] == ('release', 'create') for c in fake.calls), 1)

    def test_incomplete_uploaded_assets_are_not_published(self):
        fake = FakeGitHub(omit_asset=True)
        with self.assertRaises(ValueError):
            self.publish(fake)
        self.assertTrue(fake.existing['draft'])
        self.assertFalse(any(c[1:3] == ('release', 'edit') for c in fake.calls))

    def test_published_release_is_never_changed(self):
        fake = FakeGitHub(existing=dict(tag_name='v0.1.0', draft=False, assets=[]))
        with self.assertRaises(ValueError):
            self.publish(fake)
        self.assertFalse(any(c[1] == 'release' for c in fake.calls))

    def test_service_failure_does_not_assume_release_missing(self):
        calls = []
        def unavailable(*args):
            calls.append(args)
            raise subprocess.CalledProcessError(1, args)
        with self.assertRaises(subprocess.CalledProcessError):
            self.publish(unavailable)
        self.assertFalse(any(c[1] == 'release' for c in calls))


if __name__ == '__main__':
    unittest.main()
