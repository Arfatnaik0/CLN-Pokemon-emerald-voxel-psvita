#!/usr/bin/env python3
"""Keep Vita startup VCOUNT priming ahead of the first game thread frame."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*\)\s*\{{", source)
    if match is None:
        raise AssertionError(f"function {name} was not found")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1 : index]
    raise AssertionError(f"function {name} has an unterminated body")


def assert_vita_bootstrap_seeded_before_thread(body: str) -> None:
    marker = body.index("/* Prime the emulated LCD")
    vita_start = body.rfind("#ifdef PLATFORM_VITA", 0, marker)
    vita_end = body.index("#else", marker)
    thread_start = body.index('SDL_CreateThread(DoMain, "AgbMain", NULL)')
    vita_branch = body[vita_start:vita_end]
    seed = "REG_VCOUNT = 161;"

    if vita_start < 0 or seed not in vita_branch:
        raise AssertionError("Vita pre-thread branch must seed REG_VCOUNT")
    if body.index(seed, vita_start, vita_end) >= thread_start:
        raise AssertionError("Vita VCOUNT seed must precede game-thread creation")


class VitaVBlankStartupSourceTest(unittest.TestCase):
    def test_display_is_seeded_before_game_thread_creation(self) -> None:
        sdl2 = (ROOT / "src/platform/sdl2.c").read_text(encoding="utf-8")
        body = function_body(sdl2, "main")
        assert_vita_bootstrap_seeded_before_thread(body)

        # A later Linux-only seed must not satisfy the Vita startup assertion.
        without_vita_seed = body.replace("    REG_VCOUNT = 161;\n", "", 1)
        with self.assertRaisesRegex(AssertionError, "Vita pre-thread branch"):
            assert_vita_bootstrap_seeded_before_thread(without_vita_seed)

    def test_vblank_gate_flushes_registers_and_runs_mixer(self) -> None:
        sdl2 = (ROOT / "src/platform/sdl2.c").read_text(encoding="utf-8")
        main = (ROOT / "src/main.c").read_text(encoding="utf-8")
        loop = function_body(sdl2, "main")
        handler = function_body(main, "VBlankIntr")

        self.assertLess(loop.index("REG_DISPSTAT |= INTR_FLAG_VBLANK;"),
                        loop.index("if (REG_DISPSTAT & DISPSTAT_VBLANK_INTR)"))
        self.assertIn("gIntrTable[4]();", loop)
        self.assertLess(handler.index("CopyBufferedValuesToGpuRegs();"),
                        handler.index("m4aSoundMain();"))


if __name__ == "__main__":
    unittest.main()
