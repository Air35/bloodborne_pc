"""Boundary tests for the native loader; uses tiny synthetic x86-64 images."""
from paths import ROOT
from pathlib import Path
import struct
import os
import re
import subprocess
import tempfile
import unittest

EXE = ROOT / 'out/bb-probe'


def package(code, relocs=(), names=(), capabilities=None):
    image = code.ljust(4096, b'\0')
    header = struct.pack('<8sQQQQQ', b'BBPROBE1' if capabilities is None else b'BBPROBE2', len(image), 0, 1, len(relocs), len(names))
    if capabilities is not None:
        header += struct.pack('<Q', capabilities)
    segment = struct.pack('<QQQ', 0, len(image), 5)
    return (header + segment + b''.join(n.encode().ljust(128, b'\0') for n in names)
            + b''.join(struct.pack('<QQqq', *r) for r in relocs) + image)


def native_package(name='fixture-native', binding_address=4112, binding_kind=1,
                   lib_flags=5, init=b'\x31\xc0\xc3', native=b'\xc3', metadata=None):
    # Main calls an import then tail-jumps to the terminal diagnostic import.
    code = b'\x48\x83\xec\x08\xff\x15\x16\0\0\0\x48\x83\xc4\x08\xff\x25\x14\0\0\0'
    image = code.ljust(4096,b'\0') + init.ljust(16,b'\0') + native
    image = image.ljust(12288,b'\0')
    header=struct.pack('<8s6Q',b'BBPROBE3',len(image),0,3,2,2,1)
    meta=metadata or (4096,8192,4096,8192,32,4,1,256)
    return (header + struct.pack('<8Q',*meta) + struct.pack('<3Q',0,binding_address,binding_kind)
            + struct.pack('<9Q',0,4096,5,4096,4096,lib_flags,8192,4096,6)
            + name.encode().ljust(128,b'\0') + b'after-native'.ljust(128,b'\0')
            + struct.pack('<8Q',32,1,0,0,40,1,1,0) + image)


