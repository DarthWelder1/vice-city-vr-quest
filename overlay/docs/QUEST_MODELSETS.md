# Classic and Modern model assets on Quest

The Quest port supports the same category-based model overlay shell as PC:

- World
- Vegetation
- Vehicles
- Peds
- Weapons

Classic vegetation is the default and recommended choice. HD palm/tree models
can be dramatically more expensive on a mobile GPU even when draw-call counts
look modest.

## Prepare the overlay on PC

Use the public PC v0.5.0 Modern model-set builder. It must produce at least:

```text
modelsets/modern/models/gta3.img
modelsets/modern/models/gta3.dir
modelsets/modern/vegetation_models.txt
```

Do not redistribute the generated directory: it contains data derived from the
player's original game and external model packs.

## Copy to Quest

Close the game, create the destination directories, then copy the complete
generated `modern` folder below:

```text
/sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets/modern
```

For example, from the directory containing `modern`:

```powershell
adb shell mkdir -p /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets
adb push modern /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets
```

The final device path must not contain `modern/modern`. Verify it with:

```powershell
adb shell ls /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets/modern/models
adb shell ls /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets/modern/vegetation_models.txt
```

In the headset open `VR MENU > MODEL ASSETS`. Category changes are startup
choices: select the desired categories and fully restart the game. Keep
Vegetation on Classic unless testing the performance cost intentionally.

If the manifest is absent, the menu reports Vegetation as unavailable and the
loader forces that category to Classic instead of guessing model names.

