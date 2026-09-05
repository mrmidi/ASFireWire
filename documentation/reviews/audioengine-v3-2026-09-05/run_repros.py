#!/usr/bin/env python3
"""Build and run review reproductions against this checkout's real C++ code.

Requires the normal CMake host-test build to be configured in build/tests_build.
The assertions intentionally confirm current failure signatures. They are not
acceptance tests: after a fix, replace the relevant expectations with invariants.
No driver installation, hardware access, or production-source edits occur.
"""
import json
from pathlib import Path
import shlex
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BUILD = ROOT / "build/tests_build"
OUT = ROOT / "build/audioengine-v3-review"
OUT.mkdir(parents=True, exist_ok=True)


def run(args, cwd=ROOT):
    subprocess.run([str(x) for x in args], cwd=cwd, check=True)


run(["cmake", "--build", BUILD, "--target", "HardwareSampleTimelineTests",
     "AmdtpDirectTxTests", "IsochTxDmaRingTests", "-j", "8"])
commands = json.loads((BUILD / "compile_commands.json").read_text())


def compile_repro(test_source, source, output):
    entry = next(e for e in commands if e["file"].endswith("/" + test_source))
    args = shlex.split(entry["command"])
    args[args.index("-o") + 1] = str(output)
    args[args.index("-c") + 1] = str(source)
    run(args, entry["directory"])


def link_repro(target, test_source, obj, output, extra_objects=()):
    directory = BUILD / "audio"
    args = shlex.split((directory / f"CMakeFiles/{target}.dir/link.txt").read_text())
    args = [str(obj) if x.endswith("/" + test_source + ".o") else x for x in args]
    args = [x for x in args if not x.endswith("/IsochProgressMonitorTests.cpp.o")]
    args[args.index("-o") + 1] = str(output)
    args.extend(str(x) for x in extra_objects)
    run(args, directory)


compile_repro("HardwareSampleTimelineTests.cpp", HERE / "timeline_repro.cpp", OUT / "timeline.o")
cadence = next((BUILD / "audio/CMakeFiles/AmdtpDirectTxTests.dir").rglob("AmdtpCadence.cpp.o"))
link_repro("HardwareSampleTimelineTests", "HardwareSampleTimelineTests.cpp",
           OUT / "timeline.o", OUT / "timeline_repro", [cadence])
run([OUT / "timeline_repro"])

compile_repro("IsochTxDmaRingTests.cpp", HERE / "dma_repro.cpp", OUT / "dma.o")
link_repro("IsochTxDmaRingTests", "IsochTxDmaRingTests.cpp", OUT / "dma.o", OUT / "dma_repro")
run([OUT / "dma_repro", "--gtest_filter=V3ReviewDmaTest.*"])
