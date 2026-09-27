# AV/C Hardware Probe Matrix: Apogee Electronics Duet

- **Key**: `duet`
- **GUID**: `0x0003DB0A0000D112`
- **Node ID**: `0` (generation 3)
- **Driver Version**: `0.3.1`
- **Captured At**: `2026-09-27T19:56:06.720435+00:00`
- **Total Exchanges**: `221`

| Exchange | Intent | Command (Hex) | Response Code | Duration (µs) | Result |
|---|---|---|---|---|---|
| `unit_info` | STATUS | `01 FF 30 07 FF FF FF FF` | STABLE (0x0C) | 6484 | OK |
| `subunit_info_page_0` | STATUS | `01 FF 31 07 FF FF FF FF` | STABLE (0x0C) | 6072 | OK |
| `subunit_info_page_1` | STATUS | `01 FF 31 17 FF FF FF FF` | NOT IMPLEMENTED (0x08) | 6547 | OK |
| `plug_info_unit_00` | STATUS | `01 FF 02 00 FF FF FF FF` | STABLE (0x0C) | 6790 | OK |
| `plug_info_unit_01` | STATUS | `01 FF 02 01 FF FF FF FF` | STABLE (0x0C) | 6618 | OK |
| `plug_info_audio_0` | STATUS | `01 08 02 00 FF FF FF FF` | STABLE (0x0C) | 6850 | OK |
| `plug_info_music_0` | STATUS | `01 60 02 00 FF FF FF FF` | STABLE (0x0C) | 6150 | OK |
| `plug_signal_format_in_0_all_wildcard` | STATUS | `01 FF 19 00 FF FF FF FF` | STABLE (0x0C) | 6626 | OK |
| `plug_signal_format_in_0_am824_wildcard` | STATUS | `01 FF 19 00 90 FF FF FF` | STABLE (0x0C) | 6155 | OK |
| `plug_signal_format_out_0_all_wildcard` | STATUS | `01 FF 18 00 FF FF FF FF` | STABLE (0x0C) | 6090 | OK |
| `plug_signal_format_out_0_am824_wildcard` | STATUS | `01 FF 18 00 90 FF FF FF` | STABLE (0x0C) | 6713 | OK |
| `stream_format_0x2F_single_unit_iso_in_0` | STATUS | `01 FF 2F C0 00 00 00 00 FF FF` | STABLE (0x0C) | 6867 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_0` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 00` | STABLE (0x0C) | 6134 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_1` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 01` | STABLE (0x0C) | 6644 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_2` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 02` | STABLE (0x0C) | 6626 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_3` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 03` | STABLE (0x0C) | 6808 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_4` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 04` | REJECTED (0x0A) | 6279 | OK |
| `stream_format_0xBF_single_unit_iso_in_0` | STATUS | `01 FF BF C0 00 00 00 00 FF FF` | STABLE (0x0C) | 6217 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_0` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 00` | STABLE (0x0C) | 6314 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_1` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 01` | STABLE (0x0C) | 6317 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_2` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 02` | STABLE (0x0C) | 6638 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_3` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 03` | STABLE (0x0C) | 6708 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_4` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 04` | REJECTED (0x0A) | 6152 | OK |
| `stream_format_0x2F_single_unit_iso_out_0` | STATUS | `01 FF 2F C0 01 00 00 00 FF FF` | STABLE (0x0C) | 6219 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_0` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 00` | STABLE (0x0C) | 6068 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_1` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 01` | STABLE (0x0C) | 6231 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_2` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 02` | STABLE (0x0C) | 6695 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_3` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 03` | STABLE (0x0C) | 6145 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_4` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 04` | REJECTED (0x0A) | 6296 | OK |
| `stream_format_0xBF_single_unit_iso_out_0` | STATUS | `01 FF BF C0 01 00 00 00 FF FF` | STABLE (0x0C) | 5957 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_0` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 00` | STABLE (0x0C) | 6461 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_1` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 01` | STABLE (0x0C) | 6103 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_2` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 02` | STABLE (0x0C) | 6069 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_3` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 03` | STABLE (0x0C) | 6585 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_4` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 04` | REJECTED (0x0A) | 6190 | OK |
| `stream_format_0x2F_single_music_0_dest_0` | STATUS | `01 60 2F C0 00 01 00 FF FF FF` | STABLE (0x0C) | 6576 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_0` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 00` | STABLE (0x0C) | 6236 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_1` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 01` | STABLE (0x0C) | 6069 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_2` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 02` | STABLE (0x0C) | 6564 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_3` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 03` | STABLE (0x0C) | 6047 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_4` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 04` | REJECTED (0x0A) | 6422 | OK |
| `stream_format_0xBF_single_music_0_dest_0` | STATUS | `01 60 BF C0 00 01 00 FF FF FF` | STABLE (0x0C) | 6391 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_0` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 00` | STABLE (0x0C) | 6845 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_1` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 01` | STABLE (0x0C) | 5988 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_2` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 02` | STABLE (0x0C) | 6438 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_3` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 03` | STABLE (0x0C) | 6312 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_4` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 04` | REJECTED (0x0A) | 6837 | OK |
| `stream_format_0x2F_single_music_0_dest_1` | STATUS | `01 60 2F C0 00 01 01 FF FF FF` | STABLE (0x0C) | 5489 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_0` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 00` | STABLE (0x0C) | 5980 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_1` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 01` | STABLE (0x0C) | 6155 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_2` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 02` | STABLE (0x0C) | 6281 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_3` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 03` | STABLE (0x0C) | 6168 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_4` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 04` | REJECTED (0x0A) | 6421 | OK |
| `stream_format_0xBF_single_music_0_dest_1` | STATUS | `01 60 BF C0 00 01 01 FF FF FF` | STABLE (0x0C) | 6106 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_0` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 00` | STABLE (0x0C) | 6497 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_1` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 01` | STABLE (0x0C) | 6106 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_2` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 02` | STABLE (0x0C) | 6168 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_3` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 03` | STABLE (0x0C) | 6220 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_4` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 04` | REJECTED (0x0A) | 6078 | OK |
| `stream_format_0x2F_single_music_0_dest_2` | STATUS | `01 60 2F C0 00 01 02 FF FF FF` | STABLE (0x0C) | 6505 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_0` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 00` | STABLE (0x0C) | 7109 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_1` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 01` | STABLE (0x0C) | 6442 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_2` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 02` | STABLE (0x0C) | 6108 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_3` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 03` | STABLE (0x0C) | 6095 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_4` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 04` | REJECTED (0x0A) | 6577 | OK |
| `stream_format_0xBF_single_music_0_dest_2` | STATUS | `01 60 BF C0 00 01 02 FF FF FF` | STABLE (0x0C) | 6458 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_0` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 00` | STABLE (0x0C) | 6746 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_1` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 01` | STABLE (0x0C) | 6494 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_2` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 02` | STABLE (0x0C) | 6023 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_3` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 03` | STABLE (0x0C) | 6241 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_4` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 04` | REJECTED (0x0A) | 6011 | OK |
| `stream_format_0x2F_single_music_0_src_0` | STATUS | `01 60 2F C0 01 01 00 FF FF FF` | STABLE (0x0C) | 6116 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_0` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 00` | STABLE (0x0C) | 6253 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_1` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 01` | STABLE (0x0C) | 6040 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_2` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 02` | STABLE (0x0C) | 6554 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_3` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 03` | STABLE (0x0C) | 6744 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_4` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 04` | REJECTED (0x0A) | 6455 | OK |
| `stream_format_0xBF_single_music_0_src_0` | STATUS | `01 60 BF C0 01 01 00 FF FF FF` | STABLE (0x0C) | 6047 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_0` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 00` | STABLE (0x0C) | 6095 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_1` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 01` | STABLE (0x0C) | 6604 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_2` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 02` | STABLE (0x0C) | 6870 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_3` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 03` | STABLE (0x0C) | 6229 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_4` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 04` | REJECTED (0x0A) | 6574 | OK |
| `stream_format_0x2F_single_music_0_src_1` | STATUS | `01 60 2F C0 01 01 01 FF FF FF` | STABLE (0x0C) | 6099 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_0` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 00` | STABLE (0x0C) | 6904 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_1` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 01` | STABLE (0x0C) | 6180 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_2` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 02` | STABLE (0x0C) | 5967 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_3` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 03` | STABLE (0x0C) | 6775 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_4` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 04` | REJECTED (0x0A) | 5998 | OK |
| `stream_format_0xBF_single_music_0_src_1` | STATUS | `01 60 BF C0 01 01 01 FF FF FF` | STABLE (0x0C) | 6598 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_0` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 00` | STABLE (0x0C) | 6549 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_1` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 01` | STABLE (0x0C) | 6531 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_2` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 02` | STABLE (0x0C) | 6126 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_3` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 03` | STABLE (0x0C) | 6018 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_4` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 04` | REJECTED (0x0A) | 7070 | OK |
| `stream_format_0x2F_single_music_0_src_2` | STATUS | `01 60 2F C0 01 01 02 FF FF FF` | STABLE (0x0C) | 6371 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_0` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 00` | STABLE (0x0C) | 6350 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_1` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 01` | STABLE (0x0C) | 6914 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_2` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 02` | STABLE (0x0C) | 5954 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_3` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 03` | STABLE (0x0C) | 5917 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_4` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 04` | REJECTED (0x0A) | 6399 | OK |
| `stream_format_0xBF_single_music_0_src_2` | STATUS | `01 60 BF C0 01 01 02 FF FF FF` | STABLE (0x0C) | 6576 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_0` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 00` | STABLE (0x0C) | 5743 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_1` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 01` | STABLE (0x0C) | 6410 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_2` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 02` | STABLE (0x0C) | 6098 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_3` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 03` | STABLE (0x0C) | 6456 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_4` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 04` | REJECTED (0x0A) | 6657 | OK |
| `signal_source_0xFF_music_0_dest_0` | STATUS | `01 FF 1A FF FF FE 60 00` | STABLE (0x0C) | 6053 | OK |
| `signal_source_0x0F_music_0_dest_0` | STATUS | `01 FF 1A 0F FF FE 60 00` | STABLE (0x0C) | 6003 | OK |
| `signal_source_0xFF_music_0_dest_1` | STATUS | `01 FF 1A FF FF FE 60 01` | STABLE (0x0C) | 6634 | OK |
| `signal_source_0x0F_music_0_dest_1` | STATUS | `01 FF 1A 0F FF FE 60 01` | STABLE (0x0C) | 6551 | OK |
| `signal_source_0xFF_music_0_dest_2` | STATUS | `01 FF 1A FF FF FE 60 02` | STABLE (0x0C) | 6283 | OK |
| `signal_source_0x0F_music_0_dest_2` | STATUS | `01 FF 1A 0F FF FE 60 02` | STABLE (0x0C) | 5962 | OK |
| `bridgeco_plug_info_plug_type_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 00` | NOT IMPLEMENTED (0x08) | 6044 | OK |
| `bridgeco_plug_info_plug_name_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 01` | NOT IMPLEMENTED (0x08) | 5942 | OK |
| `bridgeco_plug_info_channel_count_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 02` | NOT IMPLEMENTED (0x08) | 6718 | OK |
| `bridgeco_plug_info_channel_positions_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 03` | NOT IMPLEMENTED (0x08) | 6100 | OK |
| `bridgeco_plug_info_channel_name_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 04` | NOT IMPLEMENTED (0x08) | 6453 | OK |
| `bridgeco_plug_info_plug_input_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 05` | NOT IMPLEMENTED (0x08) | 6063 | OK |
| `bridgeco_plug_info_plug_outputs_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 06` | NOT IMPLEMENTED (0x08) | 6416 | OK |
| `bridgeco_plug_info_plug_type_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 00` | NOT IMPLEMENTED (0x08) | 6880 | OK |
| `bridgeco_plug_info_plug_name_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 01` | NOT IMPLEMENTED (0x08) | 6541 | OK |
| `bridgeco_plug_info_channel_count_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 02` | NOT IMPLEMENTED (0x08) | 6301 | OK |
| `bridgeco_plug_info_channel_positions_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 03` | NOT IMPLEMENTED (0x08) | 5527 | OK |
| `bridgeco_plug_info_channel_name_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 04` | NOT IMPLEMENTED (0x08) | 6397 | OK |
| `bridgeco_plug_info_plug_input_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 05` | NOT IMPLEMENTED (0x08) | 6276 | OK |
| `bridgeco_plug_info_plug_outputs_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 06` | NOT IMPLEMENTED (0x08) | 6454 | OK |
| `function_block_selector_status_fb_0` | STATUS | `01 08 B8 80 00 10 02 FF 01` | NOT IMPLEMENTED (0x08) | 6788 | OK |
| `function_block_selector_status_fb_1` | STATUS | `01 08 B8 80 01 10 02 FF 01` | NOT IMPLEMENTED (0x08) | 6013 | OK |
| `function_block_feature_mute_status_fb_0` | STATUS | `01 08 B8 81 00 10 02 00 01 01 FF` | NOT IMPLEMENTED (0x08) | 6373 | OK |
| `function_block_feature_volume_current_fb_0` | STATUS | `01 08 B8 81 00 10 02 00 02 02 FF FF` | NOT IMPLEMENTED (0x08) | 6585 | OK |
| `function_block_feature_mute_status_fb_1` | STATUS | `01 08 B8 81 01 10 02 00 01 01 FF` | STABLE (0x0C) | 6606 | OK |
| `function_block_feature_volume_current_fb_1` | STATUS | `01 08 B8 81 01 10 02 00 02 02 FF FF` | STABLE (0x0C) | 6270 | OK |
| `function_block_feature_volume_min_fb_1` | STATUS | `01 08 B8 81 01 02 02 00 02 02 FF FF` | STABLE (0x0C) | 6211 | OK |
| `function_block_feature_volume_max_fb_1` | STATUS | `01 08 B8 81 01 03 02 00 02 02 FF FF` | STABLE (0x0C) | 6144 | OK |
| `function_block_feature_mute_status_fb_2` | STATUS | `01 08 B8 81 02 10 02 00 01 01 FF` | NOT IMPLEMENTED (0x08) | 6152 | OK |
| `function_block_feature_volume_current_fb_2` | STATUS | `01 08 B8 81 02 10 02 00 02 02 FF FF` | NOT IMPLEMENTED (0x08) | 6420 | OK |
| `inquiry_plug_signal_format_in_0` | INQUIRY | `02 FF 19 00 90 02 FF FF` | STABLE (0x0C) | 6768 | OK |
| `inquiry_plug_signal_format_out_0` | INQUIRY | `02 FF 18 00 90 02 FF FF` | STABLE (0x0C) | 6003 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 FF 2F C0 00 00 00 00 FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6036 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 FF BF C0 00 00 00 00 FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 5860 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 FF 2F C0 01 00 00 00 FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6259 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 FF BF C0 01 00 00 00 FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6691 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 00 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6603 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 00 01 00 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6168 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 01 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6484 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 00 01 01 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6462 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 02 FF FF 00 90 00 40` | REJECTED (0x0A) | 6960 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 00 01 02 FF FF 00 90 00 40` | REJECTED (0x0A) | 6060 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 00 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6343 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 01 01 00 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6099 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 01 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6535 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 01 01 01 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6966 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 02 FF FF 00 90 00 40` | REJECTED (0x0A) | 5985 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 01 01 02 FF FF 00 90 00 40` | REJECTED (0x0A) | 6033 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF FF 00 60 00` | NOT IMPLEMENTED (0x08) | 6512 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F FF 00 60 00` | NOT IMPLEMENTED (0x08) | 6566 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF 08 01 60 01` | NOT IMPLEMENTED (0x08) | 6344 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F 08 01 60 01` | NOT IMPLEMENTED (0x08) | 6661 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF 60 02 60 02` | NOT IMPLEMENTED (0x08) | 6016 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F 60 02 60 02` | NOT IMPLEMENTED (0x08) | 6414 | OK |
| `inquiry_feature_mute_on_fb_1_ch_0` | INQUIRY | `02 08 B8 81 01 10 02 00 01 01 70` | STABLE (0x0C) | 6628 | OK |
| `inquiry_feature_mute_off_fb_1_ch_0` | INQUIRY | `02 08 B8 81 01 10 02 00 01 01 60` | STABLE (0x0C) | 6543 | OK |
| `inquiry_feature_volume_fb_1_ch_0` | INQUIRY | `02 08 B8 81 01 10 02 00 02 02 02 00` | STABLE (0x0C) | 5676 | OK |
| `oxford_firmware_id` | CSR_READ | `F0 05 00 00` | STABLE (0x0C) | 42248 | OK |
| `oxford_hardware_id` | CSR_READ | `F0 09 00 20` | STABLE (0x0C) | 316 | OK |
| `apogee_dsp_input_meters` | CSR_READ | `F0 08 00 04 08` | STABLE (0x0C) | 41254 | OK |
| `apogee_dsp_mixer_meters` | CSR_READ | `F0 08 04 04 10` | STABLE (0x0C) | 39960 | OK |
| `apogee_mic_polarity_ch0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 00 80 00` | STABLE (0x0C) | 5887 | OK |
| `apogee_mic_polarity_ch1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 00 80 01` | STABLE (0x0C) | 6211 | OK |
| `apogee_xlr_is_mic_level_ch0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 01 80 00` | STABLE (0x0C) | 6474 | OK |
| `apogee_xlr_is_mic_level_ch1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 01 80 01` | STABLE (0x0C) | 5915 | OK |
| `apogee_xlr_is_consumer_ch0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 02 80 00` | STABLE (0x0C) | 6188 | OK |
| `apogee_xlr_is_consumer_ch1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 02 80 01` | STABLE (0x0C) | 6425 | OK |
| `apogee_mic_phantom_ch0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 03 80 00` | STABLE (0x0C) | 6675 | OK |
| `apogee_mic_phantom_ch1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 03 80 01` | STABLE (0x0C) | 6371 | OK |
| `apogee_out_is_consumer` | STATUS | `01 FF 00 00 03 DB 50 43 4D 04 80 FF` | STABLE (0x0C) | 6173 | OK |
| `apogee_in_gain_ch0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 05 80 00` | STABLE (0x0C) | 6026 | OK |
| `apogee_in_gain_ch1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 05 80 01` | STABLE (0x0C) | 5967 | OK |
| `apogee_controller_instances` | STATUS | `01 FF 00 00 03 DB 50 43 4D 06 80 FF` | STABLE (0x0C) | 6602 | OK |
| `apogee_hw_state` | STATUS | `01 FF 00 00 03 DB 50 43 4D 07 FF FF` | STABLE (0x0C) | 6574 | OK |
| `apogee_mics_grouped` | STATUS | `01 FF 00 00 03 DB 50 43 4D 08 80 00` | STABLE (0x0C) | 6074 | OK |
| `apogee_out_mute` | STATUS | `01 FF 00 00 03 DB 50 43 4D 09 80 FF` | STABLE (0x0C) | 6072 | OK |
| `apogee_firmware_version` | STATUS | `01 FF 00 00 03 DB 50 43 4D 0A 80 FF` | STABLE (0x0C) | 6119 | OK |
| `apogee_in_src_is_phone_ch0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 0C 80 00` | STABLE (0x0C) | 6806 | OK |
| `apogee_in_src_is_phone_ch1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 0C 80 01` | STABLE (0x0C) | 6807 | OK |
| `apogee_out_src_is_mixer` | STATUS | `01 FF 00 00 03 DB 50 43 4D 11 FF FF` | STABLE (0x0C) | 6300 | OK |
| `apogee_identify` | STATUS | `01 FF 00 00 03 DB 50 43 4D 12 80 FF` | STABLE (0x0C) | 6142 | OK |
| `apogee_disp_overhold_2s` | STATUS | `01 FF 00 00 03 DB 50 43 4D 13 FF FF` | STABLE (0x0C) | 6084 | OK |
| `apogee_out_volume` | STATUS | `01 FF 00 00 03 DB 50 43 4D 15 80 FF` | STABLE (0x0C) | 6459 | OK |
| `apogee_mute_line_out` | STATUS | `01 FF 00 00 03 DB 50 43 4D 16 80 FF` | STABLE (0x0C) | 6307 | OK |
| `apogee_mute_hp_out` | STATUS | `01 FF 00 00 03 DB 50 43 4D 17 80 FF` | STABLE (0x0C) | 6873 | OK |
| `apogee_unmute_line_out` | STATUS | `01 FF 00 00 03 DB 50 43 4D 18 80 FF` | STABLE (0x0C) | 6639 | OK |
| `apogee_unmute_hp_out` | STATUS | `01 FF 00 00 03 DB 50 43 4D 19 80 FF` | STABLE (0x0C) | 5977 | OK |
| `apogee_disp_is_input` | STATUS | `01 FF 00 00 03 DB 50 43 4D 1B FF FF` | STABLE (0x0C) | 6591 | OK |
| `apogee_limited_gain_range` | STATUS | `01 FF 00 00 03 DB 50 43 4D 1E FF FF` | STABLE (0x0C) | 5926 | OK |
| `apogee_nudge_count` | STATUS | `01 FF 00 00 03 DB 50 43 4D 20 80 FF` | STABLE (0x0C) | 6824 | OK |
| `apogee_disp_follow_knob` | STATUS | `01 FF 00 00 03 DB 50 43 4D 22 FF FF` | STABLE (0x0C) | 6191 | OK |
| `apogee_inputs_muted` | STATUS | `01 FF 00 00 03 DB 50 43 4D 23 80 FF` | STABLE (0x0C) | 5892 | OK |
| `apogee_select_encoder_control` | STATUS | `01 FF 00 00 03 DB 50 43 4D 25 80 80` | NOT IMPLEMENTED (0x08) | 5995 | OK |
| `apogee_mixer_src_0_dst_0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 00 00` | STABLE (0x0C) | 6311 | OK |
| `apogee_mixer_src_0_dst_1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 00 01` | STABLE (0x0C) | 6785 | OK |
| `apogee_mixer_src_1_dst_0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 01 00` | STABLE (0x0C) | 7028 | OK |
| `apogee_mixer_src_1_dst_1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 01 01` | STABLE (0x0C) | 5562 | OK |
| `apogee_mixer_src_2_dst_0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 10 00` | STABLE (0x0C) | 5653 | OK |
| `apogee_mixer_src_2_dst_1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 10 01` | STABLE (0x0C) | 6644 | OK |
| `apogee_mixer_src_3_dst_0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 11 00` | STABLE (0x0C) | 6330 | OK |
| `apogee_mixer_src_3_dst_1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 11 01` | STABLE (0x0C) | 6257 | OK |
| `inquiry_apogee_mic_phantom_ch0_on` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 03 80 00 70` | ACCEPTED (0x09) | 6386 | OK |
| `inquiry_apogee_mic_phantom_ch0_off` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 03 80 00 60` | ACCEPTED (0x09) | 6219 | OK |
| `inquiry_apogee_in_gain_ch0_30db` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 05 80 00 1E` | ACCEPTED (0x09) | 6564 | OK |
| `inquiry_apogee_mics_grouped_on` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 08 80 00 70` | ACCEPTED (0x09) | 6515 | OK |
| `inquiry_apogee_mics_grouped_off` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 08 80 00 60` | ACCEPTED (0x09) | 6513 | OK |
| `inquiry_apogee_out_mute_on` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 09 80 FF 70` | ACCEPTED (0x09) | 6134 | OK |
| `inquiry_apogee_out_mute_off` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 09 80 FF 60` | ACCEPTED (0x09) | 6416 | OK |
| `inquiry_apogee_identify_on` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 12 80 FF 70` | ACCEPTED (0x09) | 6139 | OK |
| `inquiry_apogee_out_volume_40` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 15 80 FF 28` | ACCEPTED (0x09) | 6500 | OK |
| `inquiry_apogee_disp_follow_knob_on` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 22 FF FF 70` | ACCEPTED (0x09) | 6363 | OK |
| `inquiry_apogee_inputs_muted_on` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 23 80 FF 70` | ACCEPTED (0x09) | 6224 | OK |
| `inquiry_apogee_inputs_muted_off` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 23 80 FF 60` | ACCEPTED (0x09) | 6150 | OK |
| `post_check_plug_info` | STATUS | `01 FF 02 00 FF FF FF FF` | STABLE (0x0C) | 6392 | OK |
