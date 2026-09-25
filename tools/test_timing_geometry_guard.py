"""The guard must catch what it claims to, and nothing it should not."""

from __future__ import annotations

import unittest

from timing_geometry_guard import scan_text

AUDIO = "ASFWDriver/Audio/DriverKit/ASFWAudioDevice.cpp"
BUS = "ASFWDriver/Bus/TopologyTypes.hpp"


def rules(rel: str, text: str) -> list[str]:
    return [v.rule for v in scan_text(rel, text)]


class RetiredNames(unittest.TestCase):
    def test_retired_identifier_is_caught(self) -> None:
        self.assertEqual(rules(AUDIO, "auto p = TimingCursorPolicy{};"), ["retired-name"])
        self.assertEqual(rules(AUDIO, "x = Geometry::kFrameRingFrames;"), ["retired-name"])
        self.assertEqual(rules(AUDIO, "if (IsLiveCompatible(a, b)) {}"), ["retired-name"])

    def test_successor_names_are_not_confused_with_retired_ones(self) -> None:
        self.assertEqual(rules(AUDIO, "x = Geometry::kAllocatedFrameRingFrames;"), [])
        self.assertEqual(rules(AUDIO, "x = timing.frameRingFrames;"), [])

    def test_comments_and_strings_may_mention_history(self) -> None:
        self.assertEqual(rules(AUDIO, "// replaces TimingCursorPolicy"), [])
        self.assertEqual(rules(AUDIO, "/* was kFrameRingFrames\n = 1536 */ int a;"), [])
        self.assertEqual(rules(AUDIO, 'log("TimingCursorPolicy gone");'), [])

    def test_double_transfer_delay_is_caught(self) -> None:
        self.assertEqual(rules(AUDIO, "uint32_t TransferDelayTicks(double rate);"),
                         ["retired-pattern"])


class ProfileLookup(unittest.TestCase):
    def test_second_lookup_is_caught(self) -> None:
        self.assertEqual(rules(AUDIO, "auto* p = AudioProfileRegistry::FindProfile(v, m, g, b);"),
                         ["second-profile-lookup"])

    def test_graph_may_resolve_once(self) -> None:
        graph = "ASFWDriver/Audio/DriverKit/ASFWAudioDriverGraph.cpp"
        self.assertEqual(rules(graph, "auto* p = AudioProfileRegistry::FindProfile(v, m, g, b);"), [])


class GeometryLiterals(unittest.TestCase):
    def test_literals_in_audio_code_are_caught(self) -> None:
        self.assertEqual(rules(AUDIO, "std::atomic<uint32_t> d{12800};"), ["geometry-literal"])
        self.assertEqual(rules(AUDIO, "constexpr uint32_t ring = 12'288;"), ["geometry-literal"])
        self.assertEqual(rules(AUDIO, "if (frames == 1536) {}"), ["geometry-literal"])

    def test_owners_and_other_layers_are_exempt(self) -> None:
        owner = "ASFWDriver/Shared/Isoch/AudioHalBufferProfiles.hpp"
        self.assertEqual(rules(owner, "inline constexpr uint32_t k = 12'288;"), [])
        self.assertEqual(rules(BUS, "kSpeedToMbps = {800, 1600, 3200, 6400, 12800};"), [])

    def test_longer_numbers_do_not_match(self) -> None:
        self.assertEqual(rules(AUDIO, "x = 128000; y = 15360; z = 0x1536;"), [])


if __name__ == "__main__":
    unittest.main()
