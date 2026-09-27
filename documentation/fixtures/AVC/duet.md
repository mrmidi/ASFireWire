# AV/C Hardware Probe Matrix: Apogee Electronics Duet

- **Key**: `duet`
- **GUID**: `0x0003DB0A0000D112`
- **Node ID**: `0` (generation 3)
- **Driver Version**: `0.3.1`
- **Captured At**: `2026-09-27T19:36:28.535413+00:00`
- **Total Exchanges**: `209`

| Exchange | Intent | Command (Hex) | Response Code | Duration (µs) | Result |
|---|---|---|---|---|---|
| `unit_info` | STATUS | `01 FF 30 07 FF FF FF FF` | STABLE (0x0C) | 6559 | OK |
| `subunit_info_page_0` | STATUS | `01 FF 31 07 FF FF FF FF` | STABLE (0x0C) | 6875 | OK |
| `subunit_info_page_1` | STATUS | `01 FF 31 17 FF FF FF FF` | NOT IMPLEMENTED (0x08) | 6077 | OK |
| `plug_info_unit_00` | STATUS | `01 FF 02 00 FF FF FF FF` | STABLE (0x0C) | 6588 | OK |
| `plug_info_unit_01` | STATUS | `01 FF 02 01 FF FF FF FF` | STABLE (0x0C) | 6227 | OK |
| `plug_info_audio_0` | STATUS | `01 08 02 00 FF FF FF FF` | STABLE (0x0C) | 6052 | OK |
| `plug_info_music_0` | STATUS | `01 60 02 00 FF FF FF FF` | STABLE (0x0C) | 6165 | OK |
| `plug_signal_format_in_0_all_wildcard` | STATUS | `01 FF 19 00 FF FF FF FF` | STABLE (0x0C) | 6107 | OK |
| `plug_signal_format_in_0_am824_wildcard` | STATUS | `01 FF 19 00 90 FF FF FF` | STABLE (0x0C) | 6728 | OK |
| `plug_signal_format_out_0_all_wildcard` | STATUS | `01 FF 18 00 FF FF FF FF` | STABLE (0x0C) | 6813 | OK |
| `plug_signal_format_out_0_am824_wildcard` | STATUS | `01 FF 18 00 90 FF FF FF` | STABLE (0x0C) | 6872 | OK |
| `stream_format_0x2F_single_unit_iso_in_0` | STATUS | `01 FF 2F C0 00 00 00 00 FF FF` | STABLE (0x0C) | 6811 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_0` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 00` | STABLE (0x0C) | 5901 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_1` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 01` | STABLE (0x0C) | 6409 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_2` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 02` | STABLE (0x0C) | 6412 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_3` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 03` | STABLE (0x0C) | 6700 | OK |
| `stream_format_0x2F_list_unit_iso_in_0_idx_4` | STATUS | `01 FF 2F C1 00 00 00 00 FF FF 04` | REJECTED (0x0A) | 6155 | OK |
| `stream_format_0xBF_single_unit_iso_in_0` | STATUS | `01 FF BF C0 00 00 00 00 FF FF` | STABLE (0x0C) | 6106 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_0` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 00` | STABLE (0x0C) | 6695 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_1` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 01` | STABLE (0x0C) | 5990 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_2` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 02` | STABLE (0x0C) | 6904 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_3` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 03` | STABLE (0x0C) | 7066 | OK |
| `stream_format_0xBF_list_unit_iso_in_0_idx_4` | STATUS | `01 FF BF C1 00 00 00 00 FF FF 04` | REJECTED (0x0A) | 6885 | OK |
| `stream_format_0x2F_single_unit_iso_out_0` | STATUS | `01 FF 2F C0 01 00 00 00 FF FF` | STABLE (0x0C) | 6096 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_0` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 00` | STABLE (0x0C) | 6236 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_1` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 01` | STABLE (0x0C) | 6044 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_2` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 02` | STABLE (0x0C) | 5810 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_3` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 03` | STABLE (0x0C) | 6556 | OK |
| `stream_format_0x2F_list_unit_iso_out_0_idx_4` | STATUS | `01 FF 2F C1 01 00 00 00 FF FF 04` | REJECTED (0x0A) | 6857 | OK |
| `stream_format_0xBF_single_unit_iso_out_0` | STATUS | `01 FF BF C0 01 00 00 00 FF FF` | STABLE (0x0C) | 6129 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_0` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 00` | STABLE (0x0C) | 6708 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_1` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 01` | STABLE (0x0C) | 5852 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_2` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 02` | STABLE (0x0C) | 6296 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_3` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 03` | STABLE (0x0C) | 6670 | OK |
| `stream_format_0xBF_list_unit_iso_out_0_idx_4` | STATUS | `01 FF BF C1 01 00 00 00 FF FF 04` | REJECTED (0x0A) | 6189 | OK |
| `stream_format_0x2F_single_music_0_dest_0` | STATUS | `01 60 2F C0 00 01 00 FF FF FF` | STABLE (0x0C) | 6806 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_0` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 00` | STABLE (0x0C) | 6730 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_1` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 01` | STABLE (0x0C) | 6690 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_2` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 02` | STABLE (0x0C) | 6009 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_3` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 03` | STABLE (0x0C) | 6206 | OK |
| `stream_format_0x2F_list_music_0_dest_0_idx_4` | STATUS | `01 60 2F C1 00 01 00 FF FF FF 04` | REJECTED (0x0A) | 7053 | OK |
| `stream_format_0xBF_single_music_0_dest_0` | STATUS | `01 60 BF C0 00 01 00 FF FF FF` | STABLE (0x0C) | 6415 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_0` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 00` | STABLE (0x0C) | 6576 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_1` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 01` | STABLE (0x0C) | 6657 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_2` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 02` | STABLE (0x0C) | 6107 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_3` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 03` | STABLE (0x0C) | 6196 | OK |
| `stream_format_0xBF_list_music_0_dest_0_idx_4` | STATUS | `01 60 BF C1 00 01 00 FF FF FF 04` | REJECTED (0x0A) | 6028 | OK |
| `stream_format_0x2F_single_music_0_dest_1` | STATUS | `01 60 2F C0 00 01 01 FF FF FF` | STABLE (0x0C) | 6548 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_0` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 00` | STABLE (0x0C) | 6643 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_1` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 01` | STABLE (0x0C) | 6904 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_2` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 02` | STABLE (0x0C) | 6052 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_3` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 03` | STABLE (0x0C) | 6646 | OK |
| `stream_format_0x2F_list_music_0_dest_1_idx_4` | STATUS | `01 60 2F C1 00 01 01 FF FF FF 04` | REJECTED (0x0A) | 6556 | OK |
| `stream_format_0xBF_single_music_0_dest_1` | STATUS | `01 60 BF C0 00 01 01 FF FF FF` | STABLE (0x0C) | 6698 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_0` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 00` | STABLE (0x0C) | 6232 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_1` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 01` | STABLE (0x0C) | 6291 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_2` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 02` | STABLE (0x0C) | 6679 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_3` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 03` | STABLE (0x0C) | 7039 | OK |
| `stream_format_0xBF_list_music_0_dest_1_idx_4` | STATUS | `01 60 BF C1 00 01 01 FF FF FF 04` | REJECTED (0x0A) | 6554 | OK |
| `stream_format_0x2F_single_music_0_dest_2` | STATUS | `01 60 2F C0 00 01 02 FF FF FF` | STABLE (0x0C) | 6475 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_0` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 00` | STABLE (0x0C) | 6099 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_1` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 01` | STABLE (0x0C) | 6523 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_2` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 02` | STABLE (0x0C) | 6066 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_3` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 03` | STABLE (0x0C) | 6705 | OK |
| `stream_format_0x2F_list_music_0_dest_2_idx_4` | STATUS | `01 60 2F C1 00 01 02 FF FF FF 04` | REJECTED (0x0A) | 6755 | OK |
| `stream_format_0xBF_single_music_0_dest_2` | STATUS | `01 60 BF C0 00 01 02 FF FF FF` | STABLE (0x0C) | 6049 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_0` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 00` | STABLE (0x0C) | 6594 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_1` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 01` | STABLE (0x0C) | 6466 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_2` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 02` | STABLE (0x0C) | 6493 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_3` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 03` | STABLE (0x0C) | 5985 | OK |
| `stream_format_0xBF_list_music_0_dest_2_idx_4` | STATUS | `01 60 BF C1 00 01 02 FF FF FF 04` | REJECTED (0x0A) | 6166 | OK |
| `stream_format_0x2F_single_music_0_src_0` | STATUS | `01 60 2F C0 01 01 00 FF FF FF` | STABLE (0x0C) | 6057 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_0` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 00` | STABLE (0x0C) | 6651 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_1` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 01` | STABLE (0x0C) | 6662 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_2` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 02` | STABLE (0x0C) | 6439 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_3` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 03` | STABLE (0x0C) | 6067 | OK |
| `stream_format_0x2F_list_music_0_src_0_idx_4` | STATUS | `01 60 2F C1 01 01 00 FF FF FF 04` | REJECTED (0x0A) | 6564 | OK |
| `stream_format_0xBF_single_music_0_src_0` | STATUS | `01 60 BF C0 01 01 00 FF FF FF` | STABLE (0x0C) | 6716 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_0` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 00` | STABLE (0x0C) | 6347 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_1` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 01` | STABLE (0x0C) | 6186 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_2` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 02` | STABLE (0x0C) | 6085 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_3` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 03` | STABLE (0x0C) | 6540 | OK |
| `stream_format_0xBF_list_music_0_src_0_idx_4` | STATUS | `01 60 BF C1 01 01 00 FF FF FF 04` | REJECTED (0x0A) | 6132 | OK |
| `stream_format_0x2F_single_music_0_src_1` | STATUS | `01 60 2F C0 01 01 01 FF FF FF` | STABLE (0x0C) | 6492 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_0` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 00` | STABLE (0x0C) | 6597 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_1` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 01` | STABLE (0x0C) | 6112 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_2` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 02` | STABLE (0x0C) | 5903 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_3` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 03` | STABLE (0x0C) | 6155 | OK |
| `stream_format_0x2F_list_music_0_src_1_idx_4` | STATUS | `01 60 2F C1 01 01 01 FF FF FF 04` | REJECTED (0x0A) | 7008 | OK |
| `stream_format_0xBF_single_music_0_src_1` | STATUS | `01 60 BF C0 01 01 01 FF FF FF` | STABLE (0x0C) | 6524 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_0` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 00` | STABLE (0x0C) | 6716 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_1` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 01` | STABLE (0x0C) | 5974 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_2` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 02` | STABLE (0x0C) | 6579 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_3` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 03` | STABLE (0x0C) | 6811 | OK |
| `stream_format_0xBF_list_music_0_src_1_idx_4` | STATUS | `01 60 BF C1 01 01 01 FF FF FF 04` | REJECTED (0x0A) | 6793 | OK |
| `stream_format_0x2F_single_music_0_src_2` | STATUS | `01 60 2F C0 01 01 02 FF FF FF` | STABLE (0x0C) | 6036 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_0` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 00` | STABLE (0x0C) | 6388 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_1` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 01` | STABLE (0x0C) | 6132 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_2` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 02` | STABLE (0x0C) | 6585 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_3` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 03` | STABLE (0x0C) | 6610 | OK |
| `stream_format_0x2F_list_music_0_src_2_idx_4` | STATUS | `01 60 2F C1 01 01 02 FF FF FF 04` | REJECTED (0x0A) | 6906 | OK |
| `stream_format_0xBF_single_music_0_src_2` | STATUS | `01 60 BF C0 01 01 02 FF FF FF` | STABLE (0x0C) | 6098 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_0` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 00` | STABLE (0x0C) | 5581 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_1` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 01` | STABLE (0x0C) | 6273 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_2` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 02` | STABLE (0x0C) | 6693 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_3` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 03` | STABLE (0x0C) | 6749 | OK |
| `stream_format_0xBF_list_music_0_src_2_idx_4` | STATUS | `01 60 BF C1 01 01 02 FF FF FF 04` | REJECTED (0x0A) | 6744 | OK |
| `signal_source_0xFF_music_0_dest_0` | STATUS | `01 FF 1A FF FF FE 60 00` | STABLE (0x0C) | 6090 | OK |
| `signal_source_0x0F_music_0_dest_0` | STATUS | `01 FF 1A 0F FF FE 60 00` | STABLE (0x0C) | 6496 | OK |
| `signal_source_0xFF_music_0_dest_1` | STATUS | `01 FF 1A FF FF FE 60 01` | STABLE (0x0C) | 6183 | OK |
| `signal_source_0x0F_music_0_dest_1` | STATUS | `01 FF 1A 0F FF FE 60 01` | STABLE (0x0C) | 6559 | OK |
| `signal_source_0xFF_music_0_dest_2` | STATUS | `01 FF 1A FF FF FE 60 02` | STABLE (0x0C) | 6936 | OK |
| `signal_source_0x0F_music_0_dest_2` | STATUS | `01 FF 1A 0F FF FE 60 02` | STABLE (0x0C) | 6747 | OK |
| `bridgeco_plug_info_plug_type_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 00` | NOT IMPLEMENTED (0x08) | 6060 | OK |
| `bridgeco_plug_info_plug_name_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 01` | NOT IMPLEMENTED (0x08) | 6496 | OK |
| `bridgeco_plug_info_channel_count_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 02` | NOT IMPLEMENTED (0x08) | 6676 | OK |
| `bridgeco_plug_info_channel_positions_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 03` | NOT IMPLEMENTED (0x08) | 6137 | OK |
| `bridgeco_plug_info_channel_name_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 04` | NOT IMPLEMENTED (0x08) | 6165 | OK |
| `bridgeco_plug_info_plug_input_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 05` | NOT IMPLEMENTED (0x08) | 6898 | OK |
| `bridgeco_plug_info_plug_outputs_in_0` | STATUS | `01 FF 02 C0 00 00 00 00 FF 06` | NOT IMPLEMENTED (0x08) | 6792 | OK |
| `bridgeco_plug_info_plug_type_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 00` | NOT IMPLEMENTED (0x08) | 6485 | OK |
| `bridgeco_plug_info_plug_name_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 01` | NOT IMPLEMENTED (0x08) | 6155 | OK |
| `bridgeco_plug_info_channel_count_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 02` | NOT IMPLEMENTED (0x08) | 6130 | OK |
| `bridgeco_plug_info_channel_positions_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 03` | NOT IMPLEMENTED (0x08) | 6189 | OK |
| `bridgeco_plug_info_channel_name_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 04` | NOT IMPLEMENTED (0x08) | 6157 | OK |
| `bridgeco_plug_info_plug_input_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 05` | NOT IMPLEMENTED (0x08) | 6716 | OK |
| `bridgeco_plug_info_plug_outputs_out_0` | STATUS | `01 FF 02 C0 01 00 00 00 FF 06` | NOT IMPLEMENTED (0x08) | 6067 | OK |
| `function_block_selector_status_fb_0` | STATUS | `01 08 B8 80 00 10 02 FF 01` | NOT IMPLEMENTED (0x08) | 6038 | OK |
| `function_block_selector_status_fb_1` | STATUS | `01 08 B8 80 01 10 02 FF 01` | NOT IMPLEMENTED (0x08) | 6546 | OK |
| `function_block_feature_mute_status_fb_0` | STATUS | `01 08 B8 81 00 10 02 00 01 01 FF` | NOT IMPLEMENTED (0x08) | 6650 | OK |
| `function_block_feature_volume_current_fb_0` | STATUS | `01 08 B8 81 00 10 02 00 02 02 FF FF` | NOT IMPLEMENTED (0x08) | 6759 | OK |
| `function_block_feature_mute_status_fb_1` | STATUS | `01 08 B8 81 01 10 02 00 01 01 FF` | STABLE (0x0C) | 6175 | OK |
| `function_block_feature_volume_current_fb_1` | STATUS | `01 08 B8 81 01 10 02 00 02 02 FF FF` | STABLE (0x0C) | 6097 | OK |
| `function_block_feature_volume_min_fb_1` | STATUS | `01 08 B8 81 01 02 02 00 02 02 FF FF` | STABLE (0x0C) | 6556 | OK |
| `function_block_feature_volume_max_fb_1` | STATUS | `01 08 B8 81 01 03 02 00 02 02 FF FF` | STABLE (0x0C) | 6814 | OK |
| `function_block_feature_mute_status_fb_2` | STATUS | `01 08 B8 81 02 10 02 00 01 01 FF` | NOT IMPLEMENTED (0x08) | 6347 | OK |
| `function_block_feature_volume_current_fb_2` | STATUS | `01 08 B8 81 02 10 02 00 02 02 FF FF` | NOT IMPLEMENTED (0x08) | 5771 | OK |
| `inquiry_plug_signal_format_in_0` | INQUIRY | `02 FF 19 00 90 02 FF FF` | STABLE (0x0C) | 6659 | OK |
| `inquiry_plug_signal_format_out_0` | INQUIRY | `02 FF 18 00 90 02 FF FF` | STABLE (0x0C) | 6018 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 FF 2F C0 00 00 00 00 FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 5643 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 FF BF C0 00 00 00 00 FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6838 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 FF 2F C0 01 00 00 00 FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6191 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 FF BF C0 01 00 00 00 FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 5895 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 00 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6667 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 00 01 00 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6775 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 01 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6175 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 00 01 01 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6047 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 00 01 02 FF FF 00 90 00 40` | REJECTED (0x0A) | 6233 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 00 01 02 FF FF 00 90 00 40` | REJECTED (0x0A) | 6983 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 00 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6874 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 01 01 00 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6602 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 01 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6098 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 01 01 01 FF FF 00 90 40 04 02 01 02 06` | STABLE (0x0C) | 6554 | OK |
| `inquiry_stream_format_0x2F_single` | INQUIRY | `02 60 2F C0 01 01 02 FF FF 00 90 00 40` | REJECTED (0x0A) | 6648 | OK |
| `inquiry_stream_format_0xBF_single` | INQUIRY | `02 60 BF C0 01 01 02 FF FF 00 90 00 40` | REJECTED (0x0A) | 6777 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF FF 00 60 00` | NOT IMPLEMENTED (0x08) | 6205 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F FF 00 60 00` | NOT IMPLEMENTED (0x08) | 6082 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF 08 01 60 01` | NOT IMPLEMENTED (0x08) | 6006 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F 08 01 60 01` | NOT IMPLEMENTED (0x08) | 6093 | OK |
| `inquiry_signal_source_0xFF` | INQUIRY | `02 FF 1A FF 60 02 60 02` | NOT IMPLEMENTED (0x08) | 6810 | OK |
| `inquiry_signal_source_0x0F` | INQUIRY | `02 FF 1A 0F 60 02 60 02` | NOT IMPLEMENTED (0x08) | 7099 | OK |
| `inquiry_feature_mute_on_fb_1_ch_0` | INQUIRY | `02 08 B8 81 01 10 02 00 01 01 70` | STABLE (0x0C) | 2722 | OK |
| `inquiry_feature_mute_off_fb_1_ch_0` | INQUIRY | `02 08 B8 81 01 10 02 00 01 01 60` | STABLE (0x0C) | 6004 | OK |
| `inquiry_feature_volume_fb_1_ch_0` | INQUIRY | `02 08 B8 81 01 10 02 00 02 02 02 00` | STABLE (0x0C) | 5797 | OK |
| `oxford_firmware_id` | CONTROL | `F0 05 00 00` | STABLE (0x0C) | 45804 | OK |
| `oxford_hardware_id` | CONTROL | `F0 09 00 20` | STABLE (0x0C) | 334 | OK |
| `apogee_dsp_input_meters` | CONTROL | `F0 08 00 04 08` | STABLE (0x0C) | 38033 | OK |
| `apogee_dsp_mixer_meters` | CONTROL | `F0 08 04 04 10` | STABLE (0x0C) | 36656 | OK |
| `apogee_mic_polarity_ch0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 00 80 00` | STABLE (0x0C) | 6240 | OK |
| `apogee_mic_polarity_ch1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 00 80 01` | STABLE (0x0C) | 6610 | OK |
| `apogee_xlr_is_mic_level_ch0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 01 80 00` | STABLE (0x0C) | 6441 | OK |
| `apogee_xlr_is_mic_level_ch1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 01 80 01` | STABLE (0x0C) | 6008 | OK |
| `apogee_xlr_is_consumer_ch0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 02 80 00` | STABLE (0x0C) | 6160 | OK |
| `apogee_xlr_is_consumer_ch1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 02 80 01` | STABLE (0x0C) | 6052 | OK |
| `apogee_mic_phantom_ch0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 03 80 00` | STABLE (0x0C) | 6476 | OK |
| `apogee_mic_phantom_ch1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 03 80 01` | STABLE (0x0C) | 6075 | OK |
| `apogee_out_is_consumer` | STATUS | `01 FF 00 00 03 DB 50 43 4D 04 80 FF` | STABLE (0x0C) | 5973 | OK |
| `apogee_in_gain_ch0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 05 80 00` | STABLE (0x0C) | 6474 | OK |
| `apogee_in_gain_ch1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 05 80 01` | STABLE (0x0C) | 6884 | OK |
| `apogee_hw_state` | STATUS | `01 FF 00 00 03 DB 50 43 4D 07 FF FF` | STABLE (0x0C) | 6157 | OK |
| `apogee_out_mute` | STATUS | `01 FF 00 00 03 DB 50 43 4D 09 80 FF` | STABLE (0x0C) | 6129 | OK |
| `apogee_in_src_is_phone_ch0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 0C 80 00` | STABLE (0x0C) | 6567 | OK |
| `apogee_in_src_is_phone_ch1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 0C 80 01` | STABLE (0x0C) | 6680 | OK |
| `apogee_out_src_is_mixer` | STATUS | `01 FF 00 00 03 DB 50 43 4D 11 FF FF` | STABLE (0x0C) | 6660 | OK |
| `apogee_disp_overhold_2s` | STATUS | `01 FF 00 00 03 DB 50 43 4D 13 FF FF` | STABLE (0x0C) | 6664 | OK |
| `apogee_out_volume` | STATUS | `01 FF 00 00 03 DB 50 43 4D 15 80 FF` | STABLE (0x0C) | 6163 | OK |
| `apogee_mute_line_out` | STATUS | `01 FF 00 00 03 DB 50 43 4D 16 80 FF` | STABLE (0x0C) | 5886 | OK |
| `apogee_mute_hp_out` | STATUS | `01 FF 00 00 03 DB 50 43 4D 17 80 FF` | STABLE (0x0C) | 6634 | OK |
| `apogee_unmute_line_out` | STATUS | `01 FF 00 00 03 DB 50 43 4D 18 80 FF` | STABLE (0x0C) | 5617 | OK |
| `apogee_unmute_hp_out` | STATUS | `01 FF 00 00 03 DB 50 43 4D 19 80 FF` | STABLE (0x0C) | 6723 | OK |
| `apogee_disp_is_input` | STATUS | `01 FF 00 00 03 DB 50 43 4D 1B FF FF` | STABLE (0x0C) | 6172 | OK |
| `apogee_in_clickless` | STATUS | `01 FF 00 00 03 DB 50 43 4D 1E FF FF` | STABLE (0x0C) | 6082 | OK |
| `apogee_disp_follow_knob` | STATUS | `01 FF 00 00 03 DB 50 43 4D 22 FF FF` | STABLE (0x0C) | 6027 | OK |
| `apogee_mixer_src_0_dst_0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 00 00` | STABLE (0x0C) | 5654 | OK |
| `apogee_mixer_src_0_dst_1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 00 01` | STABLE (0x0C) | 5998 | OK |
| `apogee_mixer_src_1_dst_0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 01 00` | STABLE (0x0C) | 6137 | OK |
| `apogee_mixer_src_1_dst_1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 01 01` | STABLE (0x0C) | 6450 | OK |
| `apogee_mixer_src_2_dst_0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 10 00` | STABLE (0x0C) | 6849 | OK |
| `apogee_mixer_src_2_dst_1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 10 01` | STABLE (0x0C) | 6384 | OK |
| `apogee_mixer_src_3_dst_0` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 11 00` | STABLE (0x0C) | 6662 | OK |
| `apogee_mixer_src_3_dst_1` | STATUS | `01 FF 00 00 03 DB 50 43 4D 10 11 01` | STABLE (0x0C) | 6098 | OK |
| `inquiry_apogee_mic_phantom_ch0_on` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 03 80 00 70` | ACCEPTED (0x09) | 6441 | OK |
| `inquiry_apogee_mic_phantom_ch0_off` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 03 80 00 60` | ACCEPTED (0x09) | 6785 | OK |
| `inquiry_apogee_in_gain_ch0_30db` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 05 80 00 1E` | ACCEPTED (0x09) | 6032 | OK |
| `inquiry_apogee_out_mute_on` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 09 80 FF 70` | ACCEPTED (0x09) | 6768 | OK |
| `inquiry_apogee_out_mute_off` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 09 80 FF 60` | ACCEPTED (0x09) | 6826 | OK |
| `inquiry_apogee_out_volume_40` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 15 80 FF 28` | ACCEPTED (0x09) | 6499 | OK |
| `inquiry_apogee_disp_follow_knob_on` | INQUIRY | `02 FF 00 00 03 DB 50 43 4D 22 FF FF 70` | ACCEPTED (0x09) | 6260 | OK |
| `post_check_plug_info` | STATUS | `01 FF 02 00 FF FF FF FF` | STABLE (0x0C) | 6646 | OK |
