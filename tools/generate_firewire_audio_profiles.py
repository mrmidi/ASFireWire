#!/usr/bin/env python3
"""Generate FireWire audio identity metadata for ASFireWire.

The generated tables are diagnostic metadata, not a support promise. Newly
imported public records must stay metadata-only unless ASFireWire has an
explicit, tested binding path for that device.
"""

from __future__ import annotations

import dataclasses
import re
import sys
import textwrap
import urllib.request
from pathlib import Path


FFADO_URL = "https://raw.githubusercontent.com/adiknoth/ffado/master/libffado/configuration"
SYSTEMD_HWDB_URL = "https://raw.githubusercontent.com/systemd/systemd/main/hwdb.d/80-ieee1394-unit-function.hwdb"
AM_CONFIG_ROMS_URL = "https://raw.githubusercontent.com/takaswie/am-config-roms/main/README.rst"


ROOT = Path(__file__).resolve().parents[1]
CPP_OUT = ROOT / "ASFWDriver" / "Protocols" / "Audio" / "FireWireAudioDeviceProfiles.hpp"
SWIFT_OUT = ROOT / "ASFW" / "Models" / "FireWireDeviceProfiles.generated.swift"


@dataclasses.dataclass
class Profile:
    vendor_id: int
    model_id: int
    unit_specifier_id: int = 0
    unit_version: int = 0
    vendor_name: str = ""
    model_name: str = ""
    protocol_family: str = "Unknown"
    mixer_hint: str = ""
    support_status: str = "MetadataOnly"
    integration_mode: str = "None"
    audio: bool = True
    midi: bool = False
    video: bool = False
    requires_unit_match: bool = False
    source: str = ""

    @property
    def key(self) -> tuple[int, int, int, int]:
        return (
            normalize24(self.vendor_id),
            normalize24(self.model_id),
            normalize24(self.unit_specifier_id),
            normalize24(self.unit_version),
        )

    @property
    def vendor_model_key(self) -> tuple[int, int]:
        return (normalize24(self.vendor_id), normalize24(self.model_id))


def normalize24(value: int) -> int:
    return value & 0x00FFFFFF


def fetch(url: str) -> str:
    with urllib.request.urlopen(url, timeout=30) as response:
        return response.read().decode("utf-8", errors="replace")


def parse_int(value: str) -> int:
    value = value.strip()
    return int(value, 16) if value.lower().startswith("0x") else int(value, 10)


def clean_name(value: str) -> str:
    return re.sub(r"\s+", " ", value.strip())


def protocol_from_ffado(driver: str) -> str:
    mapping = {
        "DICE": "DICE",
        "BEBOB": "BeBoB",
        "FIREWORKS": "FireWorks",
        "MOTU": "MOTU",
        "RME": "RME",
        "OXFORD": "Oxford",
        "GENERICAVC": "AVC",
    }
    return mapping.get(driver.upper(), "Unknown")


def parse_ffado(text: str) -> list[Profile]:
    profiles: list[Profile] = []
    for block in re.findall(r"\{(.*?)\},", text, flags=re.S):
        fields: dict[str, str] = {}
        for match in re.finditer(r"(\w+)\s*=\s*(?:\"([^\"]*)\"|([^;]+))\s*;", block):
            key = match.group(1)
            value = match.group(2) if match.group(2) is not None else match.group(3)
            fields[key] = value.strip()

        if "vendorid" not in fields or "modelid" not in fields:
            continue

        profiles.append(Profile(
            vendor_id=parse_int(fields["vendorid"]),
            model_id=parse_int(fields["modelid"]),
            vendor_name=clean_name(fields.get("vendorname", "")),
            model_name=clean_name(fields.get("modelname", "")),
            protocol_family=protocol_from_ffado(fields.get("driver", "")),
            mixer_hint=clean_name(fields.get("mixer", "")),
            source="FFADO",
        ))
    return profiles


