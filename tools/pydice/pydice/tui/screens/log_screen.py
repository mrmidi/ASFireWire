"""FireBug log analyzer tab for the pydice TUI."""
import csv
import io
from datetime import datetime
from pathlib import Path

from rich.text import Text
from textual import on
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.widgets import Button, DataTable, Input, Label, RichLog, Select, Static

from ...protocol.dice_address_map import annotate
from ...protocol.log_parser import LogEvent, parse_log
from ...protocol.payload_decoder import decode_payload

_FILTER_OPTIONS: list[tuple[str, str]] = [
    ("All", "All"),
    ("Qwrite", "Qwrite"),
    ("Qread", "Qread"),
    ("Block (Bread/BRresp/Bwrite)", "Block"),
    ("BusReset", "BusReset"),
    ("Isoch (e020 addresses)", "Isoch"),
]

_KIND_STYLE: dict[str, str] = {
    "BusReset":   "bold red",
    "Qwrite":     "yellow",
    "BRresp":     "cyan",
    "Bwrite":     "cyan",
    "QRresp":     "green",
    "LockRq":     "magenta",
    "LockResp":   "magenta",
    "SelfID":     "dim",
    "CycleStart": "dim",
    "PHYResume":  "dim",
}


class LogScreen(Static):
    """FireBug 2.3 packet analyzer log viewer."""

    BINDINGS = [
        Binding("shift+enter", "export_from_selection", "Export TSV from selection"),
    ]

    DEFAULT_CSS = """
    LogScreen {
        height: 1fr;
    }
    #log-main {
        height: 1fr;
    }
    #log-left {
        width: 3fr;
        height: 1fr;
        padding: 0 1;
    }
    #log-right {
        width: 1fr;
        height: 1fr;
        border-left: tall $panel;
        padding: 0 1;
    }
    LogScreen Label {
        color: $accent;
        text-style: bold;
        height: auto;
        margin-bottom: 1;
    }
    #log-toolbar {
        height: auto;
        padding: 0;
    }
    #log-filter-row {
        height: auto;
        padding: 0;
        margin-top: 1;
    }
    #log-path {
        width: 3fr;
    }
    #log-load-btn {
        width: auto;
        min-width: 8;
    }
    #log-filter {
        width: 2fr;
    }
    #log-search {
        width: 2fr;
    }
    #log-table {
        height: 1fr;
    }
    #log-status {
        height: 1;
        background: $panel;
        padding: 0 1;
    }
    #log-details {
        height: 1fr;
    }
    """

    def __init__(self) -> None:
        super().__init__()
        self._events: list[LogEvent] = []
        self._visible: list[LogEvent] = []
        self._filter_kind: str = "All"
        self._search: str = ""

    def compose(self) -> ComposeResult:
        default_path = str(Path.cwd() / "saffire-init.txt")
        with Horizontal(id="log-main"):
            with Vertical(id="log-left"):
                yield Label("FireBug Log Analyzer")
                with Horizontal(id="log-toolbar"):
                    yield Input(
                        value=default_path,
                        placeholder="Path to .txt log file...",
                        id="log-path",
                    )
                    yield Button("Load", id="log-load-btn", variant="primary")
                with Horizontal(id="log-filter-row"):
                    yield Select(_FILTER_OPTIONS, value="All", id="log-filter")
                    yield Input(placeholder="Search address / register...", id="log-search")
                yield DataTable(id="log-table", zebra_stripes=True, cursor_type="row")
                yield Static("Load a log file to begin.", id="log-status")
            with Vertical(id="log-right"):
                yield Label("\u2500\u2500 DETAILS \u2500\u2500")
                yield RichLog(id="log-details", markup=False, highlight=False)

    def on_mount(self) -> None:
        table = self.query_one(DataTable)
        table.add_columns("Time", "Type", "Src\u2192Dst", "Address", "Register", "Value / Size")

    # ── event handlers ────────────────────────────────────────────────────────

    @on(Button.Pressed, "#log-load-btn")
    def _load(self) -> None:
        path = self.query_one("#log-path", Input).value.strip()
        try:
            text = Path(path).read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            self.query_one("#log-status", Static).update(f"[red]Error: {exc}[/red]")
            return
        self._events = parse_log(text)
        self._populate_table()

    @on(Select.Changed, "#log-filter")
    def _filter_changed(self, event: Select.Changed) -> None:
        if event.value is not Select.BLANK:
            self._filter_kind = str(event.value)
            self._populate_table()

    @on(Input.Changed, "#log-search")
    def _search_changed(self, event: Input.Changed) -> None:
        self._search = event.value.lower().strip()
        self._populate_table()

    @on(DataTable.RowHighlighted, "#log-table")
    def _row_highlighted(self, event: DataTable.RowHighlighted) -> None:
        if event.row_key is None:
            return
        try:
            idx = int(event.row_key.value)
        except (ValueError, AttributeError):
            return
        if 0 <= idx < len(self._visible):
            self._show_details(self._visible[idx])

    # ── export ────────────────────────────────────────────────────────────────

    def action_export_from_selection(self) -> None:
        table = self.query_one(DataTable)
        start = table.cursor_coordinate.row if self._visible else 0
        self._export_tsv(start)

    def _export_tsv(self, start_idx: int) -> None:
        events = self._visible[start_idx:]
        if not events:
            self.query_one("#log-status", Static).update("[yellow]Nothing to export.[/yellow]")
            return

        src_path = Path(self.query_one("#log-path", Input).value.strip())
        out_path = src_path.with_name(src_path.stem + "_export.tsv")

        buf = io.StringIO()
        buf.write(f"# pydice export — rows {start_idx}–{start_idx + len(events) - 1}"
                  f" of {len(self._visible)}\n")
        buf.write(f"# source: {src_path}\n")
        buf.write(f"# generated: {datetime.now().isoformat(timespec='seconds')}\n")

        writer = csv.writer(buf, delimiter="\t", lineterminator="\n")
        writer.writerow([
            "timestamp", "kind", "src", "dst", "address",
            "register", "value", "size", "tLabel", "ack", "speed", "rcode", "payload",
        ])

        for ev in events:
            name, decoded = annotate(ev.address, ev.value)
            if ev.value is not None:
                val_str = decoded if decoded else f"0x{ev.value:08x}"
            elif ev.rcode is not None:
                val_str = f"rCode={ev.rcode}"
            else:
                val_str = ""

            payload_str = ""
            if ev.payload:
                payload_str = " | ".join(decode_payload(ev.address, ev.payload, ev.size))

            writer.writerow([
                ev.timestamp,
                ev.kind,
                ev.src or "",
                ev.dst or "",
                ev.address or "",
                name,
                val_str,
                str(ev.size) if ev.size is not None else "",
                str(ev.tLabel) if ev.tLabel is not None else "",
                str(ev.ack) if ev.ack is not None else "",
                ev.speed or "",
                str(ev.rcode) if ev.rcode is not None else "",
                payload_str,
            ])

        out_path.write_text(buf.getvalue(), encoding="utf-8")
        self.query_one("#log-status", Static).update(
            f"[green]Exported {len(events)} rows → {out_path.name}[/green]"
        )

    # ── table population ──────────────────────────────────────────────────────

    def _populate_table(self) -> None:
        table = self.query_one(DataTable)
        table.clear()

        self._visible = self._filtered_events()
        for i, ev in enumerate(self._visible):
            table.add_row(*self._make_row(ev), key=str(i))

        self._update_status()

    def _make_row(self, ev: LogEvent) -> tuple[Text, ...]:
        style = _KIND_STYLE.get(ev.kind, "")

        name, decoded = annotate(ev.address, ev.value)

        src_dst = f"{ev.src}\u2192{ev.dst}" if ev.src or ev.dst else ""
        addr_short = _short_address(ev.address)

        if ev.value is not None:
            val_str = decoded if decoded else f"0x{ev.value:08x}"
        elif ev.size is not None:
            val_str = f"{ev.size}B"
            if ev.payload:
                val_str += " + payload"
        elif ev.rcode is not None:
            val_str = f"rCode={ev.rcode}"
        else:
            val_str = ""

        cells = (ev.timestamp, ev.kind, src_dst, addr_short, name, val_str)
        if style:
            return tuple(Text(c, style=style) for c in cells)
        return tuple(Text(c) for c in cells)

    def _show_details(self, ev: LogEvent) -> None:
        details = self.query_one("#log-details", RichLog)
        details.clear()

        src_dst = f"{ev.src}\u2192{ev.dst}" if ev.src or ev.dst else ""
        header = Text()
        kind_style = _KIND_STYLE.get(ev.kind, "bold")
        header.append(ev.kind, style=kind_style)
        header.append(f"  {ev.timestamp}  {src_dst}", style="dim")
        details.write(header)

        name, decoded = annotate(ev.address, ev.value)
        addr_short = _short_address(ev.address)
        if addr_short:
            t = Text()
            t.append("Addr: ", style="bold")
            t.append(addr_short, style="cyan")
            if name:
                t.append(f"  {name}")
            details.write(t)

        if decoded:
            t = Text()
            t.append("Value: ", style="bold")
            t.append(decoded)
            details.write(t)

        if ev.size is not None:
            t = Text()
            t.append("Size: ", style="bold")
            t.append(f"{ev.size}B")
            details.write(t)

        if ev.payload:
            details.write(Text("\u2500\u2500 Payload \u2500\u2500", style="dim"))
            lines = decode_payload(ev.address, ev.payload, ev.size)
            for line in lines:
                details.write(Text(line))

        meta: list[str] = []
        if ev.tLabel is not None:
            meta.append(f"tLabel={ev.tLabel}")
        if ev.ack is not None:
            meta.append(f"ack={ev.ack}")
        if ev.speed is not None:
            meta.append(f"speed={ev.speed}")
        if ev.rcode is not None:
            meta.append(f"rCode={ev.rcode}")
        if meta:
            details.write(Text("  ".join(meta), style="dim"))

    def _filtered_events(self) -> list[LogEvent]:
        result = self._events

        if self._filter_kind != "All":
            if self._filter_kind == "Block":
                result = [e for e in result if e.kind in ("Bread", "BRresp", "Bwrite")]
            elif self._filter_kind == "Isoch":
                result = [e for e in result if e.address and "e020" in e.address]
            else:
                result = [e for e in result if e.kind == self._filter_kind]

        if self._search:
            s = self._search
            result = [
                e for e in result
                if (e.address and s in e.address.lower())
                or s in e.kind.lower()
                or s in e.src.lower()
                or s in e.dst.lower()
            ]

        return result

    def _update_status(self) -> None:
        kinds: dict[str, int] = {}
        for ev in self._events:
            kinds[ev.kind] = kinds.get(ev.kind, 0) + 1

        total = len(self._events)
        if total == 0:
            self.query_one("#log-status", Static).update("No events found.")
            return

        parts = [f"{total} events"]
        for kind in ("Qwrite", "Qread", "QRresp", "WrResp", "Bread", "BRresp",
                     "Bwrite", "LockRq", "BusReset"):
            n = kinds.get(kind, 0)
            if n:
                parts.append(f"{n} {kind}")
        self.query_one("#log-status", Static).update(" | ".join(parts))


def _short_address(address: str | None) -> str:
    """Return last two dot-segments of a dotted 48-bit address, e.g. 'e000.0074'."""
    if not address:
        return ""
    parts = address.split(".")
    if len(parts) >= 4:
        return ".".join(parts[-2:])
    return address
