"""Output channel volume/mute screen."""
from textual.app import ComposeResult
from textual.screen import Screen
from textual.widgets import DataTable, Label, Static
from textual.reactive import reactive
from ...dummy_data import AppState
from ...protocol.focusrite.spro24dsp import command_for_volume, command_for_mute


class OutputScreen(Static):
    """Displays 6 output channels with volume and mute controls."""

    DEFAULT_CSS = """
    OutputScreen {
        height: 1fr;
        padding: 1;
    }
    OutputScreen Label {
        color: $accent;
        text-style: bold;
        margin-bottom: 1;
    }
    OutputScreen DataTable {
        height: 1fr;
    }
    """

    def __init__(self, state: AppState, **kwargs):
        super().__init__(**kwargs)
        self._state = state

    def compose(self) -> ComposeResult:
        yield Label("Output Channels")
        yield DataTable(id="output-table")

    def on_mount(self) -> None:
        table = self.query_one("#output-table", DataTable)
        table.add_columns("Channel", "Volume (0-127)", "Muted")
        self._refresh_table()

    def _refresh_table(self) -> None:
        table = self.query_one("#output-table", DataTable)
        table.clear()
        for i, ch in enumerate(self._state.output_group.channels):
            table.add_row(
                f"Analog Out {i + 1}",
                str(ch.volume),
                "Yes" if ch.muted else "No",
                key=str(i),
            )

    def get_commands(self):
        cmds = []
        for i, ch in enumerate(self._state.output_group.channels):
            cmds.append(command_for_volume(i, ch.volume))
            if ch.muted:
                cmds.append(command_for_mute(i, ch.muted))
        return cmds
