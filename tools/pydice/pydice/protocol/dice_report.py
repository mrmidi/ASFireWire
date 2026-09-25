"""Parse an "ASFW DICE DEVICE REPORT" into a register image, and export images as C++.

The report is the read-only dump written by the ASFW app
(`ASFW/ViewModels/DiceReportTextFormatter.swift`) and kept under
`documentation/fixtures/`. It prints every DICE general-space register decoded;
this module turns it back into the values a device would answer with, so host
tests can simulate the recorded device (`tests/support/SimulatedDiceDevice.hpp`).

Only the general space is reconstructed (section table, GLOBAL, TX, RX, EXT_SYNC).
The TCAT extension is kept only for its per-rate-mode stream formats, which
drive the simulated geometry change on a rate-mode switch.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import re

# DICE speed register encoding (TX_SPEED): S100=0, S200=1, S400=2, S800=3.
SPEED_CODES = {"S100": 0, "S200": 1, "S400": 2, "S800": 3}

# EXT_SYNC ADAT_USER_DATA: bit 4 set means "no data"; low nibble is the data.
ADAT_NO_DATA = 0x10

# Rate-mode headers in the EAP current-config block, in register order.
RATE_MODES = ("low", "middle", "high")

MAX_STREAMS = 4  # ASFW kMaxAudioStreamsPerDirection


@dataclass(frozen=True)
class Section:
    offset_quadlets: int
    size_quadlets: int


@dataclass(frozen=True)
class Stream:
    iso: int  # signed; -1 = disabled
    pcm: int
    midi: int
    speed: int | None  # TX only
    seq_start: int | None  # RX only
    names: tuple[str, ...]


@dataclass(frozen=True)
class StreamShape:
    pcm: int
    midi: int
    names: tuple[str, ...]


@dataclass(frozen=True)
class RateModeFormat:
    tx: tuple[StreamShape, ...]
    rx: tuple[StreamShape, ...]


@dataclass(frozen=True)
class ExtSync:
    clock_source: int
    locked: bool
    rate_index: int
    adat_user_data: int  # raw register value


@dataclass
class DiceDeviceImage:
    source: str
    guid: int = 0
    vendor: str = ""
    model: str = ""
    tcat_vendor: int = 0
    tcat_category: int = 0
    tcat_product: int = 0
    tcat_serial: int = 0
    sections: dict[str, Section] = field(default_factory=dict)
    has_extension: bool = False
    owner: int = 0
    notification: int = 0
    nickname: str = ""
    clock_select: int = 0
    enable: int = 0
    status: int = 0
    ext_status: int = 0
    sample_rate: int = 0
    version: int = 0
    clock_caps: int = 0
    clock_source_names: tuple[str, ...] = ()
    tx_entry_quadlets: int = 0
    rx_entry_quadlets: int = 0
    tx_streams: list[Stream] = field(default_factory=list)
    rx_streams: list[Stream] = field(default_factory=list)
    ext_sync: ExtSync | None = None
    rate_modes: dict[str, RateModeFormat] = field(default_factory=dict)


_HEX = r"0x[0-9A-Fa-f]+"


def _int(text: str) -> int:
    return int(text, 0)


def _quoted(line: str) -> str:
    match = re.search(r"'(.*)'", line)
    if match is None:
        raise ValueError(f"expected a quoted string in: {line!r}")
    return match.group(1)


def parse_dice_report(text: str, source: str = "") -> DiceDeviceImage:
    """Parse one report. Raises ValueError on anything it cannot place."""
    image = DiceDeviceImage(source=source)
    block = ""
    space = ""
    in_names: list[str] | None = None
    in_source_names = False
    stream: dict | None = None
    streams_tx: list[dict] = []
    streams_rx: list[dict] = []
    ext: dict = {}
    rate_mode = ""
    rate_formats: dict[str, dict[str, list[StreamShape]]] = {}
    pending_shape: tuple[str, int, int] | None = None

    def flush_stream() -> None:
        nonlocal stream
        if stream is None:
            return
        target = streams_tx if block == "TX" else streams_rx
        target.append(stream)
        stream = None

    for raw in text.splitlines():
        line = raw.rstrip()
        stripped = line.strip()

        # Top-level block headers (unindented, followed by an underline).
        header = {
            "IDENTITY": "IDENTITY",
            "GLOBAL": "GLOBAL",
            "EXT_SYNC": "EXT_SYNC",
            "NOTES": "NOTES",
        }.get(stripped)
        if header is None and not line.startswith(" "):
            if stripped.startswith("SECTION TABLES"):
                header = "SECTIONS"
            elif stripped.startswith("TX STREAMS"):
                header = "TX"
            elif stripped.startswith("RX STREAMS"):
                header = "RX"
            elif stripped.startswith("EAP CURRENT CONFIG"):
                header = "EAP_FORMATS"
            elif stripped.startswith("EAP "):
                header = "EAP_OTHER"
        if header is not None:
            flush_stream()
            in_names = None
            in_source_names = False
            block = header
            continue
        if not stripped or set(stripped) <= {"-", "="}:
            continue

        if block == "IDENTITY":
            key, _, value = stripped.partition(":")
            value = value.strip()
            if key == "GUID":
                image.guid = _int(value)
            elif key == "Vendor":
                image.vendor = value
            elif key == "Model":
                image.model = value
            elif key == "TCAT vendor":
                image.tcat_vendor = _int(value)
            elif key == "TCAT category":
                image.tcat_category = _int(value.split()[0])
            elif key == "TCAT product":
                image.tcat_product = _int(value)
            elif key == "TCAT serial":
                image.tcat_serial = _int(value)
            continue

        if block == "SECTIONS":
            if stripped.startswith("general space"):
                space = "general"
                continue
            if stripped.startswith("extension space"):
                space = "extension"
                continue
            if stripped == "<absent>":
                continue
            match = re.match(rf"(\w+)\s+offset=({_HEX}).*size=({_HEX})", stripped)
            if match is None:
                raise ValueError(f"{source}: unparsed section line: {stripped!r}")
            if space == "general":
                image.sections[match.group(1)] = Section(
                    _int(match.group(2)), _int(match.group(3)))
            else:
                image.has_extension = True
            continue

        if block == "GLOBAL":
            if in_source_names:
                match = re.match(r"(\d+)\s+\w+\s+'(.*)'", stripped)
                if match is not None:
                    image.clock_source_names += (match.group(2),)
                    continue
                in_source_names = False
            name, _, value = stripped.partition("=")
            name = name.strip()
            value = value.strip()
            if stripped.startswith("CLOCK_SOURCE_NAMES"):
                in_source_names = True
            elif name == "OWNER":
                image.owner = _int(value.split()[0])
            elif name == "NOTIFICATION":
                image.notification = _int(value.split()[0])
            elif name == "NICK_NAME":
                image.nickname = _quoted(value)
            elif name == "CLOCK_SELECT":
                image.clock_select = _int(value.split()[0])
            elif name == "ENABLE":
                image.enable = _int(value.split()[0])
            elif name == "STATUS":
                image.status = _int(value.split()[0])
            elif name == "EXTENDED_STATUS":
                image.ext_status = _int(value.split()[0])
            elif name == "SAMPLE_RATE":
                image.sample_rate = _int(value.split()[0])
            elif name == "VERSION":
                image.version = _int(value.split()[0])
            elif name == "CLOCK_CAPABILITIES":
                image.clock_caps = _int(value.split()[0])
            continue

        if block in ("TX", "RX"):
            match = re.match(r"NUMBER = (\d+)\s+SIZE = (\d+) quadlets", stripped)
            if match is not None:
                if block == "TX":
                    image.tx_entry_quadlets = _int(match.group(2))
                else:
                    image.rx_entry_quadlets = _int(match.group(2))
                continue
            if re.match(r"\[stream \d+\]", stripped):
                flush_stream()
                in_names = None
                stream = {"names": [], "speed": None, "seq_start": None}
                continue
            if stream is None:
                raise ValueError(f"{source}: stream field outside a stream: {stripped!r}")
            if stripped == "NAMES:":
                in_names = stream["names"]
                continue
            if in_names is not None:
                in_names.append(_quoted(stripped))
                continue
            name, _, value = stripped.partition("=")
            name = name.strip()
            value = value.strip()
            if name == "ISOCHRONOUS":
                stream["iso"] = _int(value.split()[0])
            elif name == "PCM channels":
                stream["pcm"] = _int(value)
            elif name == "MIDI ports":
                stream["midi"] = _int(value)
            elif name == "SPEED":
                stream["speed"] = SPEED_CODES[value]
            elif name == "SEQ_START":
                stream["seq_start"] = _int(value)
            continue

        if block == "EXT_SYNC":
            if stripped == "<absent>":
                continue
            name, _, value = stripped.partition("=")
            ext[name.strip()] = value.strip()
            continue

        if block == "EAP_FORMATS":
            match = re.match(r"\[(\w+) \(", stripped)
            if match is not None:
                rate_mode = match.group(1)
                rate_formats[rate_mode] = {"tx": [], "rx": []}
                continue
            match = re.match(r"(tx|rx) \d+: pcm=(\d+) midi=(\d+)", stripped)
            if match is not None:
                pending_shape = (match.group(1), _int(match.group(2)), _int(match.group(3)))
                continue
            if stripped.startswith("names:") and pending_shape is not None:
                direction, pcm, midi = pending_shape
                names = tuple(n.strip() for n in stripped[len("names:"):].split(","))
                rate_formats[rate_mode][direction].append(StreamShape(pcm, midi, names))
                pending_shape = None
            continue

    flush_stream()

    def build(entries: list[dict]) -> list[Stream]:
        return [Stream(iso=e["iso"], pcm=e["pcm"], midi=e["midi"], speed=e["speed"],
                       seq_start=e["seq_start"], names=tuple(e["names"]))
                for e in entries]

    image.tx_streams = build(streams_tx)
    image.rx_streams = build(streams_rx)
    if ext:
        adat = ext.get("ADAT_USER_DATA", "no-data")
        image.ext_sync = ExtSync(
            clock_source=_int(ext["CLOCK_SOURCE"].split()[0]),
            locked=ext["LOCKED"] == "yes",
            rate_index=_int(ext["RATE"].split()[0]),
            adat_user_data=ADAT_NO_DATA if adat == "no-data" else _int(adat),
        )
    image.rate_modes = {
        mode: RateModeFormat(tuple(f["tx"]), tuple(f["rx"]))
        for mode, f in rate_formats.items()
    }
    _validate(image)
    return image


def _validate(image: DiceDeviceImage) -> None:
    where = image.source or "report"
    for required in ("global", "tx", "rx"):
        if required not in image.sections:
            raise ValueError(f"{where}: missing {required} section")
    if not image.tx_streams or not image.rx_streams:
        raise ValueError(f"{where}: no TX or RX streams")
    if len(image.tx_streams) > MAX_STREAMS or len(image.rx_streams) > MAX_STREAMS:
        raise ValueError(f"{where}: more than {MAX_STREAMS} streams in one direction")
    if len(image.clock_source_names) != 13:
        raise ValueError(f"{where}: expected 13 clock source names, "
                         f"got {len(image.clock_source_names)}")


def load_dice_report(path: str | Path) -> DiceDeviceImage:
    path = Path(path)
    return parse_dice_report(path.read_text(encoding="utf-8"), source=path.name)


# --------------------------------------------------------------------------- #
# C++ export
# --------------------------------------------------------------------------- #

def _cpp_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def _label_block(names: tuple[str, ...]) -> str:
    """DICE label block text: names joined by '\\', terminated by '\\\\'."""
    return "\\".join(names) + "\\\\"


def _cpp_identifier(key: str) -> str:
    return "k" + "".join(part[:1].upper() + part[1:] for part in re.split(r"[^A-Za-z0-9]+", key) if part)


def _render_stream(stream: Stream, is_tx: bool) -> str:
    extra = stream.speed if is_tx else stream.seq_start
    return (f"{{.iso = {stream.iso}, .pcm = {stream.pcm}U, .midi = {stream.midi}U, "
            f".speedOrSeqStart = {extra or 0}U, .names = {_cpp_string(_label_block(stream.names))}}}")


def _render_shapes(shapes: tuple[StreamShape, ...]) -> str:
    inner = ", ".join(
        f"{{.pcm = {s.pcm}U, .midi = {s.midi}U, .names = {_cpp_string(_label_block(s.names))}}}"
        for s in shapes)
    return f".count = {len(shapes)}U, .streams = {{{{{inner}}}}}"


def render_dice_images_cpp(images: list[tuple[str, DiceDeviceImage]]) -> str:
    """Render `(key, image)` pairs as the body of tests/support/DiceDeviceImages.inc."""
    lines = [
        "// Generated by `pydice export-dice-images-cpp` from documentation/fixtures.",
        "// Do not edit by hand: regenerate from the dumps instead.",
        "// Types are declared in DiceDeviceImage.hpp, which includes this file.",
        "",
        "namespace DiceDeviceImages {",
        "",
    ]
    identifiers = []
    for key, image in images:
        ident = _cpp_identifier(key)
        identifiers.append(ident)
        sec = image.sections
        ext_sync = sec.get("ext_sync", Section(0, 0))
        lines.append(f"// {image.vendor} {image.model} — {image.source}")
        lines.append(f"inline constexpr DiceDeviceImage {ident}{{")
        lines.append(f"    .key = {_cpp_string(key)},")
        lines.append(f"    .reportedModel = {_cpp_string(f'{image.vendor} {image.model}')},")
        lines.append(f"    .source = {_cpp_string(image.source)},")
        lines.append(f"    .guid = 0x{image.guid:016X}ULL,")
        for field_name, section in (("globalSection", sec["global"]), ("txSection", sec["tx"]),
                                    ("rxSection", sec["rx"]), ("extSyncSection", ext_sync)):
            lines.append(f"    .{field_name} = {{.offsetQuadlets = {section.offset_quadlets}U, "
                         f".sizeQuadlets = {section.size_quadlets}U}},")
        lines.append(f"    .hasExtension = {'true' if image.has_extension else 'false'},")
        lines.append(f"    .owner = 0x{image.owner:016X}ULL,")
        lines.append(f"    .notification = 0x{image.notification:08X}U,")
        lines.append(f"    .nickname = {_cpp_string(image.nickname)},")
        lines.append(f"    .clockSelect = 0x{image.clock_select:08X}U,")
        lines.append(f"    .enable = {image.enable}U,")
        lines.append(f"    .status = 0x{image.status:08X}U,")
        lines.append(f"    .extStatus = 0x{image.ext_status:08X}U,")
        lines.append(f"    .sampleRate = {image.sample_rate}U,")
        lines.append(f"    .version = 0x{image.version:08X}U,")
        lines.append(f"    .clockCaps = 0x{image.clock_caps:08X}U,")
        lines.append(f"    .clockSourceNames = {_cpp_string(_label_block(image.clock_source_names))},")
        lines.append(f"    .txEntryQuadlets = {image.tx_entry_quadlets}U,")
        lines.append(f"    .rxEntryQuadlets = {image.rx_entry_quadlets}U,")
        tx = ", ".join(_render_stream(s, True) for s in image.tx_streams)
        rx = ", ".join(_render_stream(s, False) for s in image.rx_streams)
        lines.append(f"    .txCount = {len(image.tx_streams)}U,")
        lines.append(f"    .tx = {{{{{tx}}}}},")
        lines.append(f"    .rxCount = {len(image.rx_streams)}U,")
        lines.append(f"    .rx = {{{{{rx}}}}},")
        if image.ext_sync is not None:
            e = image.ext_sync
            lines.append(f"    .hasExtSync = true,")
            lines.append(f"    .extSync = {{.clockSource = {e.clock_source}U, "
                         f".locked = {1 if e.locked else 0}U, .rateIndex = {e.rate_index}U, "
                         f".adatUserData = 0x{e.adat_user_data:02X}U}},")
        else:
            lines.append("    .hasExtSync = false,")
            lines.append("    .extSync = {},")
        if image.rate_modes:
            lines.append("    .hasRateModeFormats = true,")
            modes = []
            for mode in RATE_MODES:
                fmt = image.rate_modes.get(mode, RateModeFormat((), ()))
                modes.append(f"{{.tx = {{{_render_shapes(fmt.tx)}}}, .rx = {{{_render_shapes(fmt.rx)}}}}}")
            lines.append(f"    .rateModes = {{{{{', '.join(modes)}}}}},")
        else:
            lines.append("    .hasRateModeFormats = false,")
            lines.append("    .rateModes = {},")
        lines.append("};")
        lines.append("")
    lines.append(f"inline constexpr std::array<const DiceDeviceImage*, {len(identifiers)}> kAll{{{{")
    for ident in identifiers:
        lines.append(f"    &{ident},")
    lines.append("}};")
    lines.append("")
    lines.append("} // namespace DiceDeviceImages")
    lines.append("")
    return "\n".join(lines)


# Stable export order and keys: tests refer to devices by these keys.
DEFAULT_REPORTS = (
    ("saffire-pro24-dsp", "documentation/fixtures/DICE/spro24dsp.txt"),
    ("venice-f24", "documentation/fixtures/DICE/midasF24.txt"),
    ("venice-f32", "documentation/fixtures/DICE/midasF32.txt"),
    ("studiolive-2442", "documentation/fixtures/DICE/presonus2442.txt"),
    ("multimix", "documentation/fixtures/alesismultimix.txt"),
)


def export_dice_images_cpp(repo_root: str | Path, out: str | Path) -> Path:
    repo_root = Path(repo_root)
    images = [(key, load_dice_report(repo_root / rel)) for key, rel in DEFAULT_REPORTS]
    target = Path(out)
    target.write_text(render_dice_images_cpp(images), encoding="utf-8")
    return target
