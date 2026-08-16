# BOTW Mousecam

A mouse camera mod for *The Legend of Zelda: Breath of the Wild* on Cemu. Smooth free-look camera, full magnesis control, bow-aiming sensitivity, and mouse-driven menus — without touching a single game file.

## What it is

Two files: a small companion app and a mod it loads into Cemu at runtime. Nothing gets installed into the game or into Cemu itself. Run the app, load the mod, play with the mouse.

## Features

### Mouse camera
- Smooth, free-look camera controlled by the mouse. Toggle it in-game with **F2**.
- Separate horizontal and vertical sensitivity sliders, or sync them.
- Optional **full orbit** — the camera swings all the way around Link.
- Forced camera moments (cutscenes, scripted shots) are left to the game — the mod steps aside instead of fighting it.

### Magnesis
- Three speed modes: **Vanilla** (feels like the game), **Extended** (faster), **Unlimited** (no limits at all).
- Pull objects toward or away from you with the scroll wheel.
- Separate horizontal and vertical sensitivity for magnesis control.
- **First-person mode**: the camera moves to eye level and aims at the object you're holding, with sliders for eye height, forward/back, and left/right positioning.

### Archery
- A separate, lower sensitivity engages while you're drawing the bow, so precise aiming doesn't require a rock-steady wrist. Fully adjustable.

### Menus & UI
- When a menu is open, the mouse drives the dpad — navigate everything with the mouse.
- Scroll the wheel to flip through weapons and shields.
- Bind up to five mouse buttons to any gamepad button. The app reads your Cemu controller profile, so the dropdowns match your actual setup (GamePad or Pro).

### The companion app
- Dark or light theme, following Windows automatically.
- Live readout of camera position, aim point, field of view, and pitch while you play.
- Settings persist in `mousecam_config.json` next to the app. Old config files are converted automatically.
- A log panel showing what the mod is doing, so problems are easy to describe and fix.

## How it works (short version)

The companion app finds Cemu, loads the mod into it, and keeps it updated with your settings. The mod watches how the game drives the camera, takes over while you're using the mouse, and stays out of the way the rest of the time. Close the app and it removes itself and restores everything. Your game files are never modified.



## Requirements

- Windows 10 or 11 (64-bit)
- Cemu **2.6** (stable) or **Cemu Experimental** 
- Breath of the Wild, of course

## Install

1. Put `mousecam-companion.exe` and `botw-mousecam-rewrite.dll` in the same folder. They must stay together — the mod is loaded from next to the app.
2. Run the companion app as **administrator** (required to load into Cemu).
3. Click the inject button, then start Cemu. (Or start Cemu first and inject after.)
4. Load your save. In game, press **F2** to turn the mouse camera on.

If Cemu is in an unusual location or has a custom process name, point the app at it via `mousecam_config.json`.

## Configuration

All settings live in `mousecam_config.json`, created next to the app on first run. Everything in it is editable from the app's UI — sliders, toggles, and dropdowns all write the file. Open it by hand only if you know what you're doing.

## Usage

After injection it finds gameromcamera pretty quickly,which is all you need for camera control.but it looks for a few more stuff for the rest of the features.Most important one is menu state.for it to find menu state,you need to open and close the menu a few times after injecting.until it finds the menu state.Without menu state found,level changes will break the camera and you'll need the manually reinject.

## Building from source

- Visual Studio 2022 with the C++ (v143) toolset and MASM.
- Open `botw-mousecam-rewrite.sln`, build **x64** in Release (or Debug, if you're poking around).
- Output lands in `bin/Release/`: `mousecam-companion.exe` and `botw-mousecam-rewrite.dll`.

## License

MIT — see [LICENSE](LICENSE).
