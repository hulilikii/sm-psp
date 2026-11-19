
from pathlib import Path
import hashlib

# Exact current-main Git blob hashes. Refuse to patch a different source tree.
EXPECTED = {
    "src/sm_81.c": "94155a1adb027914ec6da61dc38d0e7b40672856",
    "src/sm_82.c": "15dadd8a5bf52992889871e4ac015cc0b492ba9b",
}

EDITS = {
    "src/sm_81.c": [
        (
            """  DrawFileSelectSamusHelmets();
  HandleFadeOut();
  if ((reg_INIDISP & 0xF) == 0) {
    game_state = kGameState_2_GameOptionsMenu;""",
            """  DrawFileSelectSamusHelmets();
  HandleFadeOut();
  PspRenderer_SetMosaicTransition(16 - (reg_INIDISP & 0xF));
  if ((reg_INIDISP & 0xF) == 0) {
    game_state = kGameState_2_GameOptionsMenu;"""
        ),
    ],
    "src/sm_82.c": [
        (
            """void GameOptionsMenu_2_FadeInOptionsScreen(void) {  // 0x82ECE4
  HandleFadeIn();
  if (reg_INIDISP == 15) {""",
            """void GameOptionsMenu_2_FadeInOptionsScreen(void) {  // 0x82ECE4
  HandleFadeIn();
  PspRenderer_SetMosaicTransition(16 - (reg_INIDISP & 0xF));
  if (reg_INIDISP == 15) {"""
        ),
        (
            """void GameOptionsMenu_C_FadeOutOptionsScreenToStart(void) {  // 0x82EE92
  HandleFadeOut();
  if (reg_INIDISP == 0x80) {""",
            """void GameOptionsMenu_C_FadeOutOptionsScreenToStart(void) {  // 0x82EE92
  HandleFadeOut();
  PspRenderer_SetMosaicTransition(16 - (reg_INIDISP & 0xF));
  if (reg_INIDISP == 0x80) {"""
        ),
        (
            """void GameOptionsMenu_4_StartGame(void) {  // 0x82EEB4
  PspRenderer_ClearButtonIcons();
  game_options_screen_index = 0;""",
            """void GameOptionsMenu_4_StartGame(void) {  // 0x82EEB4
  PspRenderer_ClearButtonIcons();
  PspRenderer_SetMosaicTransition(1);
  game_options_screen_index = 0;"""
        ),
    ],
}


def git_blob_sha(data):
    header = f"blob {len(data)}\0".encode()
    return hashlib.sha1(header + data).hexdigest()


# Validate every file and every replacement before writing anything.
updated = {}

for filename, edits in EDITS.items():
    path = Path(filename)
    original = path.read_bytes()

    actual_sha = git_blob_sha(original)
    expected_sha = EXPECTED[filename]

    if actual_sha != expected_sha:
        raise SystemExit(
            f"{filename}: source mismatch\n"
            f"Expected Git blob: {expected_sha}\n"
            f"Found Git blob:    {actual_sha}\n"
            "No files were changed."
        )

    try:
        content = original.decode("utf-8")
    except UnicodeDecodeError:
        raise SystemExit(f"{filename}: not valid UTF-8; no files changed.")

    for old, new in edits:
        count = content.count(old)
        if count != 1:
            raise SystemExit(
                f"{filename}: expected one matching patch anchor, found {count}.\n"
                "No files were changed."
            )
        content = content.replace(old, new, 1)

    updated[filename] = content.encode("utf-8")


for filename, content in updated.items():
    Path(filename).write_bytes(content)
    print(f"Patched {filename}")

print("Mosaic transition changes applied.")

