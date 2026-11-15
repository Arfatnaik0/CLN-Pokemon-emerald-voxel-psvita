#!/usr/bin/env python3
"""Guard the full object-event-table follower path against NPC eviction."""

from pathlib import Path
import re
import unittest


GAME_ROOT = Path(__file__).resolve().parents[2]
FOLLOWER_SOURCE = GAME_ROOT / "src" / "follower_mon.c"


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


class FollowerSpawnSafetyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = FOLLOWER_SOURCE.read_text(encoding="utf-8")

    def test_full_table_returns_without_removing_map_objects(self) -> None:
        body = function_body(self.source, "SpawnFollower")
        self.assertNotIn("FreeObjectEventSlotNearPlayer", body)
        self.assertNotIn("RemoveObjectEvent", body)
        self.assertNotIn("FlagSet", body)
        self.assertRegex(body, r"if\s*\(id\s*>=\s*OBJECT_EVENTS_COUNT\)\s*return NULL;")

    def test_failed_spawn_is_retried_on_the_next_update(self) -> None:
        body = function_body(self.source, "FollowerMon_Update")
        self.assertIn("follower = SpawnFollower(player);", body)
        self.assertRegex(body, r"if\s*\(follower\s*==\s*NULL\)\s*return;")


if __name__ == "__main__":
    unittest.main()
