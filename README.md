<div align=center>

<img src="extras/banner.png" alt="Banner" width="35%">

</div>
<h1 align=center>LEGO Ninjago: Shadow of Ronin · Switch Port</h1>

A wrapper/port of the Android release of **LEGO Ninjago: Shadow of Ronin**
(package `com.wb.goog.lnjgo`, v2.2.1.02). It loads the original game binary
`libLEGO_Pixel_Mobile.so` (TT Games' "Fusion" engine), patches it and runs it
inside a minimal Android-like environment natively on the Switch.

No game code or assets are included in this repository.

### How to install

You need the **v2.2.1.02** Android release; extract from your own legally-owned
copy.

1. `libLEGO_Pixel_Mobile.so` (from the APK's `lib/arm64-v8a/`).
2. Extracted `.fib`, `music`, `cutscenes` (from the APK's
   `assets/` folder).
3. The port `.nro` (any filename, e.g. `lnsor.nro`).

Resulting layout (`/switch/lnsor/` used as the example):

```
/switch/lnsor/
  lnsor.nro
  libLEGO_Pixel_Mobile.so
  assets/           # the extracted game assets
  config.txt        # created on first run
  save/             # created on first run; savegame
  write/            # created on first run
  cache/            # created on first run
```

### Notes

The port reads `/switch/lnsor_nx/config.txt`, created on first run:

* `screen_width` / `screen_height` — render resolution; `-1` auto-picks
  1280x720 handheld / 1920x1080 docked.

### How to build

Install the devkitPro Switch toolchain and portlibs:

```sh
pacman -S devkitA64 switch-tools libnx switch-sdl2 switch-mesa switch-libdrm_nouveau
```

Then `make`.

### Credits

* TheOfficialFloW for the original Android so-loader (gtasa_vita).
* gm666q for the PS Vita so-loader groundwork and the OpenSL ES build.
* fgsfds for max_nx / the Switch so-loader groundwork reused here.

### Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no affiliation with TT Games, Warner Bros., or the LEGO Group.
"LEGO Ninjago: Shadow of Ronin" and related marks belong to their respective
owners. No assets or program code from the original game or its Android port are
included here. We do not condone piracy; users must own a legal copy of the game.
