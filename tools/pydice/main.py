"""Entry point for pydice TUI and CLI tools."""
import argparse
import json
import sys
from pathlib import Path


def _cmd_list_unknown(path: str) -> None:
    from pydice.protocol.log_parser import parse_log
    from pydice.protocol.dice_address_map import annotate

    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"Error reading {path}: {exc}", file=sys.stderr)
        sys.exit(1)

    from pydice.protocol.log_parser import parse_log
    events = parse_log(text)

    # Collect unknown addresses: annotate returns (region, hex) when not found
    unknown: dict[str, dict] = {}
    for ev in events:
        if not ev.address:
            continue
        parts = ev.address.split(".")
        region = ".".join(parts[1:]) if len(parts) >= 2 else ev.address
        name, _ = annotate(ev.address, ev.value)
        if name != region:
            continue  # known register or ConfigROM
        key = region
        if key not in unknown:
            unknown[key] = {
                "count": 0,
                "values": set(),
                "kinds": set(),
                "sizes": set(),
            }
        unknown[key]["count"] += 1
        unknown[key]["kinds"].add(ev.kind)
        if ev.value is not None:
            unknown[key]["values"].add(ev.value)
        if ev.size is not None:
            unknown[key]["sizes"].add(ev.size)

    if not unknown:
        print(f"No unknown addresses in {path}.")
        return

    print(f"Unknown addresses in {path}  ({len(unknown)} distinct)\n")
    header = f"{'Address':<22} {'Count':>5}  {'Kinds':<30}  Values / Sizes"
    print(header)
    print("-" * len(header))
    for region, info in sorted(unknown.items(), key=lambda x: -x[1]["count"]):
        kinds_str = ",".join(sorted(info["kinds"]))
        vals = sorted(info["values"])
        vals_str = " ".join(f"0x{v:08x}" for v in vals[:4])
        if len(vals) > 4:
            vals_str += " …"
        if info["sizes"]:
            sizes_str = " sizes=" + ",".join(str(s) for s in sorted(info["sizes"]))
            vals_str = (vals_str + sizes_str).strip()
        print(f"{region:<22} {info['count']:>5}  {kinds_str:<30}  {vals_str}")


def _cmd_compare_raw(orig_path: str, debug_path: str, ignore_config_rom: bool = False) -> None:
    from pydice.protocol.log_parser import parse_log
    from pydice.protocol.log_comparator import (
        compare_logs,
        describe_payload_difference,
        DiffStatus,
    )

    try:
        ref_text = Path(orig_path).read_text(encoding="utf-8", errors="replace")
        dbg_text = Path(debug_path).read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"Error reading file: {exc}", file=sys.stderr)
        sys.exit(1)

    ref_events = parse_log(ref_text)
    dbg_events = parse_log(dbg_text)

    diff_lines, summary = compare_logs(
        ref_events,
        dbg_events,
        ignore_config_rom=ignore_config_rom,
    )

    _STATUS_SYM = {
        DiffStatus.MATCH: "\u2713",
        DiffStatus.MISMATCH: "\u2717",
        DiffStatus.REF_ONLY: "\u25c1",
        DiffStatus.DEBUG_ONLY: "\u25b7",
    }

    orig_name = Path(orig_path).name
    dbg_name = Path(debug_path).name

    print(f"\u2550\u2550\u2550 pydice log comparison \u2550\u2550\u2550")
    print(f"Reference: {orig_name} \u2192 {summary['ref_ops']} init ops")
    print(f"Debug:     {dbg_name} \u2192 {summary['debug_ops']} init ops")
    if ignore_config_rom:
        print("Filter:    Config ROM accesses skipped")
    print()

    for dl in diff_lines:
        sym = _STATUS_SYM[dl.status]
        op = dl.ref_op or dl.debug_op
        assert op is not None

        addr_short = op.address.replace("ffff.", "") if op.address.startswith("ffff.") else op.address
        reg = op.register

        ref_val = ""
        dbg_val = ""
        if dl.ref_op:
            ref_val = dl.ref_op.decoded or (f"0x{dl.ref_op.value:08x}" if dl.ref_op.value is not None else "")
        if dl.debug_op:
            dbg_val = dl.debug_op.decoded or (f"0x{dl.debug_op.value:08x}" if dl.debug_op.value is not None else "")

        print(
            f"  {sym}   {addr_short:<14} {reg:<28} {op.raw_kind:<8} {op.direction}  "
            f"{ref_val:<20} {dbg_val}"
        )
        if (
            dl.status == DiffStatus.MISMATCH
            and dl.ref_op is not None
            and dl.debug_op is not None
        ):
            detail = describe_payload_difference(dl.ref_op, dl.debug_op)
            if detail:
                print(f"        {detail}")

    print()
    print("\u2550\u2550\u2550 Summary \u2550\u2550\u2550")
    print(
        f"  \u2713 {summary['match']} match"
        f"  \u2717 {summary['mismatch']} mismatch"
        f"  \u25c1 {summary['ref_only']} ref-only"
        f"  \u25b7 {summary['debug_only']} debug-only"
    )