def parse_systemd_key(key: str) -> tuple[int, int, int, int] | None:
    node = re.search(
        r"ven0x([0-9a-fA-F]+)(?:mo0x([0-9a-fA-F]+))?units\*?0x([0-9a-fA-F]+):0x([0-9a-fA-F]+)",
        key,
    )
    if node:
        return (
            parse_int("0x" + node.group(1)),
            parse_int("0x" + (node.group(2) or "0")),
            parse_int("0x" + node.group(3)),
            parse_int("0x" + node.group(4)),
        )

    alias = re.search(
        r"ven([0-9a-fA-F]{8})mo([0-9a-fA-F]{8})sp([0-9a-fA-F]{8})ver([0-9a-fA-F]{8})",
        key,
    )
    if alias:
        return tuple(parse_int("0x" + alias.group(i)) for i in range(1, 5))  # type: ignore[return-value]
    return None


def parse_systemd(text: str) -> list[Profile]:
    profiles: list[Profile] = []
    blocks = re.split(r"\n\s*\n", text)
    for block in blocks:
        lines = [line.rstrip() for line in block.splitlines() if line.strip() and not line.lstrip().startswith("#")]
        if not lines:
            continue

        properties: dict[str, str] = {}
        keys: list[str] = []
        for line in lines:
            if line.startswith("ieee1394:"):
                keys.append(line)
            elif "=" in line:
                key, value = line.strip().split("=", 1)
                properties[key] = value

        has_audio = properties.get("IEEE1394_UNIT_FUNCTION_AUDIO") == "1"
        has_midi = properties.get("IEEE1394_UNIT_FUNCTION_MIDI") == "1"
        has_video = properties.get("IEEE1394_UNIT_FUNCTION_VIDEO") == "1"
        if not (has_audio or has_midi or has_video):
            continue

        vendor_name = clean_name(properties.get("ID_VENDOR_FROM_DATABASE", ""))
        model_name = clean_name(properties.get("ID_MODEL_FROM_DATABASE", ""))
        seen: set[tuple[int, int, int, int]] = set()
        for key in keys:
            parsed = parse_systemd_key(key)
            if parsed is None:
                continue
            vendor_id, model_id, unit_spec, unit_version = parsed
            profile = Profile(
                vendor_id=vendor_id,
                model_id=model_id,
                unit_specifier_id=unit_spec,
                unit_version=unit_version,
                vendor_name=vendor_name,
                model_name=model_name,
                audio=has_audio,
                midi=has_midi,
                video=has_video,
                requires_unit_match=unit_spec != 0 or unit_version != 0,
                source="systemd-hwdb",
            )
            if profile.key not in seen:
                profiles.append(profile)
                seen.add(profile.key)
    return profiles


