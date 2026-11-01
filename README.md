<div align="center">

# Spotify Remote

**A Spotify Connect remote for the Nintendo Switch, as an Ultrahand / Tesla overlay.**
Control the music playing on your phone, PC, speaker or TV without leaving your game.

[![Stars](https://img.shields.io/github/stars/zarcross-dev/spotify-remote?style=flat&color=yellow)](https://github.com/zarcross-dev/spotify-remote/stargazers)
[![Latest release](https://img.shields.io/github/v/release/zarcross-dev/spotify-remote?label=latest&color=blue)](https://github.com/zarcross-dev/spotify-remote/releases/latest)
[![License](https://img.shields.io/github/license/zarcross-dev/spotify-remote?color=189c11)](LICENSE)
[![Build](https://img.shields.io/github/actions/workflow/status/zarcross-dev/spotify-remote/ci.yml?branch=main&label=build)](https://github.com/zarcross-dev/spotify-remote/actions/workflows/ci.yml)
[![Language](https://img.shields.io/badge/language-C%2B%2B-ba1632?logo=cplusplus)](https://github.com/zarcross-dev/spotify-remote)
[![Downloads](https://img.shields.io/github/downloads/zarcross-dev/spotify-remote/total?color=6f42c1)](https://github.com/zarcross-dev/spotify-remote/releases)
[![Issues](https://img.shields.io/github/issues/zarcross-dev/spotify-remote?color=222222)](https://github.com/zarcross-dev/spotify-remote/issues)

</div>

---

## Features

- **Now playing:** title, artist, album, live progress bar and the device that is playing
- **Playback:** play / pause, next, previous, seek ±15 s, shuffle, repeat (off / all / one)
- **Volume** slider
- **Like:** save or remove the current track from your library
- **Devices:** move playback to any Spotify Connect device
- **Playlists:** play, shuffle-play, or start at a specific track
- **Queue:** see what's next and skip straight to a track
- **Auto-resume:** if nothing is playing, playback starts again on the last device you used
- **Hotkeys** on the main page:

  | Button | Action |
  |---|---|
  | `Y` | Play / Pause |
  | Right stick ← / → | Previous / Next track |
  | Right stick ↑ / ↓ | Volume up / down |

> [!IMPORTANT]
> Spotify only allows playback control with **Spotify Premium**. The zarcross-dev of the Spotify developer app (you) also needs Premium.

---

## Requirements

- A Switch running **Atmosphère** with **[Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay)** installed
- The Switch connected to the internet
- A **Spotify Premium** account
- A free **Spotify developer app** (see Setup, step 1)

---

## Installation

### 1. Download

Open the **[Releases page](https://github.com/zarcross-dev/spotify-remote/releases/latest)** and download:

- **`spotify-remote.ovl`**: the overlay
- **`spotify_auth.py`**: the PC helper that creates `config.ini` (only needed for setup method A)

### 2. Copy the overlay to the SD card

```
SD card
└── switch
    └── .overlays
        └── spotify-remote.ovl
```

> `.overlays` starts with a dot, so it may be hidden on macOS and Linux.

Put the SD card back in and open Ultrahand. **Spotify Remote** is now in the list.

---

## Setup (one time only)

### Step 1: Create a Spotify app

1. Go to **<https://developer.spotify.com/dashboard>** and log in.
2. Click **Create app** and fill in:
   - **App name / description:** anything (e.g. `Switch Remote`)
   - **Redirect URI:** `http://127.0.0.1:8888/callback` (copy it exactly)
   - **Which API/SDKs:** tick **Web API**
3. Save, open the app's **Settings**, and note the **Client ID** and **Client Secret**.

### Step 2: Create `config.ini`

The overlay reads its settings from this file:

```
SD card
└── switch
    └── spotify-remote
        └── config.ini
```

Pick **one** of the two methods below.

#### Method A: From a PC with `spotify_auth.py` (recommended)

Requires [Python 3](https://www.python.org/downloads/). No extra packages are needed.

1. Open a terminal in the folder where you saved `spotify_auth.py` and run:
   ```sh
   python3 spotify_auth.py
   ```
   On Windows: `py spotify_auth.py`
2. Paste your **Client ID** and **Client Secret** when asked.
3. Your browser opens. Log in to Spotify and click **Agree**.
4. The script writes **`config.ini`** in the same folder.
5. Copy it to the SD card as **`/switch/spotify-remote/config.ini`**. Create the `spotify-remote` folder if it doesn't exist.

Shortcuts:

```sh
python3 spotify_auth.py --sd E:\                 # write straight onto the SD card (drive letter or mount point)
python3 spotify_auth.py --ftp 192.168.1.42       # upload to the Switch over FTP (ftpd / sys-ftpd, port 5000)
python3 spotify_auth.py --manual                 # no local server: paste the redirect URL yourself
```

#### Method B: From the Switch (no PC software)

1. Open the overlay, then **Setup / link account**.
2. The screen shows an address like `http://192.168.1.42:8888/k3m9xa/`. Open it in a browser on a phone or PC **on the same Wi-Fi**.
3. Enter your **Client ID** and **Client Secret**, then tap **Log in with Spotify** and accept.
4. You land on a page that **fails to load** (`127.0.0.1`). That's expected. Copy that page's **full address** from the address bar.
5. Paste it into the setup page and tap **Finish setup**.
6. Keep the Setup screen open until the page says **All set!**. The overlay then writes `config.ini` itself.

> The short code at the end of the address changes each time. It keeps other people on your network out of the setup pages.

### Step 3: Play

Start Spotify on any device, open the overlay, and you're in control.
Nothing playing? Press `Y` to resume on your last device, or pick one in **Devices**.

---

## `config.ini` reference

```ini
[spotify]
client_id=your_client_id
client_secret=your_client_secret
refresh_token=filled_in_by_the_setup
device_id=
poll_interval_ms=2000
ssl_verify=1
debug_log=0
```

| Key | Description |
|---|---|
| `client_id`, `client_secret` | From your Spotify developer app |
| `refresh_token` | Filled in by the setup (script or Switch page). Keep it private. |
| `device_id` | Last used device, filled in automatically |
| `poll_interval_ms` | How often the overlay refreshes while open (default `2000`) |
| `ssl_verify` | `1` (default) verifies Spotify's certificates. Set `0` only if every request fails with SSL errors. |
| `debug_log` | `1` also writes a log to `/switch/spotify-remote/debug.log` |

After editing the file by hand, choose **Reload config.ini** in the overlay.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| "Not linked to Spotify yet" | `config.ini` is missing or in the wrong folder. It must be `/switch/spotify-remote/config.ini`. |
| "Login expired" | Run the setup again to get a new `refresh_token`. |
| "Wrong client id/secret" | Copy them again from the Spotify dashboard. |
| "INVALID_CLIENT: Invalid redirect URI" | The Redirect URI in the dashboard must be exactly `http://127.0.0.1:8888/callback`. |
| "No active device" | Open Spotify on a phone, PC or speaker, or pick a device in **Devices**. |
| "Spotify Premium is required" | Playback control needs Premium (Spotify's rule). |
| A playlist shows no tracks | In development mode, Spotify only lists tracks of playlists you own or collaborate on. Any playlist can still be played. |

**Debug log:** while the Setup screen is open, add `log` to its address (e.g. `http://192.168.1.42:8888/k3m9xa/log`) for a live log of every request, its result and any errors. Secrets and tokens are never logged. Please attach it when you [open an issue](https://github.com/zarcross-dev/spotify-remote/issues).

---

## Building from source

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `switch-curl switch-zlib`. [libultrahand](https://github.com/ppkantorski/libultrahand) is already included in `lib/`.

```sh
export DEVKITPRO=/opt/devkitpro
make            # -> out/switch/.overlays/spotify-remote.ovl
```

Tests run on a PC against a mock Spotify API and need g++, libcurl and python3:

```sh
tests/host/run.sh
```

---

## Credits

- [libultrahand](https://github.com/ppkantorski/libultrahand) by **ppkantorski** (GPLv2), a fork of [libtesla](https://github.com/WerWolv/libtesla) by **WerWolv**
- **libultra** by **ppkantorski**, licensed under [CC-BY-4.0](https://creativecommons.org/licenses/by/4.0/), used unmodified
- [cJSON](https://github.com/DaveGamble/cJSON), [stb_truetype](https://github.com/nothings/stb), [libnx](https://github.com/switchbrew/libnx), [curl](https://curl.se/), [zlib](https://zlib.net/)
- [Spotify Web API](https://developer.spotify.com/documentation/web-api)

## License

Spotify Remote is free software, licensed under the **[GNU General Public License v2](LICENSE)**, the same license as libultrahand, which it is built on.
Third-party components keep their own licenses. See **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**.

*Not affiliated with Spotify AB. Spotify is a trademark of Spotify AB.*
