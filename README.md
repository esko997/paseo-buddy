# Paseo Pager

A read-only pager for [Paseo](https://paseo.sh) agents that runs on a Miyoo Mini Plus (Onion OS),
with a small virtual pet that grows by eating your agents' output tokens.

```
Paseo daemon ──ws──▶ bridge/pager-bridge.mjs ──HTTP long-poll (LAN, token)──▶ Miyoo: miyoo/build/PaseoPager/pager
```

- **Read-only.** The bridge uses the official `@getpaseo/client` SDK, calls `agents.list` and
  subscribes to updates, and nothing else. It never sends prompts or answers permission requests.
- **The device app is C with no dependencies.** It draws straight to `/dev/fb0`, reads
  `/dev/input/event0`, runs the rumble motor through GPIO 48, and turns the screen off through GPIO 4.
  Sound goes through Onion's `libpadsp`/`audioserver` (OSS `/dev/dsp`). It is built as an armhf binary
  against glibc 2.28 (Debian buster toolchain, the same glibc as Onion's own toolchain).

## Bridge (one per computer running Paseo)

```sh
bridge/install.sh     # npm install, systemd user service on :6780, prints the pager.cfg line
```

- The token lives in `~/.config/paseo-pager/token`.
- The feed ledger lives in `~/.config/paseo-pager/ledger.json`. It holds:
  - the monotonic per-model-family output-token totals that the pet eats, and
  - today's output tokens and estimated cost (bridge-local day) for the den's ATM.

  History from before the bridge was installed (or before it tracked cost) is not counted.
- Environment overrides: `PAGER_LISTEN`, `PAGER_DAEMON_URL`, `PAGER_TOKEN`, `PASEO_PASSWORD`, `PAGER_CONFIG_DIR`.
- Endpoints:
  - `GET /healthz`
  - `GET /pager?token=…&rev=<last rev>&wait=<s>` returns a tab-separated `PAGER 1` document.
    `miyoo/src/feed.c` parses it. The request waits until something changes or `wait` expires.
    Devices ignore lines they don't know, so a newer bridge works with an older pager.
- If a firewall is on, allow the port from your LAN:
  - ufw (Pop!_OS, Ubuntu): `sudo ufw allow from 192.168.1.0/24 to any port 6780 proto tcp`
  - firewalld (Bazzite, Fedora): `sudo firewall-cmd --permanent --add-port=6780/tcp && sudo firewall-cmd --reload`
- To start the bridge at boot without anyone logging in, run `sudo loginctl enable-linger $USER`.

## Device

```sh
cd miyoo
./build-miyoo.sh      # cross-compiles in a cached Debian buster (glibc 2.28) podman image -> build/PaseoPager/
```

1. Copy `build/PaseoPager` to `/mnt/SDCARD/App/` on the SD card. If SSH is on (Apps → Tweaks → Network),
   run `miyoo/deploy.sh onion@<miyoo-ip>` instead. It keeps the device's `pager.cfg` and `pet.txt`, and
   checks that the binary runs.
2. Copy `pager.cfg.example` to `pager.cfg` and paste in the `bridge = …` lines that `install.sh` printed.
   You can list up to 4 bridges, for example this machine and Bazzite. Use IP addresses, not hostnames.
3. Turn on Wi-Fi in Onion and launch **Paseo Pager** from Apps.

There are three screens. Use L/R to cycle between them:
- **Agents**: a list of your agents, each with a small context-window fill bar, and the pet and a token-rate
  chart at the top.
- **Den**: a pixel-art room. Each agent is a little worker at a desk, the pet wanders the floor, and
  token crumbs fly to the pet as agents produce output.
  - An ATM in the corner shows today's tokens and estimated cost across all bridges. Big chunks of tokens
    print a receipt, which the pet tears off.
  - Workers start sweating when their context window passes 80%. Past 95% a warning sign appears on the desk.
  - The cost is Paseo's estimate at API prices (`~$`), not what a subscription actually bills.
- **Pet**: the pet's stats and diet. Y opens the wardrobe.

Controls:

| Button | Action |
| --- | --- |
| D-pad | Select an agent (in the Den and Pet screens, Left/Right switch screens) |
| A | Agent details |
| B | Back |
| X | Pat the pet |
| Y | Wardrobe (on the Pet screen) |
| L / R | Cycle Agents → Den → Pet |
| START | Clear alerts |
| SELECT | Screen off |
| MENU | Quit |

The screen turns off after `screen_timeout` idle seconds. A new alert turns it back on.
While the Miyoo is plugged in, the screen stays on (`awake_on_power = 1`).

Alerts fire when:
- an agent goes from running to finished,
- an agent starts waiting on a permission prompt, or
- an agent fails.

Each alert buzzes twice and plays a short chiptune cue:
- a soft rising chime when an agent finishes,
- a gentle three-note motif when an agent needs you,
- a soft falling pair when one fails.

The pet also chirps when it eats, when you pat it, and when it hatches or evolves.
Cues are short and rate-limited. They are silent from 23:00 to 07:00 bridge time (`quiet_nights`).
`volume = 0` turns them off.

The first snapshot after launch never buzzes or chimes. Onion's power-button sleep freezes the app and
drops Wi-Fi. The pager reconnects on wake, but it only pages you while it is awake.

### Always on

The pager is built to sit on a desk plugged in. While on USB power (`awake_on_power = 1`):
- The screen stays on.
- The pager sets Onion's `/tmp/stay_awake` flag, so Onion won't idle-sleep. A power-button press then only
  turns the screen off; the pager keeps running and paging.
- From 23:00 to 07:00 (bridge time) the screen dims to `night_dim` (an Onion brightness level, default 1;
  `-1` turns this off) and the UI drops to about 5 frames a second. Your Onion brightness comes back in the
  morning and when the app exits. `launch.sh` also restores it after a crash.
- `battery_care = 1` (the default) sets the Mini Plus's charge controller (AXP223, REG 0x33) to stop at
  4.1 V instead of 4.2 V, about 85–90% full. That's much easier on a battery that never unplugs.
  - It's applied every time the pager starts.
  - `battery_care = 0` puts back 4.2 V.
  - Onion's battery percentage will top out below 100%.
- Nothing stays still for long, to avoid image retention on the LCD:
  - the whole UI drifts by a pixel or two every few minutes;
  - the title animates now and then;
  - the den has ambient events: a bird or cat at the window, the pet tidying up, sunlight moving across
    the floor.

On battery, Onion's own sleep timer applies as usual.

### The pet

The pet asks nothing of you. There is no hunger, no decay, and no sickness, and ignoring it costs nothing.
- It eats your agents' output tokens.
- It cheers when an agent finishes a turn or when you pat it.
- It watches when agents are working.
- It naps at night.

It grows by lifetime tokens eaten: Egg → Baby at 5k, Kid at 100k, Teen at 500k, Adult at 2M.
Its colors follow its favorite model family (Claude, GPT, Gemini, other). The crest it gets as an
adult is locked in by whichever family fed it most at that moment.

The Pet screen shows:
- today's tokens and turns,
- your best day, and
- lifetime turns.

These are stats, not goals.

**Wardrobe.** The pet earns 22 cosmetics from milestones, and keeps them forever:
- hats, glasses, scarves and held items;
- den decor (a shelf ornament) and rug patterns;
- a shiny coat.

Milestones include tokens eaten, turns, days alive, its first adult form, being fed by all four model
families, a late-night snack, and seasonal dates (Halloween week, December, New Year, its birthday).
New items are put on automatically if that slot is empty. Locked ones show a silhouette and a hint.

The pet's state is saved to `pet.txt` next to the binary, only when something changed (at most every
5 minutes, right away for hatching, evolving or a new unlock, and on exit). Its clock is the bridge's,
because the Miyoo's own clock can't be trusted.

### Development

```sh
make -C miyoo desktop && PAGER_DIR=/path/with/pager.cfg miyoo/build/pager-desktop   # SDL2 window
make -C miyoo host && PAGER_DIR=… miyoo/build/pager-host --shot out.ppm --screen agents|den|pet|detail [--sim-ms 3000]
```

Desktop keys use the Miyoo's own keyboard codes, plus Z=A and X=B. Arrows are the D-pad, Enter is START,
Esc is MENU, E and T are L and R, A is X, and S is Y.

The font is Terminus (SIL OFL 1.1). `tools/gen_font.py` regenerates `miyoo/src/font_data.h`.
`tools/gen_icon.py` regenerates the Onion icon from the sprite data.
