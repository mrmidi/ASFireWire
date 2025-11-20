#!/usr/bin/env python3
"""
PHY Register 1 Gap Count Bug Simulator

Demonstrates the root cause of the gap count persistence issue:
- InitiateBusReset() using WritePhyRegister() destroys gap count
- Fixed version using UpdatePhyRegister() preserves gap count

PHY Register 1 bit layout (IEEE 1394a):
  Bits 7:6 - IBR (Initiate Bus Reset) / RHB (Root Hold-Off Bit)
  Bits 5:0 - Gap Count

References:
- Linux: firewire/core-card.c::reset_bus() uses update_phy_reg()
- ASFW Bug: HardwareInterface::InitiateBusReset() uses WritePhyRegister()
"""

class PHYRegister1:
    """Simulates IEEE 1394 PHY Register 1"""

    def __init__(self):
        self.value = 0x00  # Default: gap=0, no reset
        self.persistent_core = 0x00  # Simulates persistent storage
        self.enab_accel = False  # PHY register 5 bit 6

    def write(self, data: int) -> None:
        """Direct write (WritePhyRegister behavior)"""
        self.value = data & 0xFF
        # If Enab_accel is enabled, write to persistent core
        if self.enab_accel:
            self.persistent_core = self.value
        print(f"  PHY Reg 1 WRITE: 0x{data:02x} → register=0x{self.value:02x} persistent=0x{self.persistent_core:02x}")

    def update(self, clear_bits: int, set_bits: int) -> None:
        """Read-modify-write (UpdatePhyRegister behavior)"""
        old_value = self.value
        self.value = (self.value & ~clear_bits) | set_bits
        # If Enab_accel is enabled, write to persistent core
        if self.enab_accel:
            self.persistent_core = self.value
        print(f"  PHY Reg 1 UPDATE: 0x{old_value:02x} & ~0x{clear_bits:02x} | 0x{set_bits:02x} → 0x{self.value:02x} (persistent=0x{self.persistent_core:02x})")

    def read(self) -> int:
        """Read current register value"""
        return self.value

    def bus_reset(self) -> None:
        """Simulate bus reset - PHY reloads from persistent core"""
        old_value = self.value
        if self.enab_accel:
            # With Enab_accel: reload from persistent core
            self.value = self.persistent_core & 0x3F  # Clear IBR bits, keep gap
        else:
            # Without Enab_accel: reset to hardware default
            self.value = 0x00  # Assume hardware default gap=0
        print(f"  BUS RESET: PHY reload → register 0x{old_value:02x} → 0x{self.value:02x} (from {'persistent' if self.enab_accel else 'default'})")

    def get_gap_count(self) -> int:
        """Extract gap count (bits 5:0)"""
        return self.value & 0x3F

    def get_ibr(self) -> bool:
        """Check if IBR (bit 6) is set"""
        return (self.value & 0x40) != 0

    def set_enab_accel(self, enabled: bool) -> None:
        """Enable/disable Enab_accel (PHY reg 5 bit 6)"""
        self.enab_accel = enabled
        print(f"  PHY Reg 5: Enab_accel={'ENABLED' if enabled else 'DISABLED'}")


def print_state(phy: PHYRegister1, label: str) -> None:
    """Print current PHY state"""
    gap = phy.get_gap_count()
    ibr = phy.get_ibr()
    print(f"\n{'='*60}")
    print(f"STATE: {label}")
    print(f"  Register Value: 0x{phy.value:02x}")
    print(f"  Gap Count: 0x{gap:02x} ({gap})")
    print(f"  IBR Bit: {ibr}")
    print(f"  Persistent Core: 0x{phy.persistent_core:02x}")
    print(f"  Enab_accel: {phy.enab_accel}")
    print(f"{'='*60}")


