"""Main Textual application for pydice."""
from textual.app import App, ComposeResult
from textual.widgets import TabbedContent, TabPane, Footer
from textual.containers import Horizontal
from ..dummy_data import make_dummy_state
from .screens.output_screen import OutputScreen
from .screens.dsp_screen import DspScreen
from .screens.routing_screen import RoutingScreen
from .screens.mixer_screen import MixerScreen
from .screens.log_screen import LogScreen
from .widgets.command_panel import CommandPanel


class PyDiceApp(App):
    """DICE Protocol Interpreter / Command Generator TUI."""

    CSS = """
    Screen {
        layout: vertical;
    }
    #main-row {
        layout: horizontal;
        height: 1fr;
    }
    TabbedContent {
        width: 3fr;
        height: 100%;
    }
    CommandPanel {
        width: 1fr;
        height: 100%;
    }
    """

    BINDINGS = [
        ("q", "quit", "Quit"),
        ("c", "clear_log", "Clear log"),
        ("g", "generate_commands", "Generate cmds"),
    ]

    def compose(self) -> ComposeResult:
        self._state = make_dummy_state()
        self._output_screen = OutputScreen(self._state)
        self._dsp_screen = DspScreen(self._state)
        self._routing_screen = RoutingScreen(self._state)
        self._mixer_screen = MixerScreen(self._state)
        self._log_screen = LogScreen()
        self._cmd_panel = CommandPanel()

        with Horizontal(id="main-row"):
            with TabbedContent():
                with TabPane("Output", id="tab-output"):
                    yield self._output_screen
                with TabPane("DSP", id="tab-dsp"):
                    yield self._dsp_screen
                with TabPane("Routing", id="tab-routing"):
                    yield self._routing_screen
                with TabPane("Mixer", id="tab-mixer"):
                    yield self._mixer_screen
                with TabPane("Log", id="tab-log"):
                    yield self._log_screen
            yield self._cmd_panel
        yield Footer()

    def action_clear_log(self) -> None:
        self._cmd_panel.clear()

    def action_generate_commands(self) -> None:
        """Generate and log commands for current active tab."""
        active = self.query_one(TabbedContent).active
        if active == "tab-output":
            cmds = self._output_screen.get_commands()
        elif active == "tab-dsp":
            cmds = self._dsp_screen.get_commands()
        else:
            cmds = []
        self._cmd_panel.log_commands(cmds)
