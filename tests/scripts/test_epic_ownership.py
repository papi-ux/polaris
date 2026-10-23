"""Attribution boundaries from the observed Epic 0.101 Linux/Proton route."""

from dataclasses import replace
import importlib.util
from pathlib import Path, PurePosixPath
import sys
import unittest


MODULE = Path(__file__).resolve().parents[2] / "scripts/epic/ownership.py"
SPEC = importlib.util.spec_from_file_location("polaris_epic_ownership", MODULE)
epic = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = epic
SPEC.loader.exec_module(epic)


class EpicOwnershipTest(unittest.TestCase):
    def setUp(self):
        self.title = epic.Title("sandbox", "catalog", "artifact",
                               PurePosixPath("/games/AlanWake2"), "AlanWake2.exe",
                               PurePosixPath("/home/player/.local/share/com.epicgames.eglv2"))
        self.image = epic.Image(self.title.image, 0, 34, 236793710)
        self.prefix = epic.Prefix(52, 237637907)
        self.runtime = self.title.runtime_base / "GE-Proton10-34"
        self.service = "/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service"
        self.desktop = "/user.slice/user-1000.slice/session-44.scope"
        self.launcher = epic.Process(100, 1000, 1, 1000,
                                     PurePosixPath("/tmp/.mount_Epic/usr/bin/EpicGamesLauncher"),
                                     PurePosixPath("/tmp/.mount_Epic/usr"), self.desktop)
        self.wrapper = epic.Process(200, 2001, 100, 1000, PurePosixPath("/usr/bin/python3.14"),
                                    self.title.install_dir, self.desktop)
        self.server = epic.Process(300, 2002, 1, 1000, self.runtime / "files/bin/wineserver",
                                   self.prefix.server_dir(1000), self.desktop)
        # Both are deliberately reparented to init; group/parent ownership misses them.
        self.game = epic.Process(400, 2003, 1, 1000, self.runtime / "files/bin/wine64-preloader",
                                 self.title.install_dir, self.desktop, (self.image,))
        self.other = epic.Process(500, 900, 1, 1000, self.runtime / "files/bin/wineserver",
                                  PurePosixPath("/tmp/.wine-1000/server-34-435e536"), self.desktop)
        self.before = epic.Snapshot("same-boot", 1999, (self.launcher, self.other))
        self.after = epic.Snapshot("same-boot", 2010,
                                   (self.launcher, self.other, self.wrapper, self.server, self.game))

    def claim(self, **kw):
        args = dict(title=self.title, image=self.image, prefix=self.prefix, before=self.before,
                    after=self.after, launcher_identity=self.launcher.identity,
                    request_start_ticks=2000, polaris_cgroup=self.service)
        args.update(kw)
        return epic.match_activation(**args)

    def after_replacing(self, original, changed):
        return replace(self.after, processes=tuple(changed if p == original else p
                                                   for p in self.after.processes))

    def test_warm_activation_claims_reparented_title_not_unrelated_prefix(self):
        binding = self.claim()
        self.assertEqual(binding.game, self.game)
        self.assertEqual(binding.wine_server, self.server)
        self.assertNotEqual(binding.wine_server, self.other)

    def test_primary_uri_uses_all_three_ids_and_no_forwarded_game_args(self):
        self.assertEqual(self.title.uri, "com.epicgames.launcher://apps/sandbox%3Acatalog%3Aartifact?action=launch&silent=true")

    def test_identity_cannot_inject_uri_query_or_command(self):
        for bad in ("", "other?action=uninstall", "id%3Aother", "a b", "$(touch /tmp/x)", "a\n"):
            with self.subTest(bad=bad), self.assertRaises(epic.AttributionError):
                replace(self.title, artifact_id=bad)

    def test_executable_cannot_escape_installation(self):
        for bad in ("", "/other/game.exe", "../game.exe", "sub/../../game.exe", "C:\\game.exe", "x\x00exe"):
            with self.subTest(bad=bad), self.assertRaises(epic.AttributionError):
                replace(self.title, launch_executable=bad)

    def test_btrfs_prefix_server_uses_stat_device_and_inode(self):
        self.assertEqual(str(self.prefix.server_dir(1000)), "/tmp/.wine-1000/server-34-e2a1113")
        self.assertEqual(self.claim().image.device_minor, 34)
        self.assertNotEqual(self.claim().image.device_minor, self.prefix.device)

    def test_existing_game_is_not_adopted(self):
        old = replace(self.game, start_ticks=1900)
        with self.assertRaisesRegex(epic.AttributionError, "already running"):
            self.claim(before=replace(self.before, processes=self.before.processes + (old,)))

    def test_existing_prefix_server_is_not_adopted_even_without_game(self):
        old = replace(self.server, start_ticks=1900)
        with self.assertRaisesRegex(epic.AttributionError, "already running"):
            self.claim(before=replace(self.before, processes=self.before.processes + (old,)))

    def test_existing_wrapper_cannot_be_reclassified_as_a_new_activation(self):
        old = replace(self.wrapper, start_ticks=1900)
        with self.assertRaisesRegex(epic.AttributionError, "already running"):
            self.claim(before=replace(self.before, processes=self.before.processes + (old,)))

    def test_launcher_replaced_during_activation_is_refused(self):
        with self.assertRaisesRegex(epic.AttributionError, "unchanged launcher"):
            self.claim(after=self.after_replacing(self.launcher, replace(self.launcher, start_ticks=1001)))

    def test_runtime_cannot_be_claimed_from_another_prefix(self):
        with self.assertRaisesRegex(epic.AttributionError, "prefix server"):
            self.claim(after=self.after_replacing(self.server, replace(self.server, cwd=self.other.cwd)))

    def test_matching_exe_name_from_another_runtime_is_refused(self):
        different = replace(self.game, executable=self.title.runtime_base / "GE-Proton11/files/bin/wine64-preloader")
        with self.assertRaisesRegex(epic.AttributionError, "title process"):
            self.claim(after=self.after_replacing(self.game, different))

    def test_same_path_with_replaced_file_inode_is_refused(self):
        different = replace(self.game, images=(replace(self.image, inode=self.image.inode + 1),))
        with self.assertRaisesRegex(epic.AttributionError, "title process"):
            self.claim(after=self.after_replacing(self.game, different))

    def test_wrong_filesystem_mapping_is_refused(self):
        different = replace(self.game, images=(replace(self.image, device_minor=52),))
        with self.assertRaisesRegex(epic.AttributionError, "title process"):
            self.claim(after=self.after_replacing(self.game, different))

    def test_title_process_requires_its_own_image_not_just_cwd(self):
        with self.assertRaisesRegex(epic.AttributionError, "title process"):
            self.claim(after=self.after_replacing(self.game, replace(self.game, images=())))

    def test_ambiguous_title_and_wrapper_are_refused(self):
        for process in (self.game, self.wrapper, self.server):
            duplicate = replace(process, pid=process.pid + 1000)
            with self.subTest(process=process.pid), self.assertRaises(epic.AttributionError):
                self.claim(after=replace(self.after, processes=self.after.processes + (duplicate,)))

    def test_same_names_from_other_uid_are_not_claimed(self):
        with self.assertRaisesRegex(epic.AttributionError, "title process"):
            self.claim(after=self.after_replacing(self.game, replace(self.game, uid=2000)))

    def test_restart_or_incomplete_observation_refuses_claim(self):
        for after in (replace(self.after, boot_id="new-boot"), replace(self.after, complete=False)):
            with self.subTest(after=after), self.assertRaises(epic.AttributionError):
                self.claim(after=after)

    def test_processes_before_activation_are_not_claimed(self):
        with self.assertRaisesRegex(epic.AttributionError, "title process"):
            self.claim(after=self.after_replacing(self.game, replace(self.game, start_ticks=1999)))

    def test_missing_clock_interval_is_refused(self):
        for tick in (1998, 2011):
            with self.subTest(tick=tick), self.assertRaises(epic.AttributionError):
                self.claim(request_start_ticks=tick)

    def test_shared_polaris_cgroup_cannot_own_epic_lifetime(self):
        for process in (self.launcher, self.wrapper, self.server, self.game):
            changed = replace(process, cgroup=self.service)
            after = self.after_replacing(process, changed)
            before = self.before
            if process == self.launcher:
                before = replace(before, processes=(changed, self.other))
            with self.subTest(process=process.pid), self.assertRaises(epic.AttributionError):
                self.claim(before=before, after=after)

    def test_revalidation_never_adopts_reused_pid_or_later_launch(self):
        binding = self.claim()
        self.assertIs(epic.revalidate(binding, self.after), binding)
        for process in (self.game, self.server, self.wrapper, self.launcher):
            with self.subTest(process=process.pid), self.assertRaises(epic.AttributionError):
                epic.revalidate(binding, self.after_replacing(process, replace(process, start_ticks=2009)))

    def test_revalidation_refuses_process_identity_change(self):
        binding = self.claim()
        for field, value in (("cwd", PurePosixPath("/other")), ("cgroup", "/other"), ("images", ())):
            with self.subTest(field=field), self.assertRaises(epic.AttributionError):
                epic.revalidate(binding, self.after_replacing(self.game, replace(self.game, **{field: value})))

    def test_duplicate_pid_snapshot_is_invalid(self):
        with self.assertRaisesRegex(epic.AttributionError, "Ambiguous"):
            replace(self.after, processes=self.after.processes + (self.game,))


if __name__ == "__main__":
    unittest.main()
