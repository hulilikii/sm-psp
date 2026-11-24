#!/usr/bin/env python3
"""Apply the refill-station early HUD update patch to a sm-psp checkout
Run from the repository root: python3 apply_refill_station_hud_patch.py
"""
from pathlib import Path

root = Path.cwd()
paths = {name: root / "src" / name for name in ("sm_80.c", "sm_82.c", "sm_84.c", "funcs.h")}
if not all(p.is_file() for p in paths.values()):
    raise SystemExit("Run this script from the sm-psp repository root (src files not found).")

def edit(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path.relative_to(root)}: expected one anchor, found {count}; refusing edit")
    path.write_text(text.replace(old, new, 1))

# Declare/reset the one-frame suppression state before InitializeHud.
edit(paths["sm_80.c"],
"""void InitializeHud(void) {  // 0x809A79
""",
"""static bool hud_updated_early_for_refill_station;

void InitializeHud(void) {  // 0x809A79
  hud_updated_early_for_refill_station = false;
""")

# Add wrappers after the existing complete HUD update routine.
edit(paths["sm_80.c"],
"""  vram_write_queue_tail = v5 + 2;
}

static const uint16 kHudItemTilemapOffsets[5] = { 0x14, 0x1c, 0x22, 0x28, 0x2e };
""",
"""  vram_write_queue_tail = v5 + 2;
}

// Refill stations perform the full HUD update early, before their message.
void HandleHudTilemapForRefillStation(void) {
  HandleHudTilemap();
  hud_updated_early_for_refill_station = true;
}


// Consume the early-update marker at the normal gameplay HUD point.
void HandleHudTilemapForGameFrame(void) {
  if (hud_updated_early_for_refill_station) {
    hud_updated_early_for_refill_station = false;
    return;
  }
  HandleHudTilemap();
}

static const uint16 kHudItemTilemapOffsets[5] = { 0x14, 0x1c, 0x22, 0x28, 0x2e };
""")

edit(paths["funcs.h"],
"""void HandleHudTilemap(void);
""",
"""void HandleHudTilemap(void);
void HandleHudTilemapForRefillStation(void);
void HandleHudTilemapForGameFrame(void);
""")
edit(paths["sm_82.c"],
"""  HandleHudTilemap();
  CalculateLayer2PosAndScrollsWhenScrolling();
""",
"""  HandleHudTilemapForGameFrame();
  CalculateLayer2PosAndScrollsWhenScrolling();
""")
edit(paths["sm_84.c"],
"""    samus_health = samus_max_health;
    samus_reserve_health = samus_max_reserve_health;
    DisplayMessageBox(0x15);
""",
"""    samus_health = samus_max_health;
    samus_reserve_health = samus_max_reserve_health;
    HandleHudTilemapForRefillStation();
    DisplayMessageBox(0x15);
""")
edit(paths["sm_84.c"],
"""    samus_super_missiles = samus_max_super_missiles;
    samus_power_bombs = samus_max_power_bombs;
    DisplayMessageBox(0x16);
""",
"""    samus_super_missiles = samus_max_super_missiles;
    samus_power_bombs = samus_max_power_bombs;
    HandleHudTilemapForRefillStation();
    DisplayMessageBox(0x16);
""")
print("Applied refill-station HUD patch.")
