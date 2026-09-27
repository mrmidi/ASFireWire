# AV/C Hardware Probe Matrix: TerraTec Electronic GmbH PHASE 88 Rack FW

- **Key**: `phase88`
- **GUID**: `0x000AAC0300B1D1F7`
- **Node ID**: `1` (generation 1)
- **Driver Version**: `0.3.1`
- **Captured At**: `2026-09-27T19:17:20.843715+00:00`
- **Total Exchanges**: `364`

| Exchange | Intent | Command (Hex) | Response Code | Duration (µs) | Result |
|---|---|---|---|---|---|
| `unit_info` | STATUS | `01 FF 30 07 FF FF FF FF` | STABLE (0x0C) | 6776 | OK |
| `subunit_info_page_0` | STATUS | `01 FF 31 07 FF FF FF FF` | STABLE (0x0C) | 6091 | OK |
| `subunit_info_page_1` | STATUS | `01 FF 31 17 FF FF FF FF` | NOT IMPLEMENTED (0x08) | 6674 | OK |
| `plug_info_unit_00` | STATUS | `01 FF 02 00 FF FF FF FF` | STABLE (0x0C) | 6738 | OK |
| `plug_info_unit_01` | STATUS | `01 FF 02 01 FF FF FF FF` | STABLE (0x0C) | 6708 | OK |
| `plug_info_audio_0` | STATUS | `01 08 02 00 FF FF FF FF` | STABLE (0x0C) | 5952 | OK |
| `plug_info_music_0` | STATUS | `01 60 02 00 FF FF FF FF` | STABLE (0x0C) | 6909 | OK |
| `plug_signal_format_in_0_all_wildcard` | STATUS | `01 FF 19 00 FF FF FF FF` | STABLE (0x0C) | 6739 | OK |
| `plug_signal_format_in_0_am824_wildcard` | STATUS | `01 FF 19 00 90 FF FF FF` | STABLE (0x0C) | 6837 | OK |
| `plug_signal_format_in_1_all_wildcard` | STATUS | `01 FF 19 01 FF FF FF FF` | STABLE (0x0C) | 5987 | OK |
| `plug_signal_format_in_1_am824_wildcard` | STATUS | `01 FF 19 01 90 FF FF FF` | STABLE (0x0C) | 6021 | OK |
| `plug_signal_format_out_0_all_wildcard` | STATUS | `01 FF 18 00 FF FF FF FF` | STABLE (0x0C) | 6677 | OK |
| `plug_signal_format_out_0_am824_wildcard` | STATUS | `01 FF 18 00 90 FF FF FF` | STABLE (0x0C) | 6561 | OK |
| `plug_signal_format_out_1_all_wildcard` | STATUS | `01 FF 18 01 FF FF FF FF` | STABLE (0x0C) | 6635 | OK |
| `plug_signal_format_out_1_am824_wildcard` | STATUS | `01 FF 18 01 90 FF FF FF` | STABLE (0x0C) | 6420 | OK |
| `stream_format_0x2F_single_unit_iso_in_0` | STATUS | `01 FF 2F C0 00 00 00 00 FF FF` | STABLE (0x0C) | 17038 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_0` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 00` | STABLE (0x0C) | 17266 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_1` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 01` | STABLE (0x0C) | 17518 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_2` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 02` | STABLE (0x0C) | 18074 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_3` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 03` | STABLE (0x0C) | 17695 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_4` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 04` | STABLE (0x0C) | 18369 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_5` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 05` | REJECTED (0x0A) | 6140 | OK |
| `stream_format_0xBF_single_unit_iso_in_0` | STATUS | `01 FF BF C0 00 00 00 00 FF FF` | NOT IMPLEMENTED (0x08) | 6434 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_0` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 00` | NOT IMPLEMENTED (0x08) | 6476 | OK |
| `stream_format_0x2F_single_unit_iso_in_1` | STATUS | `01 FF 2F C0 00 00 00 01 FF FF` | STABLE (0x0C) | 11351 | OK |
| `stream_format_0x2F_list_unit_iso_in_1_idx_0` | STATUS | `01 FF 2F C1 00 00 00 01 FF FF 00` | STABLE (0x0C) | 11848 | OK |
| `stream_format_0x2F_list_unit_iso_in_1_idx_1` | STATUS | `01 FF 2F C1 00 00 00 01 FF FF 01` | STABLE (0x0C) | 12243 | OK |
| `stream_format_0x2F_list_unit_iso_in_1_idx_2` | STATUS | `01 FF 2F C1 00 00 00 01 FF FF 02` | STABLE (0x0C) | 11600 | OK |
| `stream_format_0x2F_list_unit_iso_in_1_idx_3` | STATUS | `01 FF 2F C1 00 00 00 01 FF FF 03` | STABLE (0x0C) | 12571 | OK |
| `stream_format_0x2F_list_unit_iso_in_1_idx_4` | STATUS | `01 FF 2F C1 00 00 00 01 FF FF 04` | STABLE (0x0C) | 12411 | OK |
| `stream_format_0x2F_list_unit_iso_in_1_idx_5` | STATUS | `01 FF 2F C1 00 00 00 01 FF FF 05` | REJECTED (0x0A) | 6061 | OK |
| `stream_format_0xBF_single_unit_iso_in_1` | STATUS | `01 FF BF C0 00 00 00 01 FF FF` | NOT IMPLEMENTED (0x08) | 6680 | OK |
| `stream_format_0xBF_list_unit_iso_in_1_idx_0` | STATUS | `01 FF BF C1 00 00 00 01 FF FF 00` | NOT IMPLEMENTED (0x08) | 5775 | OK |
| `stream_format_0x2F_single_unit_iso_out_0` | STATUS | `01 FF 2F C0 01 00 00 00 FF FF` | STABLE (0x0C) | 23985 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_0` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 00` | STABLE (0x0C) | 24103 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_1` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 01` | STABLE (0x0C) | 23275 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_2` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 02` | STABLE (0x0C) | 23337 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_3` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 03` | STABLE (0x0C) | 23396 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_4` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 04` | STABLE (0x0C) | 22838 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_5` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 05` | REJECTED (0x0A) | 6419 | OK |
| `stream_format_0xBF_single_unit_iso_out_0` | STATUS | `01 FF BF C0 01 00 00 00 FF FF` | NOT IMPLEMENTED (0x08) | 6714 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_0` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 00` | NOT IMPLEMENTED (0x08) | 6678 | OK |
| `stream_format_0x2F_single_unit_iso_out_1` | STATUS | `01 FF 2F C0 01 00 00 01 FF FF` | STABLE (0x0C) | 11821 | OK |
| `stream_format_0x2F_list_unit_iso_out_1_idx_0` | STATUS | `01 FF 2F C1 01 00 00 01 FF FF 00` | STABLE (0x0C) | 12220 | OK |
| `stream_format_0x2F_list_unit_iso_out_1_idx_1` | STATUS | `01 FF 2F C1 01 00 00 01 FF FF 01` | STABLE (0x0C) | 11783 | OK |
| `stream_format_0x2F_list_unit_iso_out_1_idx_2` | STATUS | `01 FF 2F C1 01 00 00 01 FF FF 02` | STABLE (0x0C) | 12228 | OK |
| `stream_format_0x2F_list_unit_iso_out_1_idx_3` | STATUS | `01 FF 2F C1 01 00 00 01 FF FF 03` | STABLE (0x0C) | 11802 | OK |
| `stream_format_0x2F_list_unit_iso_out_1_idx_4` | STATUS | `01 FF 2F C1 01 00 00 01 FF FF 04` | STABLE (0x0C) | 12136 | OK |
| `stream_format_0x2F_list_unit_iso_out_1_idx_5` | STATUS | `01 FF 2F C1 01 00 00 01 FF FF 05` | REJECTED (0x0A) | 6875 | OK |
| `stream_format_0xBF_single_unit_iso_out_1` | STATUS | `01 FF BF C0 01 00 00 01 FF FF` | NOT IMPLEMENTED (0x08) | 6058 | OK |
| `stream_format_0xBF_list_unit_iso_out_1_idx_0` | STATUS | `01 FF BF C1 01 00 00 01 FF FF 00` | NOT IMPLEMENTED (0x08) | 6142 | OK |
| `stream_format_0x2F_single_music_0_dest_0` | STATUS | `01 60 2F C0 00 01 00 FF FF FF` | STABLE (0x0C) | 18253 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_0` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 00` | STABLE (0x0C) | 17562 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_1` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 01` | STABLE (0x0C) | 17434 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_2` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 02` | STABLE (0x0C) | 17659 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_3` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 03` | STABLE (0x0C) | 17557 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_4` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 04` | STABLE (0x0C) | 23559 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_5` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 05` | REJECTED (0x0A) | 6757 | OK |
| `stream_format_0xBF_single_music_0_dest_0` | STATUS | `01 60 BF C0 00 01 00 FF FF FF` | NOT IMPLEMENTED (0x08) | 6121 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_0` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6254 | OK |
| `stream_format_0x2F_single_music_0_dest_1` | STATUS | `01 60 2F C0 00 01 01 FF FF FF` | STABLE (0x0C) | 11470 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_0` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 00` | STABLE (0x0C) | 12265 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_1` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 01` | STABLE (0x0C) | 11652 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_2` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 02` | STABLE (0x0C) | 11752 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_3` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 03` | STABLE (0x0C) | 11932 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_4` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 04` | STABLE (0x0C) | 12499 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_5` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 05` | REJECTED (0x0A) | 6108 | OK |
| `stream_format_0xBF_single_music_0_dest_1` | STATUS | `01 60 BF C0 00 01 01 FF FF FF` | NOT IMPLEMENTED (0x08) | 6196 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_0` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6587 | OK |
| `stream_format_0x2F_single_music_0_dest_2` | STATUS | `01 60 2F C0 00 01 02 FF FF FF` | STABLE (0x0C) | 12265 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_0` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 00` | STABLE (0x0C) | 12452 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_1` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 01` | STABLE (0x0C) | 12197 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_2` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 02` | STABLE (0x0C) | 11739 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_3` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 03` | STABLE (0x0C) | 12426 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_4` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 04` | STABLE (0x0C) | 11705 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_5` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 05` | REJECTED (0x0A) | 6639 | OK |
| `stream_format_0xBF_single_music_0_dest_2` | STATUS | `01 60 BF C0 00 01 02 FF FF FF` | NOT IMPLEMENTED (0x08) | 6508 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_0` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6169 | OK |
| `stream_format_0x2F_single_music_0_dest_3` | STATUS | `01 60 2F C0 00 01 03 FF FF FF` | STABLE (0x0C) | 12478 | OK |
| `stream_format_0x2F_list_music_0_dest_3_idx_0` | STATUS | `01 60 2F C1 00 01 03 FF FF FF 00` | STABLE (0x0C) | 12732 | OK |
| `stream_format_0x2F_list_music_0_dest_3_idx_1` | STATUS | `01 60 2F C1 00 01 03 FF FF FF 01` | STABLE (0x0C) | 11765 | OK |
| `stream_format_0x2F_list_music_0_dest_3_idx_2` | STATUS | `01 60 2F C1 00 01 03 FF FF FF 02` | STABLE (0x0C) | 11847 | OK |
| `stream_format_0x2F_list_music_0_dest_3_idx_3` | STATUS | `01 60 2F C1 00 01 03 FF FF FF 03` | STABLE (0x0C) | 12732 | OK |
| `stream_format_0x2F_list_music_0_dest_3_idx_4` | STATUS | `01 60 2F C1 00 01 03 FF FF FF 04` | STABLE (0x0C) | 12589 | OK |
| `stream_format_0x2F_list_music_0_dest_3_idx_5` | STATUS | `01 60 2F C1 00 01 03 FF FF FF 05` | REJECTED (0x0A) | 6793 | OK |
| `stream_format_0xBF_single_music_0_dest_3` | STATUS | `01 60 BF C0 00 01 03 FF FF FF` | NOT IMPLEMENTED (0x08) | 6752 | OK |
| `stream_format_0xBF_list_music_0_dest_3_idx_0` | STATUS | `01 60 BF C1 00 01 03 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6101 | OK |
| `stream_format_0x2F_single_music_0_dest_4` | STATUS | `01 60 2F C0 00 01 04 FF FF FF` | STABLE (0x0C) | 12509 | OK |
| `stream_format_0x2F_list_music_0_dest_4_idx_0` | STATUS | `01 60 2F C1 00 01 04 FF FF FF 00` | STABLE (0x0C) | 11809 | OK |
| `stream_format_0x2F_list_music_0_dest_4_idx_1` | STATUS | `01 60 2F C1 00 01 04 FF FF FF 01` | STABLE (0x0C) | 12232 | OK |
| `stream_format_0x2F_list_music_0_dest_4_idx_2` | STATUS | `01 60 2F C1 00 01 04 FF FF FF 02` | STABLE (0x0C) | 12758 | OK |
| `stream_format_0x2F_list_music_0_dest_4_idx_3` | STATUS | `01 60 2F C1 00 01 04 FF FF FF 03` | STABLE (0x0C) | 11785 | OK |
| `stream_format_0x2F_list_music_0_dest_4_idx_4` | STATUS | `01 60 2F C1 00 01 04 FF FF FF 04` | STABLE (0x0C) | 11986 | OK |
| `stream_format_0x2F_list_music_0_dest_4_idx_5` | STATUS | `01 60 2F C1 00 01 04 FF FF FF 05` | REJECTED (0x0A) | 6371 | OK |
| `stream_format_0xBF_single_music_0_dest_4` | STATUS | `01 60 BF C0 00 01 04 FF FF FF` | NOT IMPLEMENTED (0x08) | 5587 | OK |
| `stream_format_0xBF_list_music_0_dest_4_idx_0` | STATUS | `01 60 BF C1 00 01 04 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6168 | OK |
| `stream_format_0x2F_single_music_0_dest_5` | STATUS | `01 60 2F C0 00 01 05 FF FF FF` | STABLE (0x0C) | 12539 | OK |
| `stream_format_0x2F_list_music_0_dest_5_idx_0` | STATUS | `01 60 2F C1 00 01 05 FF FF FF 00` | STABLE (0x0C) | 12454 | OK |
| `stream_format_0x2F_list_music_0_dest_5_idx_1` | STATUS | `01 60 2F C1 00 01 05 FF FF FF 01` | STABLE (0x0C) | 12727 | OK |
| `stream_format_0x2F_list_music_0_dest_5_idx_2` | STATUS | `01 60 2F C1 00 01 05 FF FF FF 02` | STABLE (0x0C) | 12456 | OK |
| `stream_format_0x2F_list_music_0_dest_5_idx_3` | STATUS | `01 60 2F C1 00 01 05 FF FF FF 03` | STABLE (0x0C) | 11783 | OK |
| `stream_format_0x2F_list_music_0_dest_5_idx_4` | STATUS | `01 60 2F C1 00 01 05 FF FF FF 04` | STABLE (0x0C) | 11856 | OK |
| `stream_format_0x2F_list_music_0_dest_5_idx_5` | STATUS | `01 60 2F C1 00 01 05 FF FF FF 05` | REJECTED (0x0A) | 6352 | OK |
| `stream_format_0xBF_single_music_0_dest_5` | STATUS | `01 60 BF C0 00 01 05 FF FF FF` | NOT IMPLEMENTED (0x08) | 6574 | OK |
| `stream_format_0xBF_list_music_0_dest_5_idx_0` | STATUS | `01 60 BF C1 00 01 05 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6604 | OK |
| `stream_format_0x2F_single_music_0_dest_6` | STATUS | `01 60 2F C0 00 01 06 FF FF FF` | STABLE (0x0C) | 11317 | OK |
| `stream_format_0x2F_list_music_0_dest_6_idx_0` | STATUS | `01 60 2F C1 00 01 06 FF FF FF 00` | STABLE (0x0C) | 12966 | OK |
| `stream_format_0x2F_list_music_0_dest_6_idx_1` | STATUS | `01 60 2F C1 00 01 06 FF FF FF 01` | STABLE (0x0C) | 12095 | OK |
| `stream_format_0x2F_list_music_0_dest_6_idx_2` | STATUS | `01 60 2F C1 00 01 06 FF FF FF 02` | STABLE (0x0C) | 11724 | OK |
| `stream_format_0x2F_list_music_0_dest_6_idx_3` | STATUS | `01 60 2F C1 00 01 06 FF FF FF 03` | STABLE (0x0C) | 11790 | OK |
| `stream_format_0x2F_list_music_0_dest_6_idx_4` | STATUS | `01 60 2F C1 00 01 06 FF FF FF 04` | STABLE (0x0C) | 11722 | OK |
| `stream_format_0x2F_list_music_0_dest_6_idx_5` | STATUS | `01 60 2F C1 00 01 06 FF FF FF 05` | REJECTED (0x0A) | 11969 | OK |
| `stream_format_0xBF_single_music_0_dest_6` | STATUS | `01 60 BF C0 00 01 06 FF FF FF` | NOT IMPLEMENTED (0x08) | 6287 | OK |
| `stream_format_0xBF_list_music_0_dest_6_idx_0` | STATUS | `01 60 BF C1 00 01 06 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6456 | OK |
| `stream_format_0x2F_single_music_0_dest_7` | STATUS | `01 60 2F C0 00 01 07 FF FF FF` | STABLE (0x0C) | 12229 | OK |
| `stream_format_0x2F_list_music_0_dest_7_idx_0` | STATUS | `01 60 2F C1 00 01 07 FF FF FF 00` | STABLE (0x0C) | 11439 | OK |
| `stream_format_0x2F_list_music_0_dest_7_idx_1` | STATUS | `01 60 2F C1 00 01 07 FF FF FF 01` | STABLE (0x0C) | 12335 | OK |
| `stream_format_0x2F_list_music_0_dest_7_idx_2` | STATUS | `01 60 2F C1 00 01 07 FF FF FF 02` | STABLE (0x0C) | 12042 | OK |
| `stream_format_0x2F_list_music_0_dest_7_idx_3` | STATUS | `01 60 2F C1 00 01 07 FF FF FF 03` | STABLE (0x0C) | 12295 | OK |
| `stream_format_0x2F_list_music_0_dest_7_idx_4` | STATUS | `01 60 2F C1 00 01 07 FF FF FF 04` | STABLE (0x0C) | 11759 | OK |
| `stream_format_0x2F_list_music_0_dest_7_idx_5` | STATUS | `01 60 2F C1 00 01 07 FF FF FF 05` | REJECTED (0x0A) | 11820 | OK |
| `stream_format_0xBF_single_music_0_dest_7` | STATUS | `01 60 BF C0 00 01 07 FF FF FF` | NOT IMPLEMENTED (0x08) | 6749 | OK |
| `stream_format_0xBF_list_music_0_dest_7_idx_0` | STATUS | `01 60 BF C1 00 01 07 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6790 | OK |
| `stream_format_0x2F_single_music_0_dest_8` | STATUS | `01 60 2F C0 00 01 08 FF FF FF` | STABLE (0x0C) | 6245 | OK |
| `stream_format_0x2F_list_music_0_dest_8_idx_0` | STATUS | `01 60 2F C1 00 01 08 FF FF FF 00` | STABLE (0x0C) | 12436 | OK |
| `stream_format_0x2F_list_music_0_dest_8_idx_1` | STATUS | `01 60 2F C1 00 01 08 FF FF FF 01` | STABLE (0x0C) | 11685 | OK |
| `stream_format_0x2F_list_music_0_dest_8_idx_2` | STATUS | `01 60 2F C1 00 01 08 FF FF FF 02` | STABLE (0x0C) | 12060 | OK |
| `stream_format_0x2F_list_music_0_dest_8_idx_3` | STATUS | `01 60 2F C1 00 01 08 FF FF FF 03` | STABLE (0x0C) | 12166 | OK |
| `stream_format_0x2F_list_music_0_dest_8_idx_4` | STATUS | `01 60 2F C1 00 01 08 FF FF FF 04` | STABLE (0x0C) | 12683 | OK |
| `stream_format_0x2F_list_music_0_dest_8_idx_5` | STATUS | `01 60 2F C1 00 01 08 FF FF FF 05` | REJECTED (0x0A) | 11734 | OK |
| `stream_format_0xBF_single_music_0_dest_8` | STATUS | `01 60 BF C0 00 01 08 FF FF FF` | NOT IMPLEMENTED (0x08) | 7700 | OK |
| `stream_format_0xBF_list_music_0_dest_8_idx_0` | STATUS | `01 60 BF C1 00 01 08 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6072 | OK |
| `stream_format_0x2F_single_music_0_dest_9` | STATUS | `01 60 2F C0 00 01 09 FF FF FF` | STABLE (0x0C) | 12163 | OK |
| `stream_format_0x2F_list_music_0_dest_9_idx_0` | STATUS | `01 60 2F C1 00 01 09 FF FF FF 00` | STABLE (0x0C) | 11529 | OK |
| `stream_format_0x2F_list_music_0_dest_9_idx_1` | STATUS | `01 60 2F C1 00 01 09 FF FF FF 01` | STABLE (0x0C) | 12079 | OK |
| `stream_format_0x2F_list_music_0_dest_9_idx_2` | STATUS | `01 60 2F C1 00 01 09 FF FF FF 02` | STABLE (0x0C) | 11626 | OK |
| `stream_format_0x2F_list_music_0_dest_9_idx_3` | STATUS | `01 60 2F C1 00 01 09 FF FF FF 03` | STABLE (0x0C) | 12732 | OK |
| `stream_format_0x2F_list_music_0_dest_9_idx_4` | STATUS | `01 60 2F C1 00 01 09 FF FF FF 04` | STABLE (0x0C) | 12333 | OK |
| `stream_format_0x2F_list_music_0_dest_9_idx_5` | STATUS | `01 60 2F C1 00 01 09 FF FF FF 05` | REJECTED (0x0A) | 12359 | OK |
| `stream_format_0xBF_single_music_0_dest_9` | STATUS | `01 60 BF C0 00 01 09 FF FF FF` | NOT IMPLEMENTED (0x08) | 6155 | OK |
| `stream_format_0xBF_list_music_0_dest_9_idx_0` | STATUS | `01 60 BF C1 00 01 09 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6752 | OK |
| `stream_format_0x2F_single_music_0_src_0` | STATUS | `01 60 2F C0 01 01 00 FF FF FF` | STABLE (0x0C) | 23279 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_0` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 00` | STABLE (0x0C) | 23054 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_1` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 01` | STABLE (0x0C) | 22690 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_2` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 02` | STABLE (0x0C) | 24183 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_3` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 03` | STABLE (0x0C) | 24239 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_4` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 04` | STABLE (0x0C) | 24045 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_5` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 05` | REJECTED (0x0A) | 6350 | OK |
| `stream_format_0xBF_single_music_0_src_0` | STATUS | `01 60 BF C0 01 01 00 FF FF FF` | NOT IMPLEMENTED (0x08) | 6462 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_0` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6728 | OK |
| `stream_format_0x2F_single_music_0_src_1` | STATUS | `01 60 2F C0 01 01 01 FF FF FF` | STABLE (0x0C) | 12325 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_0` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 00` | STABLE (0x0C) | 11749 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_1` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 01` | STABLE (0x0C) | 11506 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_2` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 02` | STABLE (0x0C) | 11752 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_3` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 03` | STABLE (0x0C) | 12392 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_4` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 04` | STABLE (0x0C) | 12164 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_5` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 05` | REJECTED (0x0A) | 11737 | OK |
| `stream_format_0xBF_single_music_0_src_1` | STATUS | `01 60 BF C0 01 01 01 FF FF FF` | NOT IMPLEMENTED (0x08) | 5699 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_0` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6802 | OK |
| `stream_format_0x2F_single_music_0_src_2` | STATUS | `01 60 2F C0 01 01 02 FF FF FF` | STABLE (0x0C) | 11839 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_0` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 00` | STABLE (0x0C) | 12310 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_1` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 01` | STABLE (0x0C) | 12326 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_2` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 02` | STABLE (0x0C) | 11880 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_3` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 03` | STABLE (0x0C) | 12453 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_4` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 04` | STABLE (0x0C) | 12689 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_5` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 05` | REJECTED (0x0A) | 6924 | OK |
| `stream_format_0xBF_single_music_0_src_2` | STATUS | `01 60 BF C0 01 01 02 FF FF FF` | NOT IMPLEMENTED (0x08) | 6050 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_0` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6702 | OK |
| `stream_format_0x2F_single_music_0_src_3` | STATUS | `01 60 2F C0 01 01 03 FF FF FF` | STABLE (0x0C) | 6924 | OK |
| `stream_format_0x2F_list_music_0_src_3_idx_0` | STATUS | `01 60 2F C1 01 01 03 FF FF FF 00` | REJECTED (0x0A) | 6096 | OK |
| `stream_format_0xBF_single_music_0_src_3` | STATUS | `01 60 BF C0 01 01 03 FF FF FF` | NOT IMPLEMENTED (0x08) | 6349 | OK |
| `stream_format_0xBF_list_music_0_src_3_idx_0` | STATUS | `01 60 BF C1 01 01 03 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6811 | OK |
| `stream_format_0x2F_single_music_0_src_4` | STATUS | `01 60 2F C0 01 01 04 FF FF FF` | STABLE (0x0C) | 6070 | OK |
| `stream_format_0x2F_list_music_0_src_4_idx_0` | STATUS | `01 60 2F C1 01 01 04 FF FF FF 00` | REJECTED (0x0A) | 6147 | OK |
| `stream_format_0xBF_single_music_0_src_4` | STATUS | `01 60 BF C0 01 01 04 FF FF FF` | NOT IMPLEMENTED (0x08) | 6080 | OK |
| `stream_format_0xBF_list_music_0_src_4_idx_0` | STATUS | `01 60 BF C1 01 01 04 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6787 | OK |
| `stream_format_0x2F_single_music_0_src_5` | STATUS | `01 60 2F C0 01 01 05 FF FF FF` | STABLE (0x0C) | 12513 | OK |
| `stream_format_0x2F_list_music_0_src_5_idx_0` | STATUS | `01 60 2F C1 01 01 05 FF FF FF 00` | STABLE (0x0C) | 12695 | OK |
| `stream_format_0x2F_list_music_0_src_5_idx_1` | STATUS | `01 60 2F C1 01 01 05 FF FF FF 01` | STABLE (0x0C) | 11767 | OK |
| `stream_format_0x2F_list_music_0_src_5_idx_2` | STATUS | `01 60 2F C1 01 01 05 FF FF FF 02` | STABLE (0x0C) | 12593 | OK |
| `stream_format_0x2F_list_music_0_src_5_idx_3` | STATUS | `01 60 2F C1 01 01 05 FF FF FF 03` | STABLE (0x0C) | 12292 | OK |
| `stream_format_0x2F_list_music_0_src_5_idx_4` | STATUS | `01 60 2F C1 01 01 05 FF FF FF 04` | STABLE (0x0C) | 12411 | OK |
| `stream_format_0x2F_list_music_0_src_5_idx_5` | STATUS | `01 60 2F C1 01 01 05 FF FF FF 05` | REJECTED (0x0A) | 6962 | OK |
| `stream_format_0xBF_single_music_0_src_5` | STATUS | `01 60 BF C0 01 01 05 FF FF FF` | NOT IMPLEMENTED (0x08) | 6508 | OK |
| `stream_format_0xBF_list_music_0_src_5_idx_0` | STATUS | `01 60 BF C1 01 01 05 FF FF FF 00` | NOT IMPLEMENTED (0x08) | 6785 | OK |
| `signal_source_0xFF_music_0_dest_0` | STATUS | `01 FF 1A FF FF FE 60 00` | STABLE (0x0C) | 5962 | OK |
| `signal_source_0x0F_music_0_dest_0` | STATUS | `01 FF 1A 0F FF FE 60 00` | STABLE (0x0C) | 6752 | OK |
| `signal_source_0xFF_music_0_dest_1` | STATUS | `01 FF 1A FF FF FE 60 01` | STABLE (0x0C) | 6947 | OK |
| `signal_source_0x0F_music_0_dest_1` | STATUS | `01 FF 1A 0F FF FE 60 01` | STABLE (0x0C) | 6160 | OK |
| `signal_source_0xFF_music_0_dest_2` | STATUS | `01 FF 1A FF FF FE 60 02` | STABLE (0x0C) | 6048 | OK |
| `signal_source_0x0F_music_0_dest_2` | STATUS | `01 FF 1A 0F FF FE 60 02` | STABLE (0x0C) | 6909 | OK |
| `signal_source_0xFF_music_0_dest_3` | STATUS | `01 FF 1A FF FF FE 60 03` | STABLE (0x0C) | 6880 | OK |
| `signal_source_0x0F_music_0_dest_3` | STATUS | `01 FF 1A 0F FF FE 60 03` | STABLE (0x0C) | 6631 | OK |
| `signal_source_0xFF_music_0_dest_4` | STATUS | `01 FF 1A FF FF FE 60 04` | STABLE (0x0C) | 6667 | OK |
| `signal_source_0x0F_music_0_dest_4` | STATUS | `01 FF 1A 0F FF FE 60 04` | STABLE (0x0C) | 6189 | OK |
| `signal_source_0xFF_music_0_dest_5` | STATUS | `01 FF 1A FF FF FE 60 05` | STABLE (0x0C) | 6412 | OK |
| `signal_source_0x0F_music_0_dest_5` | STATUS | `01 FF 1A 0F FF FE 60 05` | STABLE (0x0C) | 11858 | OK |
| `signal_source_0xFF_music_0_dest_6` | STATUS | `01 FF 1A FF FF FE 60 06` | STABLE (0x0C) | 6127 | OK |
| `signal_source_0x0F_music_0_dest_6` | STATUS | `01 FF 1A 0F FF FE 60 06` | STABLE (0x0C) | 6579 | OK |
| `signal_source_0xFF_music_0_dest_7` | STATUS | `01 FF 1A FF FF FE 60 07` | STABLE (0x0C) | 6119 | OK |
| `signal_source_0x0F_music_0_dest_7` | STATUS | `01 FF 1A 0F FF FE 60 07` | STABLE (0x0C) | 6688 | OK |
| `signal_source_0xFF_music_0_dest_8` | STATUS | `01 FF 1A FF FF FE 60 08` | STABLE (0x0C) | 6914 | OK |
| `signal_source_0x0F_music_0_dest_8` | STATUS | `01 FF 1A 0F FF FE 60 08` | STABLE (0x0C) | 6147 | OK |
| `signal_source_0xFF_music_0_dest_9` | STATUS | `01 FF 1A FF FF FE 60 09` | STABLE (0x0C) | 6075 | OK |
| `signal_source_0x0F_music_0_dest_9` | STATUS | `01 FF 1A 0F FF FE 60 09` | STABLE (0x0C) | 7107 | OK |
| `bridgeco_plug_info_plug_type_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 00` | STABLE (0x0C) | 6686 | OK |
| `bridgeco_plug_info_plug_name_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 01` | STABLE (0x0C) | 6851 | OK |
| `bridgeco_plug_info_channel_count_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 02` | STABLE (0x0C) | 6652 | OK |
| `bridgeco_plug_info_channel_positions_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 03` | STABLE (0x0C) | 11943 | OK |
| `bridgeco_plug_info_channel_name_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 04` | NOT IMPLEMENTED (0x08) | 6049 | OK |
| `bridgeco_plug_info_plug_input_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 05` | STABLE (0x0C) | 6573 | OK |
| `bridgeco_plug_info_plug_outputs_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 06` | STABLE (0x0C) | 6964 | OK |
| `bridgeco_cluster_info_in_0_sec_1` | STATUS | `01 FF 02 C0 00 00 00 00 FF 07 01 00 00 00 00` | STABLE (0x0C) | 6893 | OK |
| `bridgeco_cluster_info_in_0_sec_2` | STATUS | `01 FF 02 C0 00 00 00 00 FF 07 02 00 00 00 00` | STABLE (0x0C) | 6258 | OK |
| `bridgeco_cluster_info_in_0_sec_3` | STATUS | `01 FF 02 C0 00 00 00 00 FF 07 03 00 00 00 00` | STABLE (0x0C) | 6672 | OK |
| `bridgeco_plug_info_plug_type_in_1` | STATUS | `01 FF 02 C0 00 00 00 01 FF 00` | STABLE (0x0C) | 6705 | OK |
| `bridgeco_plug_info_plug_name_in_1` | STATUS | `01 FF 02 C0 00 00 00 01 FF 01` | STABLE (0x0C) | 6551 | OK |
| `bridgeco_plug_info_channel_count_in_1` | STATUS | `01 FF 02 C0 00 00 00 01 FF 02` | STABLE (0x0C) | 6129 | OK |
| `bridgeco_plug_info_channel_positions_in_1` | STATUS | `01 FF 02 C0 00 00 00 01 FF 03` | STABLE (0x0C) | 6109 | OK |
| `bridgeco_plug_info_channel_name_in_1` | STATUS | `01 FF 02 C0 00 00 00 01 FF 04` | NOT IMPLEMENTED (0x08) | 6243 | OK |
| `bridgeco_plug_info_plug_input_in_1` | STATUS | `01 FF 02 C0 00 00 00 01 FF 05` | STABLE (0x0C) | 6469 | OK |
| `bridgeco_plug_info_plug_outputs_in_1` | STATUS | `01 FF 02 C0 00 00 00 01 FF 06` | STABLE (0x0C) | 6597 | OK |
| `bridgeco_cluster_info_in_1_sec_1` | STATUS | `01 FF 02 C0 00 00 00 01 FF 07 01 00 00 00 00` | REJECTED (0x0A) | 6651 | OK |
| `bridgeco_plug_info_plug_type_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 00` | STABLE (0x0C) | 6060 | OK |
| `bridgeco_plug_info_plug_name_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 01` | STABLE (0x0C) | 6425 | OK |
| `bridgeco_plug_info_channel_count_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 02` | STABLE (0x0C) | 6505 | OK |
| `bridgeco_plug_info_channel_positions_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 03` | STABLE (0x0C) | 11753 | OK |
| `bridgeco_plug_info_channel_name_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 04` | NOT IMPLEMENTED (0x08) | 6240 | OK |
| `bridgeco_plug_info_plug_input_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 05` | STABLE (0x0C) | 6811 | OK |
| `bridgeco_plug_info_plug_outputs_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 06` | STABLE (0x0C) | 6705 | OK |
| `bridgeco_cluster_info_out_0_sec_1` | STATUS | `01 FF 02 C0 01 00 00 00 FF 07 01 00 00 00 00` | STABLE (0x0C) | 6255 | OK |
| `bridgeco_cluster_info_out_0_sec_2` | STATUS | `01 FF 02 C0 01 00 00 00 FF 07 02 00 00 00 00` | STABLE (0x0C) | 11844 | OK |
| `bridgeco_cluster_info_out_0_sec_3` | STATUS | `01 FF 02 C0 01 00 00 00 FF 07 03 00 00 00 00` | STABLE (0x0C) | 6478 | OK |
| `bridgeco_cluster_info_out_0_sec_4` | STATUS | `01 FF 02 C0 01 00 00 00 FF 07 04 00 00 00 00` | STABLE (0x0C) | 6054 | OK |
| `bridgeco_cluster_info_out_0_sec_5` | STATUS | `01 FF 02 C0 01 00 00 00 FF 07 05 00 00 00 00` | STABLE (0x0C) | 6198 | OK |
| `bridgeco_cluster_info_out_0_sec_6` | STATUS | `01 FF 02 C0 01 00 00 00 FF 07 06 00 00 00 00` | STABLE (0x0C) | 6916 | OK |
| `bridgeco_plug_info_plug_type_out_1` | STATUS | `01 FF 02 C0 01 00 00 01 FF 00` | STABLE (0x0C) | 6397 | OK |
| `bridgeco_plug_info_plug_name_out_1` | STATUS | `01 FF 02 C0 01 00 00 01 FF 01` | STABLE (0x0C) | 6851 | OK |
| `bridgeco_plug_info_channel_count_out_1` | STATUS | `01 FF 02 C0 01 00 00 01 FF 02` | STABLE (0x0C) | 6821 | OK |
| `bridgeco_plug_info_channel_positions_out_1` | STATUS | `01 FF 02 C0 01 00 00 01 FF 03` | STABLE (0x0C) | 6221 | OK |
| `bridgeco_plug_info_channel_name_out_1` | STATUS | `01 FF 02 C0 01 00 00 01 FF 04` | NOT IMPLEMENTED (0x08) | 12615 | OK |
| `bridgeco_plug_info_plug_input_out_1` | STATUS | `01 FF 02 C0 01 00 00 01 FF 05` | STABLE (0x0C) | 7004 | OK |
| `bridgeco_plug_info_plug_outputs_out_1` | STATUS | `01 FF 02 C0 01 00 00 01 FF 06` | STABLE (0x0C) | 6097 | OK |
| `bridgeco_cluster_info_out_1_sec_1` | STATUS | `01 FF 02 C0 01 00 00 01 FF 07 01 00 00 00 00` | REJECTED (0x0A) | 6683 | OK |
| `function_block_selector_status_fb_0` | STATUS | `01 08 B8 80 00 10 02 FF 01` | NOT IMPLEMENTED (0x08) | 6875 | OK |
| `function_block_selector_status_fb_1` | STATUS | `01 08 B8 80 01 10 02 FF 01` | STABLE (0x0C) | 6063 | OK |
| `function_block_selector_status_fb_2` | STATUS | `01 08 B8 80 02 10 02 FF 01` | STABLE (0x0C) | 6587 | OK |
| `function_block_selector_status_fb_3` | STATUS | `01 08 B8 80 03 10 02 FF 01` | STABLE (0x0C) | 6664 | OK |
| `function_block_selector_status_fb_4` | STATUS | `01 08 B8 80 04 10 02 FF 01` | STABLE (0x0C) | 6860 | OK |
| `function_block_selector_status_fb_5` | STATUS | `01 08 B8 80 05 10 02 FF 01` | STABLE (0x0C) | 11512 | OK |
| `function_block_selector_status_fb_6` | STATUS | `01 08 B8 80 06 10 02 FF 01` | STABLE (0x0C) | 6352 | OK |
| `function_block_selector_status_fb_7` | STATUS | `01 08 B8 80 07 10 02 FF 01` | STABLE (0x0C) | 6502 | OK |
| `function_block_selector_status_fb_8` | STATUS | `01 08 B8 80 08 10 02 FF 01` | STABLE (0x0C) | 12400 | OK |
| `function_block_selector_status_fb_9` | STATUS | `01 08 B8 80 09 10 02 FF 01` | STABLE (0x0C) | 12158 | OK |
| `function_block_selector_status_fb_10` | STATUS | `01 08 B8 80 0A 10 02 FF 01` | STABLE (0x0C) | 6628 | OK |
| `function_block_selector_status_fb_11` | STATUS | `01 08 B8 80 0B 10 02 FF 01` | NOT IMPLEMENTED (0x08) | 6129 | OK |
| `function_block_feature_mute_status_fb_0` | STATUS | `01 08 B8 81 00 10 02 00 01 01 FF` | NOT IMPLEMENTED (0x08) | 12440 | OK |
| `function_block_feature_volume_current_fb_0` | STATUS | `01 08 B8 81 00 10 02 00 02 02 FF FF` | NOT IMPLEMENTED (0x08) | 12148 | OK |
| `function_block_feature_mute_status_fb_1` | STATUS | `01 08 B8 81 01 10 02 00 01 01 FF` | STABLE (0x0C) | 6622 | OK |
| `function_block_feature_volume_current_fb_1` | STATUS | `01 08 B8 81 01 10 02 00 02 02 FF FF` | STABLE (0x0C) | 6326 | OK |
| `function_block_feature_volume_min_fb_1` | STATUS | `01 08 B8 81 01 02 02 00 02 02 FF FF` | STABLE (0x0C) | 6743 | OK |
| `function_block_feature_volume_max_fb_1` | STATUS | `01 08 B8 81 01 03 02 00 02 02 FF FF` | STABLE (0x0C) | 6130 | OK |
| `function_block_feature_mute_status_fb_2` | STATUS | `01 08 B8 81 02 10 02 00 01 01 FF` | STABLE (0x0C) | 6027 | OK |
| `function_block_feature_volume_current_fb_2` | STATUS | `01 08 B8 81 02 10 02 00 02 02 FF FF` | STABLE (0x0C) | 6447 | OK |
| `function_block_feature_volume_min_fb_2` | STATUS | `01 08 B8 81 02 02 02 00 02 02 FF FF` | STABLE (0x0C) | 6204 | OK |
| `function_block_feature_volume_max_fb_2` | STATUS | `01 08 B8 81 02 03 02 00 02 02 FF FF` | STABLE (0x0C) | 6235 | OK |
| `function_block_feature_mute_status_fb_3` | STATUS | `01 08 B8 81 03 10 02 00 01 01 FF` | STABLE (0x0C) | 11821 | OK |
| `function_block_feature_volume_current_fb_3` | STATUS | `01 08 B8 81 03 10 02 00 02 02 FF FF` | STABLE (0x0C) | 6600 | OK |
| `function_block_feature_volume_min_fb_3` | STATUS | `01 08 B8 81 03 02 02 00 02 02 FF FF` | STABLE (0x0C) | 6853 | OK |
| `function_block_feature_volume_max_fb_3` | STATUS | `01 08 B8 81 03 03 02 00 02 02 FF FF` | STABLE (0x0C) | 12342 | OK |
| `function_block_feature_mute_status_fb_4` | STATUS | `01 08 B8 81 04 10 02 00 01 01 FF` | STABLE (0x0C) | 11816 | OK |
| `function_block_feature_volume_current_fb_4` | STATUS | `01 08 B8 81 04 10 02 00 02 02 FF FF` | STABLE (0x0C) | 6688 | OK |
| `function_block_feature_volume_min_fb_4` | STATUS | `01 08 B8 81 04 02 02 00 02 02 FF FF` | STABLE (0x0C) | 6649 | OK |
| `function_block_feature_volume_max_fb_4` | STATUS | `01 08 B8 81 04 03 02 00 02 02 FF FF` | STABLE (0x0C) | 6991 | OK |
| `function_block_feature_mute_status_fb_5` | STATUS | `01 08 B8 81 05 10 02 00 01 01 FF` | STABLE (0x0C) | 11217 | OK |
| `function_block_feature_volume_current_fb_5` | STATUS | `01 08 B8 81 05 10 02 00 02 02 FF FF` | STABLE (0x0C) | 6471 | OK |
| `function_block_feature_volume_min_fb_5` | STATUS | `01 08 B8 81 05 02 02 00 02 02 FF FF` | STABLE (0x0C) | 6204 | OK |
| `function_block_feature_volume_max_fb_5` | STATUS | `01 08 B8 81 05 03 02 00 02 02 FF FF` | STABLE (0x0C) | 11264 | OK |
| `function_block_feature_mute_status_fb_6` | STATUS | `01 08 B8 81 06 10 02 00 01 01 FF` | STABLE (0x0C) | 12395 | OK |
| `function_block_feature_volume_current_fb_6` | STATUS | `01 08 B8 81 06 10 02 00 02 02 FF FF` | STABLE (0x0C) | 11270 | OK |
| `function_block_feature_volume_min_fb_6` | STATUS | `01 08 B8 81 06 02 02 00 02 02 FF FF` | STABLE (0x0C) | 11813 | OK |
| `function_block_feature_volume_max_fb_6` | STATUS | `01 08 B8 81 06 03 02 00 02 02 FF FF` | STABLE (0x0C) | 6916 | OK |
| `function_block_feature_mute_status_fb_7` | STATUS | `01 08 B8 81 07 10 02 00 01 01 FF` | STABLE (0x0C) | 6649 | OK |
| `function_block_feature_volume_current_fb_7` | STATUS | `01 08 B8 81 07 10 02 00 02 02 FF FF` | STABLE (0x0C) | 6978 | OK |
| `function_block_feature_volume_min_fb_7` | STATUS | `01 08 B8 81 07 02 02 00 02 02 FF FF` | STABLE (0x0C) | 6739 | OK |
| `function_block_feature_volume_max_fb_7` | STATUS | `01 08 B8 81 07 03 02 00 02 02 FF FF` | STABLE (0x0C) | 11822 | OK |
| `function_block_feature_mute_status_fb_8` | STATUS | `01 08 B8 81 08 10 02 00 01 01 FF` | NOT IMPLEMENTED (0x08) | 11408 | OK |
| `function_block_feature_volume_current_fb_8` | STATUS | `01 08 B8 81 08 10 02 00 02 02 FF FF` | NOT IMPLEMENTED (0x08) | 7898 | OK |
| `inquiry_plug_signal_format_in_0` | INQUIRY | `02 FF 19 00 90 02 FF FF` | STABLE (0x0C) | 6754 | OK |
| `inquiry_plug_signal_format_in_1` | INQUIRY | `02 FF 19 01 90 01 FF FF` | STABLE (0x0C) | 6346 | OK |
| `inquiry_plug_signal_format_out_0` | INQUIRY | `02 FF 18 00 90 02 FF FF` | STABLE (0x0C) | 6761 | OK |
| `inquiry_plug_signal_format_out_1` | INQUIRY | `02 FF 18 01 90 01 FF FF` | STABLE (0x0C) | 6878 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 FF 2F C0 00 00 00 00 FF 00 90 40 04 01 03 08 06 02 00 01 0D` | STABLE (0x0C) | 23411 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 FF 2F C0 00 00 00 01 FF 00 90 00 40 FF 3F FF` | STABLE (0x0C) | 6122 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 FF 2F C0 01 00 00 00 FF 00 90 40 04 01 06 02 06 02 06 02 06 02 06 02 06 01 0D` | STABLE (0x0C) | 28755 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 FF 2F C0 01 00 00 01 FF 00 90 00 40 FF 3F FF` | STABLE (0x0C) | 6013 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 00 FF FF 00 90 40 04 01 03 08 06 02 00 01 0D` | STABLE (0x0C) | 22786 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 01 FF FF 00 90 40 03 01 01 02 06` | STABLE (0x0C) | 12032 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 02 FF FF 00 90 40 03 01 01 02 06` | STABLE (0x0C) | 12455 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 03 FF FF 00 90 40 03 01 01 02 06` | STABLE (0x0C) | 12230 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 04 FF FF 00 90 40 03 01 01 02 06` | STABLE (0x0C) | 12153 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 05 FF FF 00 90 40 03 01 01 02 06` | STABLE (0x0C) | 11819 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 06 FF FF 00 90 40 03 01 01 02 0D` | STABLE (0x0C) | 12394 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 07 FF FF 00 90 40 03 01 01 02 0D` | STABLE (0x0C) | 12081 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 08 FF FF 00 90 00 40 FF 3F FF` | STABLE (0x0C) | 6462 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 09 FF FF 00 90 00 40 FF 3F FF` | STABLE (0x0C) | 6174 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 00 FF FF 00 90 40 03 01 06 02 06 02 06 02 06 02 06 02 06 01 0D` | STABLE (0x0C) | 28874 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 01 FF FF 00 90 40 03 01 01 08 06` | STABLE (0x0C) | 17467 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 02 FF FF 00 90 40 03 01 01 02 00` | STABLE (0x0C) | 12028 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 03 FF FF 00 FF FF FF FF FF FF` | NOT IMPLEMENTED (0x08) | 6605 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 04 FF FF 00 FF FF FF FF FF FF` | NOT IMPLEMENTED (0x08) | 6232 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 05 FF FF 00 90 00 40 FF 3F FF` | STABLE (0x0C) | 6154 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF FF 00 60 00` | STABLE (0x0C) | 6855 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F FF 00 60 00` | STABLE (0x0C) | 6946 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF 08 00 60 01` | STABLE (0x0C) | 6160 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F 08 00 60 01` | STABLE (0x0C) | 6448 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF 08 01 60 02` | STABLE (0x0C) | 6189 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F 08 01 60 02` | STABLE (0x0C) | 6768 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF 08 02 60 03` | STABLE (0x0C) | 6142 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F 08 02 60 03` | STABLE (0x0C) | 6633 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF 08 03 60 04` | STABLE (0x0C) | 6589 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F 08 03 60 04` | STABLE (0x0C) | 6161 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF 08 04 60 05` | STABLE (0x0C) | 6238 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F 08 04 60 05` | STABLE (0x0C) | 6090 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF FF 85 60 06` | STABLE (0x0C) | 6121 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F FF 85 60 06` | STABLE (0x0C) | 6121 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF FF 86 60 07` | STABLE (0x0C) | 6093 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F FF 86 60 07` | STABLE (0x0C) | 6976 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF FF FE 60 08` | NOT IMPLEMENTED (0x08) | 6800 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F FF FE 60 08` | NOT IMPLEMENTED (0x08) | 6779 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF FF FE 60 09` | NOT IMPLEMENTED (0x08) | 6762 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F FF FE 60 09` | NOT IMPLEMENTED (0x08) | 6061 | OK |
| `inquiry_function_block_selector_fb_1` | INQUIRY | `02 08 B8 80 01 10 02 00 01` | NOT IMPLEMENTED (0x08) | 6487 | OK |
| `inquiry_function_block_selector_fb_2` | INQUIRY | `02 08 B8 80 02 10 02 00 01` | NOT IMPLEMENTED (0x08) | 6509 | OK |
| `inquiry_function_block_selector_fb_3` | INQUIRY | `02 08 B8 80 03 10 02 00 01` | NOT IMPLEMENTED (0x08) | 7038 | OK |
| `inquiry_function_block_selector_fb_4` | INQUIRY | `02 08 B8 80 04 10 02 00 01` | NOT IMPLEMENTED (0x08) | 11895 | OK |
| `inquiry_function_block_selector_fb_5` | INQUIRY | `02 08 B8 80 05 10 02 00 01` | NOT IMPLEMENTED (0x08) | 6554 | OK |
| `inquiry_function_block_selector_fb_6` | INQUIRY | `02 08 B8 80 06 10 02 01 01` | NOT IMPLEMENTED (0x08) | 6129 | OK |
| `inquiry_function_block_selector_fb_7` | INQUIRY | `02 08 B8 80 07 10 02 01 01` | NOT IMPLEMENTED (0x08) | 6484 | OK |
| `inquiry_function_block_selector_fb_8` | INQUIRY | `02 08 B8 80 08 10 02 01 01` | NOT IMPLEMENTED (0x08) | 6858 | OK |
| `inquiry_function_block_selector_fb_9` | INQUIRY | `02 08 B8 80 09 10 02 00 01` | NOT IMPLEMENTED (0x08) | 6072 | OK |
| `inquiry_function_block_selector_fb_10` | INQUIRY | `02 08 B8 80 0A 10 02 01 01` | NOT IMPLEMENTED (0x08) | 11945 | OK |
| `inquiry_feature_mute_on_fb_1_ch_0` | INQUIRY | `02 08 B8 81 01 10 02 00 01 01 70` | NOT IMPLEMENTED (0x08) | 6631 | OK |
| `inquiry_feature_mute_off_fb_1_ch_0` | INQUIRY | `02 08 B8 81 01 10 02 00 01 01 60` | NOT IMPLEMENTED (0x08) | 6583 | OK |
| `inquiry_feature_volume_fb_1_ch_0` | INQUIRY | `02 08 B8 81 01 10 02 00 02 02 02 00` | NOT IMPLEMENTED (0x08) | 6790 | OK |
| `inquiry_feature_mute_on_fb_2_ch_0` | INQUIRY | `02 08 B8 81 02 10 02 00 01 01 70` | NOT IMPLEMENTED (0x08) | 12583 | OK |
| `inquiry_feature_mute_off_fb_2_ch_0` | INQUIRY | `02 08 B8 81 02 10 02 00 01 01 60` | NOT IMPLEMENTED (0x08) | 6176 | OK |
| `inquiry_feature_volume_fb_2_ch_0` | INQUIRY | `02 08 B8 81 02 10 02 00 02 02 02 00` | NOT IMPLEMENTED (0x08) | 6386 | OK |
| `inquiry_feature_mute_on_fb_3_ch_0` | INQUIRY | `02 08 B8 81 03 10 02 00 01 01 70` | NOT IMPLEMENTED (0x08) | 6656 | OK |
| `inquiry_feature_mute_off_fb_3_ch_0` | INQUIRY | `02 08 B8 81 03 10 02 00 01 01 60` | NOT IMPLEMENTED (0x08) | 6432 | OK |
| `inquiry_feature_volume_fb_3_ch_0` | INQUIRY | `02 08 B8 81 03 10 02 00 02 02 02 00` | NOT IMPLEMENTED (0x08) | 6287 | OK |
| `inquiry_feature_mute_on_fb_4_ch_0` | INQUIRY | `02 08 B8 81 04 10 02 00 01 01 70` | NOT IMPLEMENTED (0x08) | 11844 | OK |
| `inquiry_feature_mute_off_fb_4_ch_0` | INQUIRY | `02 08 B8 81 04 10 02 00 01 01 60` | NOT IMPLEMENTED (0x08) | 6021 | OK |
| `inquiry_feature_volume_fb_4_ch_0` | INQUIRY | `02 08 B8 81 04 10 02 00 02 02 02 00` | NOT IMPLEMENTED (0x08) | 6550 | OK |
| `inquiry_feature_mute_on_fb_5_ch_0` | INQUIRY | `02 08 B8 81 05 10 02 00 01 01 70` | NOT IMPLEMENTED (0x08) | 6744 | OK |
| `inquiry_feature_mute_off_fb_5_ch_0` | INQUIRY | `02 08 B8 81 05 10 02 00 01 01 60` | NOT IMPLEMENTED (0x08) | 6492 | OK |
| `inquiry_feature_volume_fb_5_ch_0` | INQUIRY | `02 08 B8 81 05 10 02 00 02 02 02 00` | NOT IMPLEMENTED (0x08) | 5970 | OK |
| `inquiry_feature_mute_on_fb_6_ch_0` | INQUIRY | `02 08 B8 81 06 10 02 00 01 01 70` | NOT IMPLEMENTED (0x08) | 6023 | OK |
| `inquiry_feature_mute_off_fb_6_ch_0` | INQUIRY | `02 08 B8 81 06 10 02 00 01 01 60` | NOT IMPLEMENTED (0x08) | 6783 | OK |
| `inquiry_feature_volume_fb_6_ch_0` | INQUIRY | `02 08 B8 81 06 10 02 00 02 02 02 00` | NOT IMPLEMENTED (0x08) | 6806 | OK |
| `inquiry_feature_mute_on_fb_7_ch_0` | INQUIRY | `02 08 B8 81 07 10 02 00 01 01 70` | NOT IMPLEMENTED (0x08) | 6632 | OK |
| `inquiry_feature_mute_off_fb_7_ch_0` | INQUIRY | `02 08 B8 81 07 10 02 00 01 01 60` | NOT IMPLEMENTED (0x08) | 6649 | OK |
| `inquiry_feature_volume_fb_7_ch_0` | INQUIRY | `02 08 B8 81 07 10 02 00 02 02 02 00` | NOT IMPLEMENTED (0x08) | 6049 | OK |
| `post_check_plug_info` | STATUS | `01 FF 02 00 FF FF FF FF` | STABLE (0x0C) | 6497 | OK |
