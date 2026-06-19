"""Routing matrix screen."""
from textual.app import ComposeResult
from textual.widgets import Label, Static, DataTable
from ...dummy_data import AppState, ROUTING_SOURCE_LABELS, ROUTING_DEST_LABELS


CONNECTED = "[bold green]•[/bold green]"
DISCONNECTED = "[dim]·[/dim]"


class RoutingScreen(Static):
    """Displays routing matrix: destinations (rows) × sources (cols)."""

    DEFAULT_CSS = """
    RoutingScreen {
        height: 1fr;
        padding: 1;
    }
    RoutingScreen Label {
        color: $accent;
        text-style: bold;
        margin-bottom: 1;
    }
    RoutingScreen DataTable {
        height: 1fr;
    }
    """

    def __init__(self, state: AppState, **kwargs):
        super().__init__(**kwargs)
        self._state = state

    def compose(self) -> ComposeResult:
        yield Label("Routing Matrix  [dim](arrow keys to navigate)[/dim]")
        yield DataTable(id="routing-table", zebra_stripes=True)

    def on_mount(self) -> None:
        table = self.query_one("#routing-table", DataTable)
        table.cursor_type = "cell"
        # Columns: "Dest \\ Src" header + source labels
        table.add_columns("Dest \\ Src", *ROUTING_SOURCE_LABELS)
        for dst_idx, dst_label in enumerate(ROUTING_DEST_LABELS):
            row = [dst_label]
            for src_idx in range(len(ROUTING_SOURCE_LABELS)):
                connected = self._state.routing[dst_idx][src_idx]
                row.append(CONNECTED if connected else DISCONNECTED)
            table.add_row(*row, key=str(dst_idx))
