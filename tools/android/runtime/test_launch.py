"""Regression tests for inFAMOUS game layout and Android startup defaults."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

LAUNCH = Path(__file__).with_name("launch.py")
spec = importlib.util.spec_from_file_location("android_iss_launch", LAUNCH)
launch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launch)


class LayoutTests(unittest.TestCase):
    def make_game(self, root, resource="art"):
        game = Path(root) / "CUSA00223"
        (game / "sce_sys").mkdir(parents=True)
        (game / "sce_module").mkdir()
        (game / resource).mkdir()
        for path in ("eboot.bin", "sce_sys/param.sfo",
                     "sce_module/libc.prx", "sce_module/libSceFios2.prx"):
            (game / path).write_bytes(b"x")
        (game / resource / "cache").mkdir()
        return game

    def test_infamous_art_layout_accepted_without_psarc_or_dvdroot(self):
        with tempfile.TemporaryDirectory() as tmp:
            launch.check_game_layout(self.make_game(tmp))

    def test_gow3_layout_not_mistaken_for_infamous(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = self.make_game(tmp, "dvdroot_ps4")
            with self.assertRaisesRegex(RuntimeError, "art"):
                launch.check_game_layout(game)

    def test_art_missing_reports_correct_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = self.make_game(tmp)
            (game / "art" / "cache").rmdir()
            (game / "art").rmdir()
            with self.assertRaisesRegex(RuntimeError, "art"):
                launch.check_game_layout(game)

    def test_empty_art_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = self.make_game(tmp)
            (game / "art" / "cache").rmdir()
            with self.assertRaisesRegex(RuntimeError, "empty art"):
                launch.check_game_layout(game)

    def test_empty_required_module_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = self.make_game(tmp)
            (game / "sce_module/libc.prx").write_bytes(b"")
            with self.assertRaisesRegex(RuntimeError, "libc.prx"):
                launch.check_game_layout(game)


class EnvironmentTests(unittest.TestCase):
    def test_iss_pc_defaults_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = {"ISS_READBACKS": "0"}
            launch.setup_environment(env, Path(tmp))
            self.assertEqual(env["ISS_READBACKS"], "0")
            self.assertEqual(env["ISS_DEFERRED_READBACK"], "1")
            self.assertEqual(env["ISS_EAGER_READBACK"], "0")
            self.assertEqual(env["ISS_UPSCALER"], "off")
            self.assertEqual(env["ISS_CONFIG"], str(Path(tmp)/"tlg.ini"))
            self.assertNotIn("TLG_ENGINE_FPS", env)
            self.assertNotIn("TLG_READBACK_LINEAR", env)

    def test_game_dir_config_ini_fallback_and_env_overrides(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as game_tmp:
            cfg = Path(game_tmp) / "config.ini"
            cfg.write_text(
                "# comment\n"
                "ISS_DIRECT_MEM_MB = 3584\n"
                "ISS_BUFFER_CACHE_BUDGET_MB = 384\n"
                "TLG_DROP_MIP0 = 1\n"
                "resolution_scale = 0.5\n"
            )
            env = {"ISS_GAME_DIR": game_tmp}
            launch.setup_environment(env, Path(tmp))
            self.assertEqual(env["ISS_CONFIG"], str(cfg))
            self.assertEqual(env["TLG_CONFIG"], str(cfg))
            self.assertEqual(env["ISS_DIRECT_MEM_MB"], "3584")
            self.assertEqual(env["ISS_BUFFER_CACHE_BUDGET_MB"], "384")
            self.assertEqual(env["TLG_DROP_MIP0"], "1")
            self.assertNotIn("resolution_scale", env)

if __name__ == "__main__":
    unittest.main()

class ShellAudioContractTests(unittest.TestCase):
    def test_sdl3_disk_output_matches_android_bridge(self):
        script = Path(__file__).resolve().parents[1] / "device" / "run-iss.sh"
        code = script.read_text()
        self.assertIn('SDL_AUDIO_DISK_OUTPUT_FILE="$base/audio.fifo"', code)
        self.assertIn('SDL_AUDIO_FORMAT=S16LE', code)
        self.assertIn('SDL_AUDIO_CHANNELS=2', code)
        self.assertIn('SDL_AUDIO_FREQUENCY=48000', code)
        self.assertNotIn('SDL_DISKAUDIOFILE=', code)

