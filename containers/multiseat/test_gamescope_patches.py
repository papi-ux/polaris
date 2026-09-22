"""The gamescope a Space runs is the pinned source with exactly the patches its lock names."""
import json
from pathlib import Path
import re
import unittest

HERE = Path(__file__).parent


class GamescopePatches(unittest.TestCase):
    def setUp(self):
        self.lock = json.loads((HERE / 'locks/gamescope.json').read_text())
        self.containerfile = (HERE / 'Containerfile').read_text()

    def test_the_build_applies_every_locked_patch_in_order_and_no_other(self):
        # The lock is what the SBOM and the image labels record. A patch the build applies but the
        # lock leaves out ships unrecorded, and one the lock names but the build skips is recorded
        # but absent.
        applied = re.findall(r'patch -d /gamescope-src -p1 --batch --forward --fuzz=0 < /patches/(\S+)', self.containerfile)
        locked = [Path(path).name for path in self.lock['patches']]
        self.assertEqual(applied, locked)
        for path in self.lock['patches']:
            self.assertTrue((HERE / path).is_file(), path)

    def test_a_forced_fullscreen_reaches_a_window_that_names_its_own_size(self):
        # gamescope 3.16.19 sized any window with size hints to the size it asked for, even with
        # --force-windows-fullscreen. Every GTK window sets size hints, so Lutris opened at 800x600
        # and was scaled into the stream with black bars at both sides. Upstream later added the
        # flag to the same check; this is that change, until the pin moves past it.
        patch = (HERE / 'patches/gamescope-force-windows-fullscreen-desktop.patch').read_text()
        self.assertIn('-\tif ( w->sizeHintsSpecified && !window_is_fullscreen( w ) )', patch)
        self.assertIn('+\tif ( w->sizeHintsSpecified && !(window_is_fullscreen( w ) || ctx->force_windows_fullscreen) )', patch)
        self.assertIn('--force-windows-fullscreen', (HERE.parent.parent / 'multiseat_worker/internal/seatprovider/nested_compositor_linux.go').read_text(),
                      'the patch only matters while the worker starts gamescope with the flag')


if __name__ == '__main__':
    unittest.main()