def merge_profiles(ffado: list[Profile], systemd: list[Profile]) -> list[Profile]:
    by_key: dict[tuple[int, int, int, int], Profile] = {}
    ffado_by_vm: dict[tuple[int, int], list[Profile]] = {}
    systemd_by_vm: dict[tuple[int, int], list[Profile]] = {}

    for profile in ffado:
        profile.vendor_id = normalize24(profile.vendor_id)
        profile.model_id = normalize24(profile.model_id)
        profile.unit_specifier_id = normalize24(profile.unit_specifier_id)
        profile.unit_version = normalize24(profile.unit_version)
        ffado_by_vm.setdefault(profile.vendor_model_key, []).append(profile)

    for profile in systemd:
        profile.vendor_id = normalize24(profile.vendor_id)
        profile.model_id = normalize24(profile.model_id)
        profile.unit_specifier_id = normalize24(profile.unit_specifier_id)
        profile.unit_version = normalize24(profile.unit_version)
        systemd_by_vm.setdefault(profile.vendor_model_key, []).append(profile)

    for profile in ffado:
        systemd_matches = systemd_by_vm.get(profile.vendor_model_key, [])
        exact_matches = [match for match in systemd_matches if match.unit_specifier_id or match.unit_version]
        if len(exact_matches) == 1:
            profile.unit_specifier_id = exact_matches[0].unit_specifier_id
            profile.unit_version = exact_matches[0].unit_version
            profile.audio = profile.audio or exact_matches[0].audio
            profile.midi = profile.midi or exact_matches[0].midi
            profile.video = profile.video or exact_matches[0].video
            profile.source = "FFADO+systemd-hwdb"
        by_key[profile.key] = profile

    for profile in systemd:
        if profile.key in by_key:
            existing = by_key[profile.key]
            existing.vendor_name = existing.vendor_name or profile.vendor_name
            existing.model_name = existing.model_name or profile.model_name
            existing.audio = existing.audio or profile.audio
            existing.midi = existing.midi or profile.midi
            existing.video = existing.video or profile.video
            if "systemd-hwdb" not in existing.source:
                existing.source = f"{existing.source}+systemd-hwdb" if existing.source else "systemd-hwdb"
            continue

        # Avoid adding unit-specific duplicates when the matching FFADO record
        # was already upgraded with the same exact unit evidence.
        if any(profile.key == existing.key for existing in ffado_by_vm.get(profile.vendor_model_key, [])):
            continue
        by_key[profile.key] = profile

    apply_support_overrides(by_key)

    vendor_model_counts: dict[tuple[int, int], int] = {}
    for profile in by_key.values():
        vendor_model_counts[profile.vendor_model_key] = vendor_model_counts.get(profile.vendor_model_key, 0) + 1

    for profile in by_key.values():
        if vendor_model_counts[profile.vendor_model_key] > 1 and (profile.unit_specifier_id or profile.unit_version):
            profile.requires_unit_match = True

    return sorted(
        by_key.values(),
        key=lambda p: (p.vendor_name.lower(), p.model_name.lower(), p.vendor_id, p.model_id, p.unit_specifier_id, p.unit_version),
    )


def apply_support_overrides(profiles: dict[tuple[int, int, int, int], Profile]) -> None:
    def ensure(vendor: int, model: int, vendor_name: str, model_name: str,
               family: str, status: str, integration: str, source: str,
               unit_spec: int = 0, unit_version: int = 0, mixer: str = "") -> None:
        key = (normalize24(vendor), normalize24(model), normalize24(unit_spec), normalize24(unit_version))
        profile = profiles.get(key)
        if profile is None:
            profile = Profile(vendor, model, unit_spec, unit_version, vendor_name, model_name, family, mixer, status, integration, True, False, False, False, source)
            profiles[key] = profile
        profile.vendor_name = vendor_name
        profile.model_name = model_name
        profile.protocol_family = family
        profile.support_status = status
        profile.integration_mode = integration
        profile.source = profile.source if source in profile.source else source
        if mixer:
            profile.mixer_hint = mixer

    # Existing ASFW binding/identity policy. These are not inferred from the
    # broad import; they reflect code paths already present in this repository.
    for model, name in [
        (0x000009, "Saffire Pro 14"),
        (0x000007, "Saffire Pro 24"),
        (0x000008, "Saffire Pro 24 DSP"),
    ]:
        ensure(0x00130e, model, "Focusrite", name, "DICE", "SupportedBinding", "HardcodedNub", "ASFW")

    for model, name in [
        (0x000005, "Saffire Pro 40"),
        (0x000006, "Liquid Saffire 56"),
        (0x000012, "Saffire Pro 26"),
        (0x0000de, "Saffire Pro 40 (TCD3070)"),
    ]:
        ensure(0x00130e, model, "Focusrite", name, "DICE", "Deferred", "None", "ASFW")

    ensure(0x0003db, 0x01dddd, "Apogee", "Duet", "Oxford", "SupportedBinding", "AVCDriven", "ASFW")
    ensure(0x000595, 0x000000, "Alesis", "MultiMix FireWire", "DICE", "SupportedBinding", "HardcodedNub", "ASFW")
    ensure(0x10c73f, 0x000001, "Midas", "Venice F32", "DICE", "SupportedBinding", "HardcodedNub", "FFADO+systemd-hwdb", 0x10c73f, 0x000001, "Generic_Dice_EAP")


def cpp_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def swift_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


