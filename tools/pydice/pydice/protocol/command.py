"""FireWire command representation and command log."""
from dataclasses import dataclass, field
from typing import Optional
from .constants import FW_BASE, APP_SECTION_BASE


@dataclass(frozen=True)
class FireWireCommand:
    description: str
    app_offset: int   # offset within application section
    value: int        # u32 value to write
    sw_notice: int    # software notice value (0 = none)

    @property
    def target_address(self) -> int:
        return FW_BASE + APP_SECTION_BASE + self.app_offset

    def format_display(self) -> str:
        lines = [
            f"WRITE  {self.description}",
            f"  addr: 0x{self.target_address:012X}",
            f"  val:  0x{self.value:08X}",
        ]
        if self.sw_notice:
            lines.append(f"  ntc:  SW_NOTICE → 0x{self.sw_notice:08X}")
        return "\n".join(lines)


@dataclass
class CommandLog:
    entries: list[FireWireCommand] = field(default_factory=list)

    def append(self, cmd: FireWireCommand) -> None:
        self.entries.append(cmd)

    def extend(self, cmds: list[FireWireCommand]) -> None:
        self.entries.extend(cmds)

    def clear(self) -> None:
        self.entries.clear()

    def __len__(self) -> int:
        return len(self.entries)