def _cmd_compare_init(
    reference_path: str,
    current_path: str,
    output_format: str,
    show: str,
    strict_phase0: bool,
) -> None:
    from pydice.protocol.semantic_analysis import (
        load_and_compare_init,
        load_and_compare_init_strict_phase0,
        render_json_report,
        render_strict_phase0_json_report,
        render_strict_phase0_text_report,
        render_text_report,
    )

    try:
        comparison = (
            load_and_compare_init_strict_phase0(reference_path, current_path)
            if strict_phase0
            else load_and_compare_init(reference_path, current_path)
        )
    except OSError as exc:
        print(f"Error reading file: {exc}", file=sys.stderr)
        sys.exit(1)

    if output_format == "json":
        renderer = render_strict_phase0_json_report if strict_phase0 else render_json_report
        print(json.dumps(renderer(comparison), indent=2))
        return

    if strict_phase0:
        print(render_strict_phase0_text_report(comparison), end="")
        return

    sections = [part.strip() for part in show.split(",") if part.strip()]
    print(render_text_report(comparison, sections=sections), end="")


def _cmd_export_parity_md(
    log_path: str,
    ignore_config_rom: bool,
    style: str,
    out_dir: str,
) -> None:
    from pydice.protocol.parity_markdown import load_and_export_parity_markdown

    try:
        written = load_and_export_parity_markdown(
            log_path,
            out_dir,
            ignore_config_rom=ignore_config_rom,
            style=style,
        )
    except OSError as exc:
        print(f"Error reading file: {exc}", file=sys.stderr)
        sys.exit(1)

    print("Exported parity markdown:")
    for key in ("phases", "timeline"):
        path = written.get(key)
        if path is not None:
            print(f"- {key}: {path}")