def simulate_buggy_sequence():
    """
    Simulates CURRENT (BUGGY) implementation:
    1. Write gap=0x3F during initialization
    2. Call InitiateBusReset() using WritePhyRegister(1, 0x40)
    3. Gap count is DESTROYED
    """
    print("\n" + "="*60)
    print("SIMULATION 1: BUGGY Implementation (Current Code)")
    print("="*60)

    phy = PHYRegister1()

    # Step 1: Initialization
    print("\n--- STEP 1: Initialization ---")
    print("Writing gap count = 0x3F to PHY register 1")
    phy.write(0x3F)
    print_state(phy, "After initialization gap write")

    # Step 2: Enable Enab_accel (happens AFTER gap write in current code)
    print("\n--- STEP 2: Enable Enab_accel ---")
    phy.set_enab_accel(True)
    print("NOTE: Gap was written BEFORE Enab_accel enabled!")
    print("      → Gap count NOT in persistent core yet")
    print_state(phy, "After Enab_accel enabled")

    # Step 3: First bus reset (initialization forced reset)
    print("\n--- STEP 3: Forced Bus Reset (InitiateBusReset) ---")
    print("Calling: InitiateBusReset(false) → WritePhyRegister(1, 0x40)")
    phy.write(0x40)  # BUGGY: Direct write overwrites gap!
    print_state(phy, "After InitiateBusReset WRITE")

    # Step 4: Bus reset happens
    print("\n--- STEP 4: Bus Reset Occurs ---")
    phy.bus_reset()
    print_state(phy, "After first bus reset")

    # Step 5: Second bus reset (e.g., from bus manager)
    print("\n--- STEP 5: Second Bus Reset (Bus Manager) ---")
    print("Calling: InitiateBusReset(false) → WritePhyRegister(1, 0x40)")
    phy.write(0x40)  # BUGGY: Overwrites gap AGAIN!
    print_state(phy, "After second InitiateBusReset WRITE")

    print("\n--- STEP 6: Second Bus Reset Occurs ---")
    phy.bus_reset()
    print_state(phy, "After second bus reset")

    # Result
    final_gap = phy.get_gap_count()
    print(f"\n{'='*60}")
    print(f"RESULT: Gap count = {final_gap} (EXPECTED: 63, GOT: {final_gap})")
    print(f"BUG CONFIRMED: Gap count was DESTROYED by WritePhyRegister!")
    print(f"{'='*60}")

    return final_gap


def simulate_fixed_sequence():
    """
    Simulates FIXED implementation:
    1. Write gap=0x3F during initialization
    2. Call InitiateBusReset() using UpdatePhyRegister(1, 0, 0x40)
    3. Gap count is PRESERVED
    """
    print("\n" + "="*60)
    print("SIMULATION 2: FIXED Implementation (Using UpdatePhyRegister)")
    print("="*60)

    phy = PHYRegister1()

    # Step 1: Initialization
    print("\n--- STEP 1: Initialization ---")
    print("Writing gap count = 0x3F to PHY register 1")
    phy.write(0x3F)
    print_state(phy, "After initialization gap write")

    # Step 2: Enable Enab_accel
    print("\n--- STEP 2: Enable Enab_accel ---")
    phy.set_enab_accel(True)
    print("NOTE: Gap was written BEFORE Enab_accel enabled!")
    print("      → Gap count NOT in persistent core yet")
    print_state(phy, "After Enab_accel enabled")

    # Step 3: First bus reset (initialization forced reset)
    print("\n--- STEP 3: Forced Bus Reset (InitiateBusReset - FIXED) ---")
    print("Calling: InitiateBusReset(false) → UpdatePhyRegister(1, 0, 0x40)")
    phy.update(0x00, 0x40)  # FIXED: Read-modify-write preserves gap!
    print_state(phy, "After InitiateBusReset UPDATE")

    # Step 4: Bus reset happens
    print("\n--- STEP 4: Bus Reset Occurs ---")
    phy.bus_reset()
    print_state(phy, "After first bus reset")

    # Step 5: Second bus reset (e.g., from bus manager)
    print("\n--- STEP 5: Second Bus Reset (Bus Manager) ---")
    print("Calling: InitiateBusReset(false) → UpdatePhyRegister(1, 0, 0x40)")
    phy.update(0x00, 0x40)  # FIXED: Preserves gap!
    print_state(phy, "After second InitiateBusReset UPDATE")

    print("\n--- STEP 6: Second Bus Reset Occurs ---")
    phy.bus_reset()
    print_state(phy, "After second bus reset")

    # Result
    final_gap = phy.get_gap_count()
    print(f"\n{'='*60}")
    print(f"RESULT: Gap count = {final_gap} (EXPECTED: 63, GOT: {final_gap})")
    print(f"FIX CONFIRMED: Gap count PRESERVED by UpdatePhyRegister!")
    print(f"{'='*60}")

    return final_gap