SWIFT_PROTOCOL_CASES = {
    "Unknown": "unknown",
    "DICE": "dice",
    "BeBoB": "bebob",
    "FireWorks": "fireWorks",
    "MOTU": "motu",
    "RME": "rme",
    "Oxford": "oxford",
    "Digi00x": "digi00x",
    "Tascam": "tascam",
    "AVC": "avc",
    "UnsupportedKnownAudio": "unsupportedKnownAudio",
}


SWIFT_INTEGRATION_CASES = {
    "None": "none",
    "HardcodedNub": "hardcodedNub",
    "AVCDriven": "avcDriven",
}


def flags_for(profile: Profile) -> list[str]:
    flags: list[str] = []
    if profile.audio:
        flags.append("kFireWireAudioProfileFlagAudio")
    if profile.midi:
        flags.append("kFireWireAudioProfileFlagMidi")
    if profile.video:
        flags.append("kFireWireAudioProfileFlagVideo")
    if profile.requires_unit_match:
        flags.append("kFireWireAudioProfileFlagRequiresUnitMatch")
    if profile.source != "FFADO" and "systemd-hwdb" in profile.source:
        flags.append("kFireWireAudioProfileFlagHasUnitEvidence")
    return flags or ["0"]


def generate_cpp(profiles: list[Profile]) -> str:
    rows = []
    for p in profiles:
        rows.append(
            '    FireWireAudioDeviceProfile{'
            f'0x{p.vendor_id:06x}U, 0x{p.model_id:06x}U, 0x{p.unit_specifier_id:06x}U, 0x{p.unit_version:06x}U, '
            f'"{cpp_string(p.vendor_name)}", "{cpp_string(p.model_name)}", '
            f'FireWireProtocolFamily::k{p.protocol_family}, '
            f'"{cpp_string(p.mixer_hint)}", '
            f'FireWireProfileSupportStatus::k{p.support_status}, '
            f'DeviceIntegrationMode::k{p.integration_mode}, '
            f'({" | ".join(flags_for(p))}), '
            f'"{cpp_string(p.source)}"'
            '},'
        )

    return textwrap.dedent(f"""\
        // SPDX-License-Identifier: LGPL-3.0-or-later
        // Copyright (c) 2026 ASFireWire Project
        //
        // Generated by tools/generate_firewire_audio_profiles.py.
        // Sources:
        // - {FFADO_URL}
        // - {SYSTEMD_HWDB_URL}
        // - {AM_CONFIG_ROMS_URL}
        //
        // Recognition is not support. Most imported profiles are metadata-only.

        #pragma once

        #include <cstddef>
        #include <cstdint>
        #include <optional>

        namespace ASFW::Audio {{

        enum class DeviceIntegrationMode : uint8_t {{
            kNone = 0,
            kHardcodedNub,
            kAVCDriven,
        }};

        enum class FireWireProtocolFamily : uint8_t {{
            kUnknown = 0,
            kDICE,
            kBeBoB,
            kFireWorks,
            kMOTU,
            kRME,
            kOxford,
            kDigi00x,
            kTascam,
            kAVC,
            kUnsupportedKnownAudio,
        }};

        enum class FireWireProfileSupportStatus : uint8_t {{
            kMetadataOnly = 0,
            kDeferred,
            kSupportedBinding,
        }};

        enum FireWireAudioProfileFlags : uint32_t {{
            kFireWireAudioProfileFlagAudio = 1U << 0U,
            kFireWireAudioProfileFlagMidi = 1U << 1U,
            kFireWireAudioProfileFlagVideo = 1U << 2U,
            kFireWireAudioProfileFlagRequiresUnitMatch = 1U << 3U,
            kFireWireAudioProfileFlagHasUnitEvidence = 1U << 4U,
        }};

        struct FireWireAudioDeviceProfile {{
            uint32_t vendorId;
            uint32_t modelId;
            uint32_t unitSpecifierId;
            uint32_t unitVersion;
            const char* vendorName;
            const char* modelName;
            FireWireProtocolFamily protocolFamily;
            const char* mixerHint;
            FireWireProfileSupportStatus supportStatus;
            DeviceIntegrationMode integrationMode;
            uint32_t flags;
            const char* source;
        }};

        [[nodiscard]] constexpr uint32_t Normalize24(uint32_t value) noexcept {{
            return value & 0x00ffffffU;
        }}

        [[nodiscard]] constexpr bool HasProfileFlag(const FireWireAudioDeviceProfile& profile,
                                                    FireWireAudioProfileFlags flag) noexcept {{
            return (profile.flags & static_cast<uint32_t>(flag)) != 0U;
        }}

        [[nodiscard]] constexpr const char* ToString(FireWireProtocolFamily family) noexcept {{
            switch (family) {{
                case FireWireProtocolFamily::kDICE: return "DICE";
                case FireWireProtocolFamily::kBeBoB: return "BeBoB";
                case FireWireProtocolFamily::kFireWorks: return "FireWorks";
                case FireWireProtocolFamily::kMOTU: return "MOTU";
                case FireWireProtocolFamily::kRME: return "RME";
                case FireWireProtocolFamily::kOxford: return "Oxford";
                case FireWireProtocolFamily::kDigi00x: return "Digi00x";
                case FireWireProtocolFamily::kTascam: return "Tascam";
                case FireWireProtocolFamily::kAVC: return "AVC";
                case FireWireProtocolFamily::kUnsupportedKnownAudio: return "UnsupportedKnownAudio";
                default: return "Unknown";
            }}
        }}

        [[nodiscard]] constexpr const char* ToString(FireWireProfileSupportStatus status) noexcept {{
            switch (status) {{
                case FireWireProfileSupportStatus::kDeferred: return "Deferred";
                case FireWireProfileSupportStatus::kSupportedBinding: return "SupportedBinding";
                default: return "MetadataOnly";
            }}
        }}

        inline constexpr FireWireAudioDeviceProfile kFireWireAudioDeviceProfiles[] = {{
        {chr(10).join(rows)}
        }};

        inline constexpr size_t kFireWireAudioDeviceProfileCount =
            sizeof(kFireWireAudioDeviceProfiles) / sizeof(kFireWireAudioDeviceProfiles[0]);

        [[nodiscard]] constexpr const FireWireAudioDeviceProfile* LookupProfileByVendorModel(
            uint32_t vendorId,
            uint32_t modelId
        ) noexcept {{
            const uint32_t vendor = Normalize24(vendorId);
            const uint32_t model = Normalize24(modelId);
            for (const auto& profile : kFireWireAudioDeviceProfiles) {{
                if (Normalize24(profile.vendorId) == vendor &&
                    Normalize24(profile.modelId) == model &&
                    !HasProfileFlag(profile, kFireWireAudioProfileFlagRequiresUnitMatch)) {{
                    return &profile;
                }}
            }}
            return nullptr;
        }}

        [[nodiscard]] constexpr const FireWireAudioDeviceProfile* LookupProfileByVendorModelUnit(
            uint32_t vendorId,
            uint32_t modelId,
            uint32_t unitSpecifierId,
            uint32_t unitVersion
        ) noexcept {{
            const uint32_t vendor = Normalize24(vendorId);
            const uint32_t model = Normalize24(modelId);
            const uint32_t spec = Normalize24(unitSpecifierId);
            const uint32_t version = Normalize24(unitVersion);
            for (const auto& profile : kFireWireAudioDeviceProfiles) {{
                if (Normalize24(profile.vendorId) == vendor &&
                    Normalize24(profile.modelId) == model &&
                    Normalize24(profile.unitSpecifierId) == spec &&
                    Normalize24(profile.unitVersion) == version) {{
                    return &profile;
                }}
            }}
            return nullptr;
        }}

        [[nodiscard]] constexpr const FireWireAudioDeviceProfile* LookupBestProfile(
            uint32_t vendorId,
            uint32_t modelId,
            uint32_t unitSpecifierId = 0,
            uint32_t unitVersion = 0
        ) noexcept {{
            const auto* weak = LookupProfileByVendorModel(vendorId, modelId);
            if (unitSpecifierId != 0U || unitVersion != 0U) {{
                if (const auto* exact = LookupProfileByVendorModelUnit(vendorId, modelId, unitSpecifierId, unitVersion)) {{
                    if (exact->supportStatus != FireWireProfileSupportStatus::kMetadataOnly) {{
                        return exact;
                    }}
                    if (weak && weak->supportStatus != FireWireProfileSupportStatus::kMetadataOnly) {{
                        return weak;
                    }}
                    return exact;
                }}
            }}
            return weak;
        }}

        [[nodiscard]] constexpr bool IsKnownFireWireAudioDevice(uint32_t vendorId,
                                                                uint32_t modelId,
                                                                uint32_t unitSpecifierId = 0,
                                                                uint32_t unitVersion = 0) noexcept {{
            return LookupBestProfile(vendorId, modelId, unitSpecifierId, unitVersion) != nullptr;
        }}

        }} // namespace ASFW::Audio
        """)


