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
    The yellow context-warning monitor rim blinks slowly while working (one second bright, one second dim).
    Sleeping bots get a softer four-second breathing glow instead, including bots asleep from exhausted
    subscription fuel. Both animations run at half speed in night-dim mode; urgent states keep a steady rim.
  - The cost is Paseo's estimate at API prices (`~$`), not what a subscription actually bills.
- **Pet**: the pet's stats and diet. A opens form previews; Y opens the wardrobe.

Controls:

| Button | Action |
| --- | --- |
| D-pad | Select an agent; browse in forms/wardrobe (otherwise Left/Right switch Den/Pet screens) |
| A | Agent details; on Pet stats, open forms |
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

The first snapshot after launch never buzzes or chimes. On battery, Onion's power-button sleep freezes the
app and drops Wi-Fi; the pager reconnects on wake, but it only pages you while it is awake. Plugged in,
the power button just turns the screen off (see Always on).

If no bridge has answered for 30 seconds, a red banner says why: either the Miyoo's own Wi-Fi is off
(switch it back on in Onion's menu) or Wi-Fi is up but no bridge answers. Losing the connection buzzes
once and wakes the screen.

### Subscription fuel gauges

One bridge (run `bridge/install.sh --usage` on it) polls `omp usage --json` every 5 minutes. That covers
every provider omp is logged into: Claude, Codex, Antigravity and Z.ai, each with its own windows
(5-hour, weekly, per model).

The gauges show fuel **left**:
- **Agents screen:** a row with one cell per provider, showing the % left in its tightest window. A small
  bar shows the 5-hour window when that isn't the tightest.
- **Den:** the wall panel alternates between tok/min and a FUEL panel. The FUEL panel counts down to the
  next reset of the most-used window.
- **Header:** a fuel-pump icon appears on every screen when any provider is low.

When a provider is low, its robots in the den get sleepy and drop fewer token crumbs.

Colours:
- **Yellow:** 80% or more used (20% or less left).
- **Red:** 95% or more used, or out. A red cell shows the time until it refills.

Readings older than 20 minutes are dimmed.

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
Its natural colors blend all four model-family diets (Claude, GPT, Gemini, Other), weighted by lifetime
output tokens eaten, with a gentle light-to-dark body gradient. There is no abrupt palette swap when
one family overtakes another, and adult colors keep responding to food. A wardrobe coat overrides
the natural gradient; the shiny coat uses a brighter version of the diet blend.

**Inherited adult forms.** The original look is always form 0. Fifteen generated forms become ready
at 2M, 3M, 5M, 8M, 12M, 17M, 23M, 30M, 38M, 47M, 57M, 68M, 80M, 93M, and 107M lifetime tokens.
Each pet has saved DNA, independent of its random finds. The family that fed it most on reaching
adulthood locks its lineage; later diet changes affect colors, not anatomy. The first generated
form combines body, ears, tail, crown, wings, markings, and movement traits. Each later form inherits
six traits and changes one. Movements include bouncing, floating, and waddling.

On **Pet → A**, compare CURRENT with PREVIEW. D-pad browses all forms; **L2/R2** jump five.
**A transforms** into a ready form, while **B keeps the current form**. Browse previous forms to
switch back freely, including the original. Reaching a milestone never changes the pet automatically,
uses no items, and never resets growth. The Pet screen marks newly ready forms, and growth bars keep
showing the next milestone after adulthood. Future-form previews before adulthood are marked
**not final**, because the favorite feeding family can still change.

Bodies share the wardrobe's face, hat, neck, and held-item anchors, so outfits stay on through
transformations. Hats hide crowns and the tops of ears. Old saves acquire deterministic DNA and keep
their current original shape, outfits, and collection progress; mixed diets adopt the new blended colors.

The Pet screen shows:
- today's tokens and turns,
- your best day, and
- lifetime turns.

These are stats, not goals.

**Wardrobe and collection book.** There are **120 permanent cosmetics**:
the original 22 milestone items, 86 finds, and 12 set-completion gifts. Old saves keep their
earned items, outfits, find progress, and daily draw limit. Collection storage supports 256 items.

- **Wearables:** hats, glasses, scarves, pendants, headphones, and held toys.
- **Den decor:** shelf ornaments, rug patterns, six wallpapers, and six animated window views.
- **Coats:** six new palettes alongside the original shiny coat.
- **Robot outfits:** 12 costumes, independently equipped for Claude, GPT, Gemini, and Other robots.
  They preserve the robots' status eyes, warning signs, and tired behavior.
- **Twelve sets:** Cozy, Garden, Space, Arcade, Seaside, Explorer, Ocean, Woodland, Celestial,
  Workshop, Candy, and Winter. The first six award shelf ornaments; the new six award necklaces,
  collars, a bow, or a scarf. Winter finds are available year-round.

On the Pet screen, **Y** opens the wardrobe. Left/Right change categories; Up/Down browse;
**L2/R2** jump a page; **A** equips or removes an item. Categories scroll across the tab strip.
Press **Y again** for the collection book, with set progress, reward previews, and progress toward
your next find. **L2/R2** flip between its two pages; the D-pad selects a set. **B** goes back.
Locked items show silhouettes or obscured swatches and earning hints. New ordinary wearables and
shelf/rug items go on automatically only if their slot is empty. Coats, wallpapers, window views,
and robot outfits are always opt-in. Removing a wallpaper or window view restores the original den.

**Finds come from normal agent activity.** The first fresh turn or output tokens after the app
has established a bridge baseline bring one welcome find. Then each completed turn earns one
point, and each 1,000 output tokens earns one point (partial thousands carry over).
Every 1,000 points earns another find, at most one per bridge-local day. Extra points carry over;
a banked find can arrive on a later day without new activity. There are no streaks or penalties.
The draw is pet-specific and saved: only unowned finds are eligible, and reopening cannot reroll.
Set rewards arrive immediately and do not consume that day's find.

Existing token, turn, age, growth, diet, late-night and seasonal milestones still work.
Historical totals on a newly connected bridge, and the first catch-up from an older save, do not
generate find points. Clock corrections backwards cannot bypass the daily limit.

The pet's state is saved to `pet.txt` next to the binary, only when something changed (at most every
5 minutes, promptly for hatching, evolving, new unlocks or outfit/form choices, and on exit). Its clock is the bridge's,
because the Miyoo's own clock can't be trusted.

### Development

```sh
make -C miyoo desktop && PAGER_DIR=/path/with/pager.cfg miyoo/build/pager-desktop   # SDL2 window
make -C miyoo host && PAGER_DIR=… miyoo/build/pager-host --shot out.ppm --screen agents|den|pet|detail [--sim-ms 3000]
make -C miyoo test   # migration, finds, outfits, inherited forms, diet blends and rendering bounds
```

Desktop keys use the Miyoo's own keyboard codes, plus Z=A and X=B. Arrows are the D-pad, Enter is START,
Esc is MENU, E and T are L and R, A is X, and S is Y.

The font is Terminus (SIL OFL 1.1). `tools/gen_font.py` regenerates `miyoo/src/font_data.h`.
`tools/gen_icon.py` regenerates the Onion icon from the sprite data.
