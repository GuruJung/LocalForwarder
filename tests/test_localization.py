import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('localization', ROOT / 'scripts/localization.py')
i18n = importlib.util.module_from_spec(spec)
spec.loader.exec_module(i18n)


class LocalizationCatalogTests(unittest.TestCase):
    def fixture(self):
        return {lang: {'example': 'Host {host}: {count}'} for lang in i18n.LANGUAGES}

    def test_real_catalogs_and_generated_files_are_current(self):
        catalogs = i18n.load_catalogs(ROOT / 'localization')
        i18n.validate_references(ROOT, catalogs)
        for relative, expected in i18n.generated(catalogs).items():
            self.assertEqual((ROOT / relative).read_text(encoding='utf-8'), expected)

    def test_missing_extra_empty_duplicate_and_bad_placeholders_fail(self):
        for mutate in (
            lambda c: c['ja'].clear(),
            lambda c: c['de'].update(extra='Extra'),
            lambda c: c['es'].update(example='  '),
            lambda c: c['ko'].update(example='Host {name}: {count}'),
            lambda c: c['zh-Hant'].update(example='Host {host}: {count'),
            lambda c: c['zh-Hans'].update(example='Host {host}: {count}\x00'),
        ):
            catalogs = self.fixture()
            mutate(catalogs)
            with self.assertRaises(ValueError):
                i18n.validate(catalogs)
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            json.loads('{"example":"first","example":"second"}', object_pairs_hook=i18n.unique_object)

    def test_placeholder_reordering_and_unicode_are_supported(self):
        catalogs = self.fixture()
        catalogs['ja']['example'] = '{count} 件 — {host} の状態'
        i18n.validate(catalogs)

    def test_generator_is_deterministic_and_escapes_literals(self):
        catalogs = self.fixture()
        for entries in catalogs.values():
            entries['quote'] = '"日本語" \\path\nnext\tline'
        i18n.validate(catalogs)
        first = i18n.generated(catalogs)
        reversed_catalogs = {lang: dict(reversed(list(entries.items()))) for lang, entries in catalogs.items()}
        self.assertEqual(first, i18n.generated(reversed_catalogs))
        for output in first.values():
            self.assertIn('\\"日本語\\"', output)
            self.assertIn('\\\\path\\nnext\\tline', output)


if __name__ == '__main__':
    unittest.main()
