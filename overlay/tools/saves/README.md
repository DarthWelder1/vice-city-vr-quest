# Vice City VR save transfer

These tools transfer current Vice City VR saves between the Win64 PC build and
the native Meta Quest build. They preserve the game's slot number, convert the
small ABI-dependent garages section, validate the save checksum, and create a
backup before overwriting an existing save.

Original retail, old 32-bit, and `Vanilla` reVC saves are not supported. The
converter deliberately rejects unknown or corrupt files instead of risking a
crash or damaged progress. Before an import writes anything to the Quest, it
also verifies the current Win64 `Cranes`, `Pickups`, `Phone`, and `PlayerInfo`
block signatures; an old save can have the expected file size and checksum but
still use an incompatible internal layout.

## Windows

Connect the Quest by USB, enable USB debugging, close the game, then double
click one of these files:

- `import-pc-save-to-quest.bat` - copy a PC save to the Quest.
- `export-quest-save-to-pc.bat` - copy a Quest save to the PC.

The script asks for the slot (1-8) and, when necessary, the PC `userfiles`
folder. It finds ADB from SideQuest, Android platform-tools, or `PATH`.
Python is not required on Windows.

Command-line examples:

```powershell
.\transfer-vr-save.ps1 -Mode Import -Slot 3 -PcSaveDirectory "C:\Games\Vice City VR\userfiles"
.\transfer-vr-save.ps1 -Mode Export -Slot 3 -PcSaveDirectory "C:\Games\Vice City VR\userfiles"
```

## Linux and Steam Deck

Requirements: `adb`, `python3`, USB debugging, and an authorized headset with
Vice City VR installed.

```bash
bash ./import-pc-save-to-quest.sh 3 ~/Games/vice-city-vr/userfiles/GTAVCsf3.b
bash ./export-quest-save-to-pc.sh 3 ~/Games/vice-city-vr/userfiles/GTAVCsf3.b
```

You can also call `save_transfer.sh import ...` or `save_transfer.sh export ...`
directly. Set `ADB_SERIAL` when more than one Android device is connected.

Backups are stored in `MiamiVR-save-backups` on Windows and in
`save-transfer/backups` on Linux by default. The game is force-stopped during a
transfer; start it again after the script reports success.
