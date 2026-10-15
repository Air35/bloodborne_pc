# SPDX-License-Identifier: GPL-2.0-or-later
"""Import spelling of link_modules.py: NID#library, the same for every game (sotc, A1)."""
import paths  # noqa: F401  (puts scripts/ on sys.path)
import unittest
import link_modules


class CanonicalNameTests(unittest.TestCase):
    def test_identity_becomes_nid_and_library(self):
        identity = ('vNe1w4diLCs', ('libkernel', 1), ('libkernel', 1))
        self.assertEqual(link_modules.canonical(identity), 'vNe1w4diLCs#libkernel')

    def test_library_version_and_module_do_not_change_the_name(self):
        a = link_modules.canonical(('1jfXLRVzisc', ('libScePosix', 1), ('libkernel', 1)))
        b = link_modules.canonical(('1jfXLRVzisc', ('libScePosix', 2), ('libc', 1)))
        self.assertEqual(a, b)
        self.assertEqual(a, '1jfXLRVzisc#libScePosix')

    def test_name_without_identity_keeps_the_fallback(self):
        self.assertEqual(link_modules.canonical(None, 'raw#c#C'), 'raw#c#C')


if __name__ == '__main__':
    unittest.main()