def generate_swift(profiles: list[Profile]) -> str:
    rows = []
    for p in profiles:
        rows.append(
            "                FireWireAudioDeviceProfile("
            f"vendorId: 0x{p.vendor_id:06X}, modelId: 0x{p.model_id:06X}, "
            f"unitSpecifierId: 0x{p.unit_specifier_id:06X}, unitVersion: 0x{p.unit_version:06X}, "
            f'vendorName: "{swift_string(p.vendor_name)}", modelName: "{swift_string(p.model_name)}", '
            f"protocolFamily: .{SWIFT_PROTOCOL_CASES[p.protocol_family]}, "
            f'mixerHint: "{swift_string(p.mixer_hint)}", supportStatus: .{p.support_status[0].lower() + p.support_status[1:]}, '
            f"integrationMode: .{SWIFT_INTEGRATION_CASES[p.integration_mode]}, "
            f"flags: {sum(flag_value(f) for f in flags_for(p) if f != '0')}, "
            f'source: "{swift_string(p.source)}"'
            "),"
        )

    return textwrap.dedent(f"""\
        // Generated by tools/generate_firewire_audio_profiles.py.
        // Recognition is not support. Most imported profiles are metadata-only.

        import Foundation

        enum FireWireProtocolFamily: String, CaseIterable, Identifiable {{
            case unknown = "Unknown"
            case dice = "DICE"
            case bebob = "BeBoB"
            case fireWorks = "FireWorks"
            case motu = "MOTU"
            case rme = "RME"
            case oxford = "Oxford"
            case digi00x = "Digi00x"
            case tascam = "Tascam"
            case avc = "AVC"
            case unsupportedKnownAudio = "UnsupportedKnownAudio"

            var id: String {{ rawValue }}
        }}

        enum FireWireProfileSupportStatus: String, CaseIterable, Identifiable {{
            case metadataOnly = "MetadataOnly"
            case deferred = "Deferred"
            case supportedBinding = "SupportedBinding"

            var id: String {{ rawValue }}
        }}

        enum FireWireDeviceIntegrationMode: String, CaseIterable, Identifiable {{
            case none = "None"
            case hardcodedNub = "HardcodedNub"
            case avcDriven = "AVCDriven"

            var id: String {{ rawValue }}
        }}

        struct FireWireAudioProfileFlags {{
            static let audio: UInt32 = 1 << 0
            static let midi: UInt32 = 1 << 1
            static let video: UInt32 = 1 << 2
            static let requiresUnitMatch: UInt32 = 1 << 3
            static let hasUnitEvidence: UInt32 = 1 << 4
        }}

        struct FireWireAudioDeviceProfile: Identifiable, Equatable {{
            let vendorId: UInt32
            let modelId: UInt32
            let unitSpecifierId: UInt32
            let unitVersion: UInt32
            let vendorName: String
            let modelName: String
            let protocolFamily: FireWireProtocolFamily
            let mixerHint: String
            let supportStatus: FireWireProfileSupportStatus
            let integrationMode: FireWireDeviceIntegrationMode
            let flags: UInt32
            let source: String

            var id: String {{
                String(format: "%06X:%06X:%06X:%06X", vendorId, modelId, unitSpecifierId, unitVersion)
            }}

            var displayName: String {{
                [vendorName, modelName].filter {{ !$0.isEmpty }}.joined(separator: " ")
            }}

            var isBoundByASFW: Bool {{ supportStatus == .supportedBinding }}
            var requiresUnitMatch: Bool {{ (flags & FireWireAudioProfileFlags.requiresUnitMatch) != 0 }}
            var hasUnitEvidence: Bool {{ (flags & FireWireAudioProfileFlags.hasUnitEvidence) != 0 }}
        }}

        enum FireWireDeviceProfiles {{
            static let all: [FireWireAudioDeviceProfile] = [
{chr(10).join(rows)}
            ]

            static func normalize24(_ value: UInt32) -> UInt32 {{
                value & 0x00FF_FFFF
            }}

            static func lookup(vendorId: UInt32, modelId: UInt32) -> FireWireAudioDeviceProfile? {{
                let vendor = normalize24(vendorId)
                let model = normalize24(modelId)
                return all.first {{
                    normalize24($0.vendorId) == vendor &&
                    normalize24($0.modelId) == model &&
                    !$0.requiresUnitMatch
                }}
            }}

            static func lookup(vendorId: UInt32,
                               modelId: UInt32,
                               unitSpecifierId: UInt32,
                               unitVersion: UInt32) -> FireWireAudioDeviceProfile? {{
                let vendor = normalize24(vendorId)
                let model = normalize24(modelId)
                let spec = normalize24(unitSpecifierId)
                let version = normalize24(unitVersion)
                return all.first {{
                    normalize24($0.vendorId) == vendor &&
                    normalize24($0.modelId) == model &&
                    normalize24($0.unitSpecifierId) == spec &&
                    normalize24($0.unitVersion) == version
                }}
            }}

            static func bestMatch(for device: ASFWDriverConnector.FWDeviceInfo) -> FireWireAudioDeviceProfile? {{
                let weak = lookup(vendorId: device.vendorId, modelId: device.modelId)
                for unit in device.units {{
                    if let match = lookup(vendorId: device.vendorId,
                                          modelId: device.modelId,
                                          unitSpecifierId: unit.specId,
                                          unitVersion: unit.swVersion) {{
                        if match.supportStatus != .metadataOnly {{
                            return match
                        }}
                        if let weak, weak.supportStatus != .metadataOnly {{
                            return weak
                        }}
                        return match
                    }}
                }}
                return weak
            }}
        }}
        """)


