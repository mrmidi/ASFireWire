import argparse
import unittest

from motu_probe import Probe, decode, export_fixture


class FakeClient:
    def __init__(self, reset_at=None, streaming=False, failure=False):
        self.reads = 0
        self.reset_at = reset_at
        self.streaming = streaming
        self.failure = failure
        self.calls = []

    def request(self, method, params):
        if method == "resources/read":
            if params["uri"].endswith("health"):
                return {"data": {"status": "ready"}}
            generation = 0 if self.reset_at is None or self.reads < self.reset_at else 1
            return {"data": {"generation": generation, "nodes": [
                {"guid": "0x123", "nodeId": 2, "vendorId": "0x0001f2"}]}}
        name = params["name"]
        self.calls.append(name)
        if name == "asfw_get_audio_stream_health":
            data = {"endpoints": [{"streaming": self.streaming}]}
        elif name == "asfw_read_quadlet":
            self.reads += 1
            if self.failure:
                return {"structuredContent": {"ok": False, "errors": ["timeout"]}}
            data = {"payload": [0, 0, 1, 0]}
        else:
            data = {"semanticVersion": "test"}
        return {"structuredContent": {"ok": True, "data": data}}


def arguments():
    return argparse.Namespace(guid=0x123, model="828mk3-fw", firmware="unknown",
                              phase="idle", notes="", telemetry_only=False,
                              packet_capture=None, packet_source="unknown")


class ProbeTests(unittest.TestCase):
    def test_reset_stops_before_next_transaction_and_refuses_export(self):
        client = FakeClient(reset_at=1)
        doc = Probe(client).snapshot(arguments())
        self.assertFalse(doc["complete"])
        self.assertEqual(client.reads, 1)
        with self.assertRaises(ValueError):
            export_fixture(doc)

    def test_streaming_collects_telemetry_without_register_reads(self):
        client = FakeClient(streaming=True)
        doc = Probe(client).snapshot(arguments())
        self.assertTrue(doc["complete"])
        self.assertEqual(client.reads, 0)
        with self.assertRaises(ValueError):
            export_fixture(doc)

    def test_transport_failure_preserves_partial_snapshot(self):
        client = FakeClient(failure=True)
        doc = Probe(client).snapshot(arguments())
        self.assertFalse(doc["complete"])
        self.assertEqual(client.reads, 1)
        self.assertIn("timeout", doc["error"])

    def test_zero_generation_and_big_endian_fixture_provenance(self):
        doc = Probe(FakeClient()).snapshot(arguments())
        fixture = export_fixture(doc)
        self.assertEqual(fixture["generation"], 0)
        self.assertEqual(fixture["registers"][0]["value"], 256)
        self.assertFalse(fixture["validatedGolden"])
        self.assertEqual(fixture["firmwareDeclaration"], "unknown")

    def test_reserved_rate_is_unknown_and_optical_directions_are_independent(self):
        self.assertIsNone(decode("828mk3-fw", "clock_status", 0xff00)["sampleRateHz"])
        banks = decode("828mk3-fw", "optical_banks", 0x10001)
        self.assertEqual(banks["inputA"], {"enabled": True, "mode": "spdif"})
        self.assertFalse(banks["outputA"]["enabled"])


if __name__ == "__main__":
    unittest.main()
