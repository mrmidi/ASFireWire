"""Command log panel widget."""
from textual.app import ComposeResult
from textual.widgets import RichLog, Static
from textual.reactive import reactive
from rich.text import Text


class CommandPanel(Static):
    """A panel that displays a scrollable log of FireWire commands."""

    DEFAULT_CSS = """
    CommandPanel {
        width: 1fr;
        height: 100%;
        border: solid $accent;
        padding: 0 1;
    }
    CommandPanel #title {
        text-style: bold;
        color: $accent;
        padding: 0 0 1 0;
    }
    CommandPanel RichLog {
        height: 1fr;
        border: none;
    }
    """

    def compose(self) -> ComposeResult:
        yield Static("COMMAND LOG  [dim](c=clear)[/dim]", id="title")
        yield RichLog(id="log", wrap=False, highlight=False, markup=True)

    def on_key(self, event) -> None:
        if event.key == "c":
            self.clear()

    def clear(self) -> None:
        self.query_one("#log", RichLog).clear()

    def log_command(self, description: str, app_offset: int, value: int, sw_notice: int) -> None:
        log = self.query_one("#log", RichLog)
        addr = 0xFFFFF0000000 + 0x6DD4 + app_offset
        log.write(
            f"[bold yellow]WRITE[/bold yellow] [cyan]{addr:#016x}[/cyan]\n"
            f"  [green]{description}[/green]\n"
            f"  val: [white]{value:#010x}[/white]  ntc: [dim]{sw_notice:#010x}[/dim]"
        )

    def log_commands(self, commands) -> None:
        for cmd in commands:
            self.log_command(cmd.description, cmd.app_offset, cmd.value, cmd.sw_notice)
