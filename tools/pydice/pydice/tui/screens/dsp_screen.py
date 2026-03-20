"""DSP (compressor, EQ, reverb) screen."""
from textual.app import ComposeResult
from textual.widgets import Label, Static, DataTable
from ...dummy_data import AppState
from ...protocol.focusrite.spro24dsp import (
    compressor_commands, reverb_commands, effect_general_params_commands,
)
from ...protocol.constants import DSP_ENABLE_OFFSET, DSP_ENABLE_SW_NOTICE
from ...protocol.command import FireWireCommand
from ...protocol.codec import pack_u32
from ...protocol.codec import unpack_u32


class DspScreen(Static):
    """Displays DSP state: enable, ch-strip flags, compressor, reverb."""

    DEFAULT_CSS = """
    DspScreen {
        height: 1fr;
        padding: 1;
    }
    DspScreen Label {
        color: $accent;
        text-style: bold;
        margin-bottom: 1;
    }
    DspScreen DataTable {
        height: 1fr;
    }
    """

    def __init__(self, state: AppState, **kwargs):
        super().__init__(**kwargs)
        self._state = state

    def compose(self) -> ComposeResult:
        yield Label("DSP Parameters")
        yield DataTable(id="dsp-table")

    def on_mount(self) -> None:
        table = self.query_one("#dsp-table", DataTable)
        table.add_columns("Parameter", "Ch 1", "Ch 2")
        self._refresh_table()

    def _refresh_table(self) -> None:
        table = self.query_one("#dsp-table", DataTable)
        table.clear()
        s = self._state
        table.add_row("DSP Enable", str(s.dsp_enable), "")
        ep = s.effect_params
        table.add_row("Comp Enable", str(ep.comp_enable[0]), str(ep.comp_enable[1]))
        table.add_row("EQ Enable", str(ep.eq_enable[0]), str(ep.eq_enable[1]))
        table.add_row("EQ After Comp", str(ep.eq_after_comp[0]), str(ep.eq_after_comp[1]))
        c = s.compressor
        table.add_row("Comp Output", f"{c.output[0]:.3f}", f"{c.output[1]:.3f}")
        table.add_row("Comp Threshold", f"{c.threshold[0]:.3f}", f"{c.threshold[1]:.3f}")
        table.add_row("Comp Ratio", f"{c.ratio[0]:.3f}", f"{c.ratio[1]:.3f}")
        table.add_row("Comp Attack", f"{c.attack[0]:.3f}", f"{c.attack[1]:.3f}")
        table.add_row("Comp Release", f"{c.release[0]:.3f}", f"{c.release[1]:.3f}")
        r = s.reverb
        table.add_row("Reverb Enable", str(r.enabled), "")
        table.add_row("Reverb Size", f"{r.size:.3f}", "")
        table.add_row("Reverb Air", f"{r.air:.3f}", "")
        table.add_row("Reverb Pre-filter", f"{r.pre_filter:.3f}", "")

    def get_commands(self):
        cmds = []
        cmds.append(FireWireCommand(
            description="DSP Enable",
            app_offset=DSP_ENABLE_OFFSET,
            value=1 if self._state.dsp_enable else 0,
            sw_notice=DSP_ENABLE_SW_NOTICE,
        ))
        cmds.extend(effect_general_params_commands(self._state.effect_params))
        cmds.extend(compressor_commands(self._state.compressor))
        cmds.extend(reverb_commands(self._state.reverb))
        return cmds
