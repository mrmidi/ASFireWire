# Existing-family audio device template

`FakeExistingFamilyDevice.hpp` is a host-compiled example, guarded by
`ASFW_HOST_TEST`; it cannot register production hardware. It demonstrates the
complete device-only extension surface:

- declarative identity and diagnostics-only safety rules;
- explicit selection of an already-registered family provider;
- optional equivalence candidates narrowed by typed, safe probe facts;
- a pure profile builder selected by ID;
- an optional family-local typed facet; and
- probe success, failure, cancellation, reset, hot-unplug, republish, and strict
  teardown tests in `tests/audio/AudioDeviceSessionManagerTests.cpp`.

For ordinary hardware in an existing family, copy the shape—not the fixture
IDs—into the production audio catalog and the matching family-local profile or
probe data. Discovery, AV/C core, duplex planning, nub construction, and the
DriverKit graph are not extension points.

If the device cannot be expressed through an existing provider and a named
operation policy, stop: it needs a new family/protocol design. Do not hide that
boundary behind vendor/model conditionals in shared code.
