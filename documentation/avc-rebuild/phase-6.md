# Phase 6: Oxford + M-Audio on the new layer (outline)

Source: `00-overview.md` stage B3.

- **Duet and Onyx-i onto the new model/engine.** Goldens unchanged. Duet is on the user's desk: verify on
  hardware.
- **M-Audio 1814 / ProjectMix:** replace the assumed allowlist (`AVCCommandFilter.hpp`) with one built from the
  phase-1 1814 matrix (measured, with the user present).
- **Final hardware batch** (see `00-overview.md` Verification):
  - re-run the capture tool on every device and diff it against the phase-1 fixtures;
  - attach, 48k play, rate switch where supported;
  - 1814 attach with measured-safe frames only.

**Later, not in this plan:** tape and panel subunit modules (AVCVideoServices controllers), the target role (Mac as
an AV/C device), DV capture (content side).
