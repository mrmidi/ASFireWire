"""Scrollable routing/mixer matrix table widget."""
from textual.app import ComposeResult
from textual.widgets import DataTable
from textual.reactive import reactive


class MatrixTable(DataTable):
    """A DataTable subclass for routing/mixer matrices with keyboard navigation."""

    DEFAULT_CSS = """
    MatrixTable {
        height: 1fr;
    }
    """

    def on_mount(self) -> None:
        self.cursor_type = "cell"
        self.zebra_stripes = True
