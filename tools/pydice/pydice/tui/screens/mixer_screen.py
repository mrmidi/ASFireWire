"""Mixer matrix screen (read-only display)."""
from textual.app import ComposeResult
from textual.widgets import Label, Static, DataTable
from ...dummy_data import AppState, MIXER_INPUT_LABELS, MIXER_OUTPUT_LABELS


def _db_str(v) -> str:
    if v is None:
        return "[dim]-inf[/dim]"
    return f"[green]{v:+.0f}dB[/green]"


class MixerScreen(Static):
    """Displays mixer matrix: outputs (rows) × inputs (cols). Read-only."""

    DEFAULT_CSS = """
    MixerScreen {
        height: 1fr;
        padding: 1;
    }
    MixerScreen Label {
        color: $accent;
        text-style: bold;
        margin-bottom: 1;
    }
    MixerScreen DataTable {
        height: 1fr;
    }
    """

    def __init__(self, state: AppState, **kwargs):
        super().__init__(**kwargs)
        self._state = state

    def compose(self) -> ComposeResult:
        yield Label("Mixer Matrix  [dim](read-only)[/dim]")
        yield DataTable(id="mixer-table", zebra_stripes=True)

    def on_mount(self) -> None:
        table = self.query_one("#mixer-table", DataTable)
        table.cursor_type = "cell"
        table.add_columns("Out \\ In", *MIXER_INPUT_LABELS)
        for out_idx, out_label in enumerate(MIXER_OUTPUT_LABELS):
            row = [out_label]
            for inp_idx in range(len(MIXER_INPUT_LABELS)):
                row.append(_db_str(self._state.mixer[out_idx][inp_idx]))
            table.add_row(*row, key=str(out_idx))