def flag_value(flag: str) -> int:
    values = {
        "kFireWireAudioProfileFlagAudio": 1 << 0,
        "kFireWireAudioProfileFlagMidi": 1 << 1,
        "kFireWireAudioProfileFlagVideo": 1 << 2,
        "kFireWireAudioProfileFlagRequiresUnitMatch": 1 << 3,
        "kFireWireAudioProfileFlagHasUnitEvidence": 1 << 4,
    }
    return values[flag]


def main() -> int:
    print("Fetching FFADO configuration...", file=sys.stderr)
    ffado_text = fetch(FFADO_URL)
    print("Fetching systemd IEEE1394 hwdb...", file=sys.stderr)
    systemd_text = fetch(SYSTEMD_HWDB_URL)
    # Fetching this source keeps the generator's dependency visible even though
    # the current table generation uses FFADO/systemd as structured inputs.
    print("Checking am-config-roms reference...", file=sys.stderr)
    fetch(AM_CONFIG_ROMS_URL)

    profiles = merge_profiles(parse_ffado(ffado_text), parse_systemd(systemd_text))
    CPP_OUT.write_text(generate_cpp(profiles), encoding="utf-8")
    SWIFT_OUT.write_text(generate_swift(profiles), encoding="utf-8")
    print(f"Generated {len(profiles)} profiles", file=sys.stderr)
    print(CPP_OUT)
    print(SWIFT_OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
