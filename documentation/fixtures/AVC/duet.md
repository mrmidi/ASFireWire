# AV/C Hardware Probe Matrix: Apogee Electronics Duet

- **Key**: `duet`
- **GUID**: `0x0003DB0A0000D112`
- **Node ID**: `0` (generation 3)
- **Driver Version**: `0.3.1`
- **Captured At**: `2026-09-27T19:28:32.823566+00:00`
- **Total Exchanges**: `165`

| Exchange | Intent | Command (Hex) | Response Code | Duration (µs) | Result |
|---|---|---|---|---|---|
| `unit_info` | STATUS | `01 FF 30 07 FF FF FF FF` | STABLE (0x0C) | 6329 | OK |
| `subunit_info_page_0` | STATUS | `01 FF 31 07 FF FF FF FF` | STABLE (0x0C) | 6285 | OK |
| `subunit_info_page_1` | STATUS | `01 FF 31 17 FF FF FF FF` | NOT IMPLEMENTED (0x08) | 6494 | OK |
| `plug_info_unit_00` | STATUS | `01 FF 02 00 FF FF FF FF` | STABLE (0x0C) | 5915 | OK |
| `plug_info_unit_01` | STATUS | `01 FF 02 01 FF FF FF FF` | STABLE (0x0C) | 6642 | OK |
| `plug_info_audio_0` | STATUS | `01 08 02 00 FF FF FF FF` | STABLE (0x0C) | 6335 | OK |
| `plug_info_music_0` | STATUS | `01 60 02 00 FF FF FF FF` | STABLE (0x0C) | 5937 | OK |
| `plug_signal_format_in_0_all_wildcard` | STATUS | `01 FF 19 00 FF FF FF FF` | STABLE (0x0C) | 6173 | OK |
| `plug_signal_format_in_0_am824_wildcard` | STATUS | `01 FF 19 00 90 FF FF FF` | STABLE (0x0C) | 6155 | OK |
| `plug_signal_format_out_0_all_wildcard` | STATUS | `01 FF 18 00 FF FF FF FF` | STABLE (0x0C) | 6399 | OK |
| `plug_signal_format_out_0_am824_wildcard` | STATUS | `01 FF 18 00 90 FF FF FF` | STABLE (0x0C) | 6629 | OK |
| `stream_format_0x2F_single_unit_iso_in_0` | STATUS | `01 FF 2F C0 00 00 00 00 FF FF` | STABLE (0x0C) | 6101 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_0` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 00` | STABLE (0x0C) | 6484 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_1` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 01` | STABLE (0x0C) | 6153 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_2` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 02` | STABLE (0x0C) | 6819 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_3` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 03` | STABLE (0x0C) | 6175 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_4` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 04` | REJECTED (0x0A) | 6192 | OK |
| `stream_format_0xBF_single_unit_iso_in_0` | STATUS | `01 FF BF C0 00 00 00 00 FF FF` | STABLE (0x0C) | 6659 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_0` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 00` | STABLE (0x0C) | 6567 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_1` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 01` | STABLE (0x0C) | 6222 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_2` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 02` | STABLE (0x0C) | 6168 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_3` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 03` | STABLE (0x0C) | 6062 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_4` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 04` | REJECTED (0x0A) | 6469 | OK |
| `stream_format_0x2F_single_unit_iso_out_0` | STATUS | `01 FF 2F C0 01 00 00 00 FF FF` | STABLE (0x0C) | 6847 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_0` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 00` | STABLE (0x0C) | 6572 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_1` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 01` | STABLE (0x0C) | 6101 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_2` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 02` | STABLE (0x0C) | 6422 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_3` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 03` | STABLE (0x0C) | 6142 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_4` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 04` | REJECTED (0x0A) | 6414 | OK |
| `stream_format_0xBF_single_unit_iso_out_0` | STATUS | `01 FF BF C0 01 00 00 00 FF FF` | STABLE (0x0C) | 6711 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_0` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 00` | STABLE (0x0C) | 6702 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_1` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 01` | STABLE (0x0C) | 6000 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_2` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 02` | STABLE (0x0C) | 6052 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_3` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 03` | STABLE (0x0C) | 6603 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_4` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 04` | REJECTED (0x0A) | 6109 | OK |
| `stream_format_0x2F_single_music_0_dest_0` | STATUS | `01 60 2F C0 00 01 00 FF FF FF` | STABLE (0x0C) | 6764 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_0` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 00` | STABLE (0x0C) | 6675 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_1` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 01` | STABLE (0x0C) | 5551 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_2` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 02` | STABLE (0x0C) | 6325 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_3` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 03` | STABLE (0x0C) | 6641 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_4` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 04` | REJECTED (0x0A) | 6813 | OK |
| `stream_format_0xBF_single_music_0_dest_0` | STATUS | `01 60 BF C0 00 01 00 FF FF FF` | STABLE (0x0C) | 6126 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_0` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 00` | STABLE (0x0C) | 6623 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_1` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 01` | STABLE (0x0C) | 6157 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_2` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 02` | STABLE (0x0C) | 6631 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_3` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 03` | STABLE (0x0C) | 6021 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_4` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 04` | REJECTED (0x0A) | 5580 | OK |
| `stream_format_0x2F_single_music_0_dest_1` | STATUS | `01 60 2F C0 00 01 01 FF FF FF` | STABLE (0x0C) | 6489 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_0` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 00` | STABLE (0x0C) | 6615 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_1` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 01` | STABLE (0x0C) | 6593 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_2` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 02` | STABLE (0x0C) | 6698 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_3` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 03` | STABLE (0x0C) | 6072 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_4` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 04` | REJECTED (0x0A) | 6164 | OK |
| `stream_format_0xBF_single_music_0_dest_1` | STATUS | `01 60 BF C0 00 01 01 FF FF FF` | STABLE (0x0C) | 6610 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_0` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 00` | STABLE (0x0C) | 6760 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_1` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 01` | STABLE (0x0C) | 6060 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_2` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 02` | STABLE (0x0C) | 6377 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_3` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 03` | STABLE (0x0C) | 6104 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_4` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 04` | REJECTED (0x0A) | 6831 | OK |
| `stream_format_0x2F_single_music_0_dest_2` | STATUS | `01 60 2F C0 00 01 02 FF FF FF` | STABLE (0x0C) | 6656 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_0` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 00` | STABLE (0x0C) | 6543 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_1` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 01` | STABLE (0x0C) | 6031 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_2` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 02` | STABLE (0x0C) | 6038 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_3` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 03` | STABLE (0x0C) | 6393 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_4` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 04` | REJECTED (0x0A) | 6633 | OK |
| `stream_format_0xBF_single_music_0_dest_2` | STATUS | `01 60 BF C0 00 01 02 FF FF FF` | STABLE (0x0C) | 6808 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_0` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 00` | STABLE (0x0C) | 6652 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_1` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 01` | STABLE (0x0C) | 5510 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_2` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 02` | STABLE (0x0C) | 6649 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_3` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 03` | STABLE (0x0C) | 6600 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_4` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 04` | REJECTED (0x0A) | 6134 | OK |
| `stream_format_0x2F_single_music_0_src_0` | STATUS | `01 60 2F C0 01 01 00 FF FF FF` | STABLE (0x0C) | 6598 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_0` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 00` | STABLE (0x0C) | 6275 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_1` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 01` | STABLE (0x0C) | 6152 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_2` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 02` | STABLE (0x0C) | 6109 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_3` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 03` | STABLE (0x0C) | 6722 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_4` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 04` | REJECTED (0x0A) | 6578 | OK |
| `stream_format_0xBF_single_music_0_src_0` | STATUS | `01 60 BF C0 01 01 00 FF FF FF` | STABLE (0x0C) | 6231 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_0` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 00` | STABLE (0x0C) | 6223 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_1` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 01` | STABLE (0x0C) | 6739 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_2` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 02` | STABLE (0x0C) | 6608 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_3` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 03` | STABLE (0x0C) | 6474 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_4` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 04` | REJECTED (0x0A) | 6569 | OK |
| `stream_format_0x2F_single_music_0_src_1` | STATUS | `01 60 2F C0 01 01 01 FF FF FF` | STABLE (0x0C) | 5907 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_0` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 00` | STABLE (0x0C) | 6546 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_1` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 01` | STABLE (0x0C) | 5963 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_2` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 02` | STABLE (0x0C) | 6155 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_3` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 03` | STABLE (0x0C) | 6223 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_4` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 04` | REJECTED (0x0A) | 5975 | OK |
| `stream_format_0xBF_single_music_0_src_1` | STATUS | `01 60 BF C0 01 01 01 FF FF FF` | STABLE (0x0C) | 6043 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_0` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 00` | STABLE (0x0C) | 6337 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_1` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 01` | STABLE (0x0C) | 6705 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_2` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 02` | STABLE (0x0C) | 6958 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_3` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 03` | STABLE (0x0C) | 6298 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_4` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 04` | REJECTED (0x0A) | 6242 | OK |
| `stream_format_0x2F_single_music_0_src_2` | STATUS | `01 60 2F C0 01 01 02 FF FF FF` | STABLE (0x0C) | 6787 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_0` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 00` | STABLE (0x0C) | 6943 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_1` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 01` | STABLE (0x0C) | 6721 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_2` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 02` | STABLE (0x0C) | 7126 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_3` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 03` | STABLE (0x0C) | 6095 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_4` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 04` | REJECTED (0x0A) | 6677 | OK |
| `stream_format_0xBF_single_music_0_src_2` | STATUS | `01 60 BF C0 01 01 02 FF FF FF` | STABLE (0x0C) | 6090 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_0` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 00` | STABLE (0x0C) | 6775 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_1` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 01` | STABLE (0x0C) | 6767 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_2` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 02` | STABLE (0x0C) | 6044 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_3` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 03` | STABLE (0x0C) | 6333 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_4` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 04` | REJECTED (0x0A) | 6967 | OK |
| `signal_source_0xFF_music_0_dest_0` | STATUS | `01 FF 1A FF FF FE 60 00` | STABLE (0x0C) | 6123 | OK |
| `signal_source_0x0F_music_0_dest_0` | STATUS | `01 FF 1A 0F FF FE 60 00` | STABLE (0x0C) | 6111 | OK |
| `signal_source_0xFF_music_0_dest_1` | STATUS | `01 FF 1A FF FF FE 60 01` | STABLE (0x0C) | 6197 | OK |
| `signal_source_0x0F_music_0_dest_1` | STATUS | `01 FF 1A 0F FF FE 60 01` | STABLE (0x0C) | 6608 | OK |
| `signal_source_0xFF_music_0_dest_2` | STATUS | `01 FF 1A FF FF FE 60 02` | STABLE (0x0C) | 6261 | OK |
| `signal_source_0x0F_music_0_dest_2` | STATUS | `01 FF 1A 0F FF FE 60 02` | STABLE (0x0C) | 6262 | OK |
| `bridgeco_plug_info_plug_type_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 00` | NOT IMPLEMENTED (0x08) | 6088 | OK |
| `bridgeco_plug_info_plug_name_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 01` | NOT IMPLEMENTED (0x08) | 6532 | OK |
| `bridgeco_plug_info_channel_count_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 02` | NOT IMPLEMENTED (0x08) | 6142 | OK |
| `bridgeco_plug_info_channel_positions_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 03` | NOT IMPLEMENTED (0x08) | 6649 | OK |
| `bridgeco_plug_info_channel_name_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 04` | NOT IMPLEMENTED (0x08) | 6582 | OK |
| `bridgeco_plug_info_plug_input_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 05` | NOT IMPLEMENTED (0x08) | 6703 | OK |
| `bridgeco_plug_info_plug_outputs_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 06` | NOT IMPLEMENTED (0x08) | 6088 | OK |
| `bridgeco_plug_info_plug_type_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 00` | NOT IMPLEMENTED (0x08) | 6623 | OK |
| `bridgeco_plug_info_plug_name_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 01` | NOT IMPLEMENTED (0x08) | 6465 | OK |
| `bridgeco_plug_info_channel_count_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 02` | NOT IMPLEMENTED (0x08) | 6298 | OK |
| `bridgeco_plug_info_channel_positions_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 03` | NOT IMPLEMENTED (0x08) | 6024 | OK |
| `bridgeco_plug_info_channel_name_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 04` | NOT IMPLEMENTED (0x08) | 6148 | OK |
| `bridgeco_plug_info_plug_input_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 05` | NOT IMPLEMENTED (0x08) | 6774 | OK |
| `bridgeco_plug_info_plug_outputs_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 06` | NOT IMPLEMENTED (0x08) | 6228 | OK |
| `function_block_selector_status_fb_0` | STATUS | `01 08 B8 80 00 10 02 FF 01` | NOT IMPLEMENTED (0x08) | 6237 | OK |
| `function_block_selector_status_fb_1` | STATUS | `01 08 B8 80 01 10 02 FF 01` | NOT IMPLEMENTED (0x08) | 6734 | OK |
| `function_block_feature_mute_status_fb_0` | STATUS | `01 08 B8 81 00 10 02 00 01 01 FF` | NOT IMPLEMENTED (0x08) | 6103 | OK |
| `function_block_feature_volume_current_fb_0` | STATUS | `01 08 B8 81 00 10 02 00 02 02 FF FF` | NOT IMPLEMENTED (0x08) | 6309 | OK |
| `function_block_feature_mute_status_fb_1` | STATUS | `01 08 B8 81 01 10 02 00 01 01 FF` | STABLE (0x0C) | 6729 | OK |
| `function_block_feature_volume_current_fb_1` | STATUS | `01 08 B8 81 01 10 02 00 02 02 FF FF` | STABLE (0x0C) | 6253 | OK |
| `function_block_feature_volume_min_fb_1` | STATUS | `01 08 B8 81 01 02 02 00 02 02 FF FF` | STABLE (0x0C) | 6242 | OK |
| `function_block_feature_volume_max_fb_1` | STATUS | `01 08 B8 81 01 03 02 00 02 02 FF FF` | STABLE (0x0C) | 6121 | OK |
| `function_block_feature_mute_status_fb_2` | STATUS | `01 08 B8 81 02 10 02 00 01 01 FF` | NOT IMPLEMENTED (0x08) | 6644 | OK |
| `function_block_feature_volume_current_fb_2` | STATUS | `01 08 B8 81 02 10 02 00 02 02 FF FF` | NOT IMPLEMENTED (0x08) | 6285 | OK |
| `inquiry_plug_signal_format_in_0` | INQUIRY | `02 FF 19 00 90 02 FF FF` | STABLE (0x0C) | 6269 | OK |
| `inquiry_plug_signal_format_out_0` | INQUIRY | `02 FF 18 00 90 02 FF FF` | STABLE (0x0C) | 6827 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 FF 2F C0 00 00 00 00 FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6059 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 FF BF C0 00 00 00 00 FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6128 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 FF 2F C0 01 00 00 00 FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6397 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 FF BF C0 01 00 00 00 FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6268 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 00 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6466 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 00 01 00 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6422 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 01 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6099 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 00 01 01 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6505 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 02 FF FF 00 90 00 40` | REJECTED (0x0A) | 6205 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 00 01 02 FF FF 00 90 00 40` | REJECTED (0x0A) | 6316 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 00 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 5753 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 01 01 00 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6216 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 01 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6160 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 01 01 01 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6268 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 02 FF FF 00 90 00 40` | REJECTED (0x0A) | 6554 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 01 01 02 FF FF 00 90 00 40` | REJECTED (0x0A) | 6078 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF FF 00 60 00` | NOT IMPLEMENTED (0x08) | 5944 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F FF 00 60 00` | NOT IMPLEMENTED (0x08) | 6764 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF 08 01 60 01` | NOT IMPLEMENTED (0x08) | 6607 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F 08 01 60 01` | NOT IMPLEMENTED (0x08) | 6810 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF 60 02 60 02` | NOT IMPLEMENTED (0x08) | 6782 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F 60 02 60 02` | NOT IMPLEMENTED (0x08) | 6163 | OK |
| `inquiry_feature_mute_on_fb_1_ch_0` | INQUIRY | `02 08 B8 81 01 10 02 00 01 01 70` | STABLE (0x0C) | 6226 | OK |
| `inquiry_feature_mute_off_fb_1_ch_0` | INQUIRY | `02 08 B8 81 01 10 02 00 01 01 60` | STABLE (0x0C) | 6088 | OK |
| `inquiry_feature_volume_fb_1_ch_0` | INQUIRY | `02 08 B8 81 01 10 02 00 02 02 02 00` | STABLE (0x0C) | 5878 | OK |
| `post_check_plug_info` | STATUS | `01 FF 02 00 FF FF FF FF` | STABLE (0x0C) | 6701 | OK |