@unittest.skipUnless(EXE.exists(), 'build native probe first')
class LoaderTests(unittest.TestCase):
    def test_invalid_content_profile_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'content.bin'
            for data in (b'BBCONT01',struct.pack('<8s5I',b'BBCONT01',2,0,0,0,0),
                         struct.pack('<8s5I',b'BBCONT01',3,0,0,0,0)+b'extra'):
                with self.subTest(data=data):
                    path.write_bytes(data)
                    r=self.run_image(package(b'\xc3'),'--content-profile',str(path))
                    self.assertEqual(r.returncode,1,r.stdout+r.stderr)
                    self.assertIn('content profile',r.stderr)

    def run_image(self, data, *options):
        # sotc: these tests are about the loader, not about the CPU: independent of the host's
        # instruction sets (a host that lacks some would stop with STOP/20, docs/10 §3).
        env = dict(os.environ, BB_SKIP_CPU_CHECK='1')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'boot.bin'
            path.write_bytes(data)
            return subprocess.run([str(EXE.resolve()), str(path), '--cpu-only', *options],
                                  capture_output=True, text=True, timeout=5, env=env)

    def cpu_features(self, mode=None):
        env = {k: v for k, v in os.environ.items() if k not in ('BB_SKIP_CPU_CHECK', 'SOTC_ISA_MODE')}
        if mode is not None:
            env['SOTC_ISA_MODE'] = mode
        return subprocess.run([str(EXE.resolve()), '--cpu-features'], capture_output=True, text=True,
                              timeout=5, env=env)

    def test_cpu_features_reports_the_plan_or_stops(self):
        # sotc: docs/10 §3. A host with a fatal set missing answers STOP/20; otherwise CPU + ISA lines.
        r = self.cpu_features()
        self.assertIn(r.returncode, (0, 20), r.stdout + r.stderr)
        if r.returncode == 0:
            self.assertRegex(r.stdout, r'CPU: (complete|missing .+)\nISA: rewrite=.+\n')
        else:
            self.assertIn('STOP:', r.stdout)

    def test_isa_mode_is_validated_never_silently_auto(self):
        r = self.cpu_features('bogus')
        if 'STOP: this CPU lacks instruction sets' not in r.stdout:    # a fatal set missing stops first
            self.assertEqual(r.returncode, 20, r.stdout)
            self.assertIn('SOTC_ISA_MODE=bogus', r.stdout)

    def test_rewrite_mode_names_every_rewritable_set(self):
        r = self.cpu_features('rewrite')
        if r.returncode == 0:
            self.assertIn('ISA: rewrite=MOVBE, F16C, BMI1, LZCNT, SSE4a', r.stdout)

    def test_native_mode_stops_when_something_is_missing(self):
        plan = self.cpu_features()
        r = self.cpu_features('native')
        if plan.returncode == 0 and 'CPU: missing' in plan.stdout:
            self.assertEqual(r.returncode, 20, r.stdout)
            self.assertIn('does not rewrite', r.stdout)
        elif plan.returncode == 0:
            self.assertEqual(r.returncode, 0, r.stdout)

    def test_original_instruction_reaches_named_import(self):
        # jmp [rip+2]; two padding bytes; relocated function pointer at +8
        r = self.run_image(package(b'\xff\x25\x02\0\0\0\x90\x90', [(8, 1, 0, 0)], ['fixture-import']))
        self.assertEqual(r.returncode, 20, r.stderr)
        self.assertIn('first unsupported PS4 import: fixture-import', r.stdout)

    def test_native_initializer_and_export_return(self):
        r=self.run_image(native_package())
        self.assertEqual(r.returncode,20,r.stdout+r.stderr)
        self.assertIn('Module 0 initializer returned 0',r.stdout)
        self.assertIn('first unsupported PS4 import: after-native',r.stdout)

    def test_host_contract_takes_priority_over_native_export(self):
        r=self.run_image(native_package(name='bzQExy189ZI#libc',native=b'\x0f\x0b'))
        self.assertEqual(r.returncode,20,r.stdout+r.stderr)
        self.assertIn('_init_env returned',r.stdout)
        self.assertIn('first unsupported PS4 import: after-native',r.stdout)

    def test_strict_mode_skips_native_initialization_and_binding(self):
        r=self.run_image(native_package(init=b'\x0f\x0b'), '--strict-imports')
        self.assertEqual(r.returncode,20,r.stdout+r.stderr)
        self.assertIn('first unsupported PS4 import: fixture-native',r.stdout)
        self.assertNotIn('Starting native libc',r.stdout)

    def test_native_metadata_and_bindings_are_validated(self):
        for kwargs,message in [
            ({'binding_address':12288},'invalid native binding'),
            ({'binding_kind':2},'native export kind/range mismatch'),
            ({'lib_flags':4},'native function is not executable'),
            ({'metadata':(4096,8192,4096,8192,32,33,1,256)},'invalid linked module metadata'),
            ({'metadata':(4096,8192,4096,8192,32,4,1,12280)},'unmapped procparam'),
        ]:
            with self.subTest(kwargs=kwargs):
                r=self.run_image(native_package(**kwargs))
                self.assertEqual(r.returncode,1,r.stdout+r.stderr)
                self.assertIn(message,r.stderr)

    def test_failed_native_initializer_does_not_enter_game(self):
        r=self.run_image(native_package(init=b'\xb8\x01\0\0\0\xc3'))
        self.assertEqual(r.returncode,1,r.stdout+r.stderr)
        self.assertIn('module initializer failed',r.stderr)
        self.assertNotIn('Entering original',r.stdout)

    def test_base_relative_address_is_relocated(self):
        # jump via a relative relocation to code at +16, then to an import at +24
        code = b'\xff\x25\x02\0\0\0\x90\x90' + b'\0'*8 + b'\xff\x25\x02\0\0\0\x90\x90'
        r = self.run_image(package(code, [(8, 0, 16, 0), (24, 1, 0, 0)], ['after-relative-jump']))
        self.assertEqual(r.returncode, 20, r.stderr)
        self.assertIn('after-relative-jump', r.stdout)

    def test_image_is_mapped_below_2_gib(self):
        # sotc: the game packs pointers to its static data into 31 bits (ShadowOfTheColossus-PC
        # 70bb438 src/probe.c:55-58; ADR-0019), so the image must end below 0x80000000 on Linux
        # too. lea rdi,[rip] hands the code's own address to an unresolved import, whose STOP
        # line prints it as the first argument.
        code = b'\x48\x8d\x3d\0\0\0\0' + b'\xff\x25\x03\0\0\0' + b'\x90' * 3 + b'\0' * 8
        r = self.run_image(package(code, [(16, 1, 0, 0)], ['image-base-check']))
        self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
        self.assertIn('image-base-check', r.stdout + r.stderr)
        found = re.search(r'first argument: (0x[0-9a-f]+)', r.stdout + r.stderr)
        self.assertIsNotNone(found, r.stdout + r.stderr)
        self.assertLess(int(found[1], 16), 0x80000000)

    def test_out_of_bounds_relocation_is_rejected(self):
        r = self.run_image(package(b'\xc3', [(4092, 0, 0, 0)]))
        self.assertEqual(r.returncode, 1)
        self.assertIn('bad relocation', r.stderr)

    def test_truncated_image_is_rejected(self):
        r = self.run_image(package(b'\xc3')[:-1])
        self.assertEqual(r.returncode, 1)
        self.assertIn('incorrect memory image size', r.stderr)

    def test_illegal_instruction_reports_guest_offset(self):
        r = self.run_image(package(b'\x0f\x0b'))
        self.assertEqual(r.returncode, 132)
        self.assertIn('at guest offset 0x0,', r.stderr)

    def test_runtime_returns_from_verified_init_env(self):
        # Align stack, call _init_env through +32, then tail-jump to unknown +40.
        code = b'\x48\x83\xec\x08\xff\x15\x16\0\0\0\x48\x83\xc4\x08\xff\x25\x14\0\0\0'
        data = package(code, [(32,1,0,0),(40,1,1,0)], ['bzQExy189ZI#libc','after-init'], 1)
        r = self.run_image(data)
        self.assertEqual(r.returncode,20,r.stderr)
        self.assertIn('first unsupported PS4 import: after-init',r.stdout)
        self.assertIn('_init_env=1',r.stdout)
        strict = self.run_image(data,'--strict-imports')
        self.assertEqual(strict.returncode,20,strict.stderr)
        self.assertIn('first unsupported PS4 import: bzQExy189ZI#libc',strict.stdout)

    def test_check_imports_lists_missing_without_running(self):
        # sotc: --check-imports lists what would stop the game without running it; it needs no
        # CPU check (no guest code runs) and, with --cpu-only, reports GPU-library imports apart.
        names = ['bzQExy189ZI#libc', 'AAAAAAAAAAA#libSceFoo', 'BBBBBBBBBBB#libSceGnmDriver']
        data = package(b'\xc3' + b'\0' * 31, [(8,1,0,0),(16,1,1,0),(24,1,2,0)], names, 1)
        env = {k: v for k, v in os.environ.items() if k != 'BB_SKIP_CPU_CHECK'}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'boot.bin'
            path.write_bytes(data)
            r = subprocess.run([str(EXE.resolve()), str(path), '--cpu-only', '--check-imports'],
                               capture_output=True, text=True, timeout=5, env=env)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn('MISSING: AAAAAAAAAAA#libSceFoo', r.stdout)
        self.assertNotIn('bzQExy189ZI', r.stdout)
        self.assertIn('GPU (not checked without the GPU library): BBBBBBBBBBB#libSceGnmDriver', r.stdout)
        self.assertIn('Imports: 1 missing, 1 left to the GPU library', r.stdout)

    def test_unverified_runtime_stays_disabled(self):
        r = self.run_image(package(b'\xff\x25\x02\0\0\0\x90\x90',[(8,1,0,0)],['bzQExy189ZI#libc'],0))
        self.assertEqual(r.returncode,20,r.stderr)
        self.assertIn('_init_env=0',r.stdout)

    def test_unknown_capabilities_rejected(self):
        r = self.run_image(package(b'\xc3',capabilities=2))
        self.assertEqual(r.returncode,1)
        self.assertIn('unknown runtime capabilities',r.stderr)


if __name__ == '__main__':
    unittest.main()