def _cmd_export_parity_cpp(
    log_path: str,
    ignore_config_rom: bool,
    out: str,
) -> None:
    from pydice.protocol.parity_cpp_fixture import load_and_export_parity_cpp_fixture

    try:
        written = load_and_export_parity_cpp_fixture(
            log_path,
            out,
            ignore_config_rom=ignore_config_rom,
        )
    except OSError as exc:
        print(f"Error reading file: {exc}", file=sys.stderr)
        sys.exit(1)

    print(f"Exported parity C++ fixture: {written}")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="pydice",
        description="pydice — DICE/TCAT FireWire device tool",
    )
    subparsers = parser.add_subparsers(dest="command")
    default_parity_dir = str(Path(__file__).resolve().parent / "parity")
    default_parity_cpp = str(
        Path(__file__).resolve().parents[2] / "tests" / "ReferencePhase0ParityFixture.inc"
    )

    compare_init = subparsers.add_parser(
        "compare-init",
        help="Semantic init comparison for two FireWire logs",
    )
    compare_init.add_argument("reference", metavar="REFERENCE")
    compare_init.add_argument("current", metavar="CURRENT")
    compare_init.add_argument(
        "--format",
        choices=("text", "json"),
        default="text",
        help="Output format for semantic comparison",
    )
    compare_init.add_argument(
        "--show",
        default="findings,phases,state",
        help="Comma-separated sections for text output",
    )
    compare_init.add_argument(
        "--strict-phase0",
        action="store_true",
        help="Enforce the strict phase-0 control-plane contract and stop on the first mismatch",
    )

    compare_raw = subparsers.add_parser(
        "compare-raw",
        help="Raw register diff for two FireWire logs",
    )
    compare_raw.add_argument("reference", metavar="REFERENCE")
    compare_raw.add_argument("current", metavar="CURRENT")
    compare_raw.add_argument(
        "--ignore-config-rom",
        action="store_true",
        help="Skip Config ROM accesses in the raw diff",
    )

    export_parity_md = subparsers.add_parser(
        "export-parity-md",
        help="Export a compact phase-0 startup parity checklist as Markdown",
    )
    export_parity_md.add_argument("log", metavar="LOG")
    export_parity_md.add_argument(
        "--ignore-config-rom",
        action="store_true",
        help="Skip Config ROM accesses in the exported checklist",
    )
    export_parity_md.add_argument(
        "--style",
        choices=("phases", "timeline", "both"),
        default="both",
        help="Markdown layout to export",
    )
    export_parity_md.add_argument(
        "--out-dir",
        default=default_parity_dir,
        help="Directory to write exported Markdown files",
    )

    export_parity_cpp = subparsers.add_parser(
        "export-parity-cpp",
        help="Export a phase-0 reference trace as a C++ fixture include",
    )
    export_parity_cpp.add_argument("log", metavar="LOG")
    export_parity_cpp.add_argument(
        "--ignore-config-rom",
        action="store_true",
        help="Skip Config ROM accesses in the exported fixture",
    )
    export_parity_cpp.add_argument(
        "--out",
        default=default_parity_cpp,
        help="Path to write the generated C++ fixture include",
    )

    list_unknown = subparsers.add_parser(
        "list-unknown",
        help="List addresses not yet in the register map",
    )
    list_unknown.add_argument("log", metavar="LOG")

    parser.add_argument("--file", metavar="PATH", help="Path to FireBug log file")
    parser.add_argument(
        "--list-unknown",
        action="store_true",
        help="List addresses not yet in the register map (requires --file)",
    )
    parser.add_argument(
        "--compare",
        action="store_true",
        help="Compare two FireBug init logs (requires --orig and --debug)",
    )
    parser.add_argument("--orig", metavar="PATH", help="Reference log file for --compare")
    parser.add_argument("--debug", metavar="PATH", help="Debug log file for --compare")
    return parser


def main() -> None:
    parser = _build_parser()
    args = parser.parse_args()

    if args.command == "list-unknown":
        _cmd_list_unknown(args.log)
        return

    if args.command == "compare-raw":
        _cmd_compare_raw(args.reference, args.current, args.ignore_config_rom)
        return

    if args.command == "compare-init":
        _cmd_compare_init(args.reference, args.current, args.format, args.show, args.strict_phase0)
        return

    if args.command == "export-parity-md":
        _cmd_export_parity_md(args.log, args.ignore_config_rom, args.style, args.out_dir)
        return

    if args.command == "export-parity-cpp":
        _cmd_export_parity_cpp(args.log, args.ignore_config_rom, args.out)
        return

    if args.list_unknown:
        if not args.file:
            parser.error("--list-unknown requires --file <path>")
        _cmd_list_unknown(args.file)
        return

    if args.compare:
        if not args.orig or not args.debug:
            parser.error("--compare requires --orig <path> and --debug <path>")
        _cmd_compare_raw(args.orig, args.debug)
        return

    # Default: launch TUI (optionally pre-load a file — future use)
    from pydice.tui.app import PyDiceApp
    app = PyDiceApp()
    app.run()


if __name__ == "__main__":
    main()
