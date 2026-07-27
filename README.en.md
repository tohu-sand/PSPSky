# PSPSky

**Languages:** [日本語](README.md) | [English](README.en.md)

A [Bluesky](https://bsky.app) client homebrew that runs entirely on the PSP (PlayStation Portable).

The PSP connects **directly to `bsky.social` over TLS 1.2** (libcurl + mbedTLS, with CA verification) to read the timeline and post.

---

## Features

- Runs standalone on the PSP — no relay server or proxy sitting between you and Bluesky. The PSP makes a direct TLS connection to `bsky.social` (with CA verification).
- Home timeline — shows display name (falling back to handle), handle, post text, and a relative timestamp ("5m ago", "3h ago"). Cursor-based paging loads up to 100 posts in total.
- Text posting — composed with the on-screen keyboard (Japanese supported, up to 300 characters).
- Image attachment — one JPEG or PNG per post (2,000,000 bytes or less). A thumbnail preview appears before sending, and an ALT text can be added optionally.

---

## Supported hardware and requirements

### On the PSP

| Item | Requirement |
|---|---|
| Console | PSP-1000 / 2000 / 3000 |
| Firmware | Custom firmware such as [ARK-4](https://github.com/PSP-Archive/ARK-4) |
| Memory Stick | 2 MB or more free |
| Wi-Fi | Uses "Connection 1" in the XMB network settings |
| Account | A Bluesky account and an issued app password |

The PSP's wireless supports **802.11b only**. Stock firmware handles encryption only up to WPA-PSK (TKIP), but some CFW such as [ARK-4](https://github.com/PSP-Archive/ARK-4) support WPA2, so with one installed the PSP can join WPA2 (AES) access points as-is.

### To build (only if building yourself)

| Item | Requirement |
|---|---|
| SDK | [pspdev](https://github.com/pspdev/pspdev) (`psp-gcc`, `psp-config`) |
| PSP-side libraries | libcurl / mbedTLS / zlib / intraFont (bundled with the pspdev SDK) |
| Host side | gcc (C99), GNU make, openssl, curl (to regenerate the CA bundle) |

---

## Installation

Issue a Bluesky app password beforehand (Settings → Privacy and Security → App Passwords). Do not use your main account password.

The layout on the Memory Stick is as follows.

```
ms0:/PSP/GAME/PSPSKY/
├── EBOOT.PBP      ← the application
├── cacert.pem     ← copy of assets/cacert.pem
└── pspsky.cfg     ← generated after the first login (you may also create it by hand)
```

If `cacert.pem` is missing or in the wrong place, TLS certificate verification fails and you cannot log in.

---

## Configuration file `pspsky.cfg`

A plain text file placed at `ms0:/PSP/GAME/PSPSKY/pspsky.cfg`.

```
identifier=user.bsky.social
password=xxxx-xxxx-xxxx-xxxx
lang=en
```

| Key | Meaning |
|---|---|
| `identifier` | Your Bluesky handle (no `@`; use the domain itself for a custom domain) |
| `password` | Your app password (`xxxx-xxxx-xxxx-xxxx`, hyphens included) |
| `lang` | Pins the UI language to `ja` or `en`. Optional (auto-detected if omitted) |

- Do not put spaces around `=`, and no quotes are needed. The order of lines does not matter.
- If the file does not exist, you are prompted on the on-screen keyboard at startup and the values are saved automatically after a successful login. Delete the file to re-enter them.

### Note on plaintext storage

**`pspsky.cfg` stores your handle and app password in plaintext, unencrypted.**
The PSP has no secure key storage, and a key placed anywhere the EBOOT can read it could equally be read by anyone with the same executable, so no cosmetic encryption is performed.

- **Always use a Bluesky app password; never enter your account password.**
  App passwords are issued from Settings → Privacy and Security → App Passwords in Bluesky and can be revoked individually at any time.

---

## Controls

| Button | Action |
|---|---|
| Up / Down (analog stick works too) | Scroll |
| L | Reload from the top (refresh) |
| R (or Down again at the bottom) | Load older posts (up to 100 in total) |
| Triangle | Compose a post (on-screen keyboard, up to 300 characters) |
| O | Confirm / accept (on the image preview screen: post with this image) |
| Square | On the post confirmation screen: attach one image (JPEG / PNG) and post |
| X | Cancel (on the image preview screen: choose a different file) |
| START | Quit |

Posts that contain an image are marked with `[image]` (`[画像]` in the Japanese UI) below the post text in the timeline.

### Posting with an image

Enter your text, continue to the confirmation screen, and press Square instead of O to open the file browser.

Once you pick a file, a preview screen appears showing the thumbnail, file path, file size, and the original image dimensions. After the preview you can optionally enter an ALT text (an image description).

- On the PSP's 802.11b radio, transferring close to 2 MB takes a while. The "uploading image" screen can block for up to 180 seconds, so do not power off even if the screen appears frozen.

---

## Building

Install the pspdev SDK and set up the environment variables.

```sh
export PSPDEV=/path/to/pspdev
export PATH=$PSPDEV/bin:$PATH
psp-gcc --version   # sanity check
```

Run `make` at the repository root to produce `EBOOT.PBP`.

```sh
make            # → EBOOT.PBP
```

Copy the generated `EBOOT.PBP` and `assets/cacert.pem` to the Memory Stick.

### CA bundle (`assets/cacert.pem`)

The PSP has no way to obtain root certificates on its own, so the repository bundles only the 6 root certificates that are needed (a full CA bundle is about 180 KB / 140 certificates — too large for the PSP's memory and load time).

| Certificate | Purpose |
|---|---|
| Amazon Root CA 1-4 | The current `bsky.social` chain (Amazon Trust Services) |
| Starfield Services Root Certificate Authority - G2 | Cross-signer of Amazon Root CA 1 |
| ISRG Root X1 | Let's Encrypt (in case of future switches or CDN differences) |

Regenerate it when an update is needed.

```sh
sh tools/make_cacert.sh    # → regenerates assets/cacert.pem
```

The script fetches from `https://curl.se/ca/cacert.pem`. If the network is unavailable, it automatically falls back to extracting the same 6 certificates from the host's `/etc/ssl/certs/ca-certificates.crt` with openssl.

### Repository layout

```
.
├── Makefile                 PSP build (EBOOT.PBP)
├── tools/make_cacert.sh     CA bundle generator
├── assets/cacert.pem        CA bundle
├── vendor/cjson/            cJSON v1.7.18 (vendored)
├── vendor/picojpeg/         picojpeg (vendored)
├── include/pspsky/          Shared headers
├── src/core/                Portable core (C99, no PSP dependencies)
└── src/psp/                 PSP shell (networking, OSK, UI, entry point)
```

`src/core/` is C99 code with no `#include <psp...>`; the only boundary to the PSP-specific part is `pspsky_transport_t` (the function-pointer vtable in `include/pspsky/transport.h`).

---

## Troubleshooting

TLS-related error screens include an error code in the form `(curl:number)`.

| Symptom | What to check |
|---|---|
| "cannot read cacert.pem" (curl:77) | Whether `cacert.pem` exists at the displayed path. Whether hidden Windows file extensions turned it into `cacert.pem.txt`. Whether the folder layout is `PSP/GAME/PSPSKY/` (watch out for doubled folders) |
| "server certificate verification failed" (curl:60) | The console's date and time settings (including year and time zone). If it persists, regenerate `cacert.pem` with `tools/make_cacert.sh` and replace it |
| "TLS handshake failed" (curl:35) | Poor signal or a momentary disconnect. Move closer to the router and retry with L |
| Cannot connect to the access point | The **physical WLAN switch** (left side on PSP-1000, top left on 2000/3000) → **"Connection 1"** in the network settings → whether the router has the 2.4 GHz band (802.11b) enabled. WPA2 (AES) works via ARK-4; WPA3-only networks do not |
| Login fails | Handle spelling (no `@`). Whether the **app password** was entered with hyphens. Reissue it if unsure |
| "please wait and retry" | Bluesky-side rate limiting. Wait a few minutes and refresh with L |
| Returns to the XMB right after launch | The copy of `EBOOT.PBP` may be corrupt — copy it again. Also check that the CFW is active |
| Japanese text is missing or garbled | The console font `flash0:/font/jpn0.pgf` may be unavailable (e.g. on non-Japanese consoles) |

---

## License

This project is released under the MIT License. See the [LICENSE](LICENSE) file for details.

For the licenses of bundled and linked third-party software (cJSON, picojpeg, libcurl, mbedTLS, zlib, intraFont, and the root certificate bundle), see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