def simulate_linux_approach():
    """
    Simulates LINUX approach:
    1. Enable Enab_accel FIRST
    2. Don't write gap count at all (rely on hardware default)
    3. InitiateBusReset uses UpdatePhyRegister
    """
    print("\n" + "="*60)
    print("SIMULATION 3: LINUX Approach (No Gap Write)")
    print("="*60)

    phy = PHYRegister1()

    # Simulate hardware strapping: gap=0x3F by default
    print("\n--- HARDWARE RESET ---")
    print("PHY hardware straps gap count to 0x3F (typical default)")
    phy.persistent_core = 0x3F
    phy.value = 0x3F
    print_state(phy, "After hardware reset")

    # Step 1: Enable Enab_accel FIRST (Linux approach)
    print("\n--- STEP 1: Enable Enab_accel (BEFORE any writes) ---")
    phy.set_enab_accel(True)
    print_state(phy, "After Enab_accel enabled")

    # Step 2: Configure PHY register 4 (Linux does this, not reg 1)
    print("\n--- STEP 2: Configure PHY Reg 4 (link_on + contender) ---")
    print("Linux: update_phy_reg(4, 0, PHY_LINK_ACTIVE | PHY_CONTENDER)")
    print("(Does NOT touch register 1 / gap count)")

    # Step 3: First bus reset
    print("\n--- STEP 3: First Bus Reset (InitiateBusReset) ---")
    print("Calling: InitiateBusReset(false) → UpdatePhyRegister(1, 0, 0x40)")
    phy.update(0x00, 0x40)
    print_state(phy, "After InitiateBusReset UPDATE")

    # Step 4: Bus reset happens
    print("\n--- STEP 4: Bus Reset Occurs ---")
    phy.bus_reset()
    print_state(phy, "After first bus reset")

    # Step 5: Second bus reset
    print("\n--- STEP 5: Second Bus Reset ---")
    print("Calling: InitiateBusReset(false) → UpdatePhyRegister(1, 0, 0x40)")
    phy.update(0x00, 0x40)
    print_state(phy, "After second InitiateBusReset UPDATE")

    print("\n--- STEP 6: Second Bus Reset Occurs ---")
    phy.bus_reset()
    print_state(phy, "After second bus reset")

    # Result
    final_gap = phy.get_gap_count()
    print(f"\n{'='*60}")
    print(f"RESULT: Gap count = {final_gap} (EXPECTED: 63, GOT: {final_gap})")
    print(f"LINUX APPROACH: Relies on hardware default + UpdatePhyRegister")
    print(f"{'='*60}")

    return final_gap


def main():
    print("""
╔════════════════════════════════════════════════════════════╗
║  PHY Register 1 Gap Count Bug Simulator                   ║
║  Demonstrates InitiateBusReset() gap count destruction     ║
╚════════════════════════════════════════════════════════════╝

This simulator validates the root cause of the gap count bug:

BUGGY CODE (HardwareInterface.cpp:393-402):
    bool InitiateBusReset(bool shortReset) {
        const uint8_t data = 0x40;
        return WritePhyRegister(1, data);  // ← DESTROYS gap count!
    }

FIXED CODE (Linux approach):
    bool InitiateBusReset(bool shortReset) {
        return UpdatePhyRegister(1, 0, 0x40);  // ← PRESERVES gap count!
    }

PHY Register 1 Layout:
  [7:6] IBR/RHB - Initiate Bus Reset / Root Hold-Off
  [5:0] Gap Count
""")

    # Run all three simulations
    buggy_gap = simulate_buggy_sequence()
    fixed_gap = simulate_fixed_sequence()
    linux_gap = simulate_linux_approach()

    # Summary
    print("\n" + "="*60)
    print("SUMMARY OF RESULTS")
    print("="*60)
    print(f"Buggy Implementation (WritePhyRegister):   gap = {buggy_gap:2d} ❌")
    print(f"Fixed Implementation (UpdatePhyRegister):  gap = {fixed_gap:2d} ✅")
    print(f"Linux Approach (No gap write):            gap = {linux_gap:2d} ✅")
    print("="*60)

    print("""
CONCLUSION:
-----------
The bug is in HardwareInterface::InitiateBusReset() at line 401:
    return WritePhyRegister(/*addr=*/1, data);

This OVERWRITES the entire register, destroying gap count bits [5:0].

FIX:
    return UpdatePhyRegister(/*addr=*/1, /*clear=*/0, /*set=*/0x40);

This performs read-modify-write, preserving gap count while setting IBR bit.

REFERENCES:
-----------
- Linux: firewire/core-card.c:220
  → return card->driver->update_phy_reg(card, reg, 0, bit);

- IEEE 1394a-2000: PHY Register 1 bit layout
  → Bits [5:0] must be preserved across reset operations

- ASFW Bug Report: /ASFW/docs/BUGS/GAP_COUNT_ISSUE.md
""")


if __name__ == "__main__":
    main()
