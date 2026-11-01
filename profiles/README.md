# Game profiles

Each game's defaults for the per-game **Performance** settings, one file per
title: `<TITLE ID>.txt` (8 hex digits, upper case), whatever the disc's
version. The build packs this folder into the APK
(`android/app/build.gradle.kts`, task `syncProfileAssets`); a game's own
settings (Game settings > Performance) override its profile, and a setting a
profile leaves out takes the built-in default (`GameProfile.BUILTIN` in
`data/GameSettings.kt`). This file is not packed.

```
# a comment
threads=1        # CPU cores: 0 any, 1 renderer and decoder fast + audio slow,
                 # 2 emulated CPU and renderer fast, 3 emulated CPU and decoder fast
vis_wait=0       # visibility tests: 0 the last result at once, else ms to wait
submit_ms=4      # GPU submits within a frame every n ms, 0 once a frame
tex_new=true     # a texture's new image at each change
merge_draws=false
```
