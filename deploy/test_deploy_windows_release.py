"""Local validation tests; no network or Windows writes."""
import base64
from pathlib import Path
import tempfile
import unittest
from zipfile import ZipFile

from deploy_windows_release import PACKAGE, powershell_command, validate_archive


class DeploymentTests(unittest.TestCase):
    def test_release_layout(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'release.zip'
            with ZipFile(path, 'w') as z:
                z.writestr(PACKAGE + '/andor_emccd_gui.exe', b'test')
            validate_archive(path)

    def test_rejects_windows_and_unix_zip_traversal(self):
        for bad in ['../bad', PACKAGE + '/../../bad', PACKAGE + r'\..\bad',
                    'C:/bad', PACKAGE + '/file:stream']:
            with self.subTest(bad=bad), tempfile.TemporaryDirectory() as d:
                path = Path(d) / 'release.zip'
                with ZipFile(path, 'w') as z:
                    z.writestr(PACKAGE + '/andor_emccd_gui.exe', b'test')
                    z.writestr(bad, b'test')
                with self.assertRaises(ValueError): validate_archive(path)

    def test_payload_never_becomes_powershell_code(self):
        dangerous = "C:\\local\\'; Write-Host 'injected'"
        command = powershell_command('Write-Output $p.root', {'root': dangerous})
        script = base64.b64decode(command).decode('utf-16le')
        self.assertNotIn(dangerous, script)
        self.assertIn('ConvertFrom-Json', script)


if __name__ == '__main__':
    unittest.main()
