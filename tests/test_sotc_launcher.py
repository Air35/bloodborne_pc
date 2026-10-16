# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: sotc.py on Linux (ADR-0019): finds bb-probe without .exe, runs the scripts isolated
(python3 -I) with their folder on the path, writes into SOTC_OUT_DIR, and asks the probe which
instruction sets need the rewrite table (docs/10 §7)."""
import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT


def load_launcher():
    spec = importlib.util.spec_from_file_location('sotc_launcher', ROOT / 'sotc.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.sotc = load_launcher()

    def test_probe_is_found_without_exe_suffix(self):
        with tempfile.TemporaryDirectory() as tmp:
            port = Path(tmp)
            (port / 'out').mkdir()
            probe = port / 'out' / ('bb-probe.exe' if os.name == 'nt' else 'bb-probe')
            probe.write_text('')
            with mock.patch.object(self.sotc, 'PORT', port):
                self.assertEqual(self.sotc.find_probe(), probe)

    def test_scripts_run_isolated_with_their_folder_on_the_path(self):
        calls = []

        def fake_run(command, **kwargs):
            calls.append(command)
            return mock.Mock(returncode=0)

        with mock.patch.object(self.sotc.subprocess, 'run', fake_run):
            self.sotc.run_script('prepare.py', 'game', '--out', 'out')
        command = calls[0]
        self.assertEqual(command[0], sys.executable)
        self.assertIn('-I', command[1:3])
        self.assertIn(str(self.sotc.SCRIPTS), ' '.join(command))
        self.assertEqual(command[-3:], ['game', '--out', 'out'])

    def test_no_console_is_zero_off_windows(self):
        if os.name == 'nt':
            self.skipTest('Windows keeps its console logic')
        self.assertEqual(self.sotc.no_console(), 0)

    def test_isa_table_only_when_the_probe_asks(self):
        made = []
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            for plan, expected in (('CPU: complete\nISA: rewrite=none\n', []),
                                   ('CPU: missing MOVBE\nISA: rewrite=MOVBE\n',
                                    ['--isa-rewrite', str(out / 'isa-rewrite.bin')])):
                with self.subTest(plan=plan), \
                        mock.patch.object(self.sotc.subprocess, 'run',
                                          return_value=mock.Mock(returncode=0, stdout=plan)), \
                        mock.patch.object(self.sotc, 'run_script',
                                          side_effect=lambda *a: made.append(a)):
                    self.assertEqual(self.sotc.isa_args(Path('probe'), out), expected)
        self.assertEqual(made, [('isa_rewrite.py', out / 'boot-linked.bin', '--out', out)])

    def test_out_dir_and_update_from_the_sandbox_variables(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = {'SOTC_OUT_DIR': str(Path(tmp) / 'prepared'), 'SOTC_PATCH_DIR': '/patch'}
            with mock.patch.dict(os.environ, env, clear=False):
                os.environ.pop('SOTC_UPDATE_DIR', None)
                self.assertEqual(self.sotc.out_dir(), Path(tmp) / 'prepared')
                self.assertEqual(self.sotc.game_update(Path('/nowhere/game')), Path('/patch'))


if __name__ == '__main__':
    unittest.main()
