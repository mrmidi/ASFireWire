# ASFireWire documentation

This directory contains design notes, implementation guides, investigations, and
supporting reports that are useful during development but are not repository
entry points.

## Design and planning

- [Audio backend controls](AUDIO_BACKENDS_CONTROLS.md)
- [Audio TX code-review guide](AUDIO_TX_CODE_REVIEW_GUIDE.md)
- [BeBoB refactoring plan](BeBoB_REFACTOR.md)
- [Clean-code refactor ledger](CLEAN_CODE.md)
- [Isoch/audio cleanup preparation](ISOCH_AUDIO_CLEANUP_PREP.md)
- [Volume control TODO](VOLUME_TODO.md)

## Investigations and regression records

- [AV/C recovery and duplex SYT](AVC_RECOVERY_AND_SYNC_ALGO_AND_BUGS.md)
- [AV/C stream health and recovery](AVC_STREAM_HEALTH_AND_RECOVERY.md)
- [Bug list](BUGLIST.md)
- [DICE stream regression](DICE_REGRESSION.md)
- [DICE stability regression](DICE_STABILITY_REGRESSION.md)
- [TerraTec PHASE 88 topology research](TERRATEC_TOPOLOGY_RESEARCH.md)

The other Markdown files in this directory document protocol behavior, timing,
MCP tooling, lifecycle, and prior review decisions. Their filenames are intended
to be self-describing; use this index as the starting point when browsing.

## Reports

- [ASFireWire timing and recovery triage report](reports/ASFireWire%20timing%20and%20recovery%20triage%20report.pdf)
- [AVC recovery triage findings](reports/AVC_RECOVERY_TRIAGE_FINDINGS.pdf)

## Reference material

The separate [`docs/`](../docs/) directory contains imported standards and
reference-specific material. It remains separate from this project-authored
documentation so those two roles do not get mixed together.
