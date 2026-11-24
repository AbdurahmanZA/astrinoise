# Dashboard

The dashboard lets a team leader or admin choose which phones get cleaned without touching the
dialplan. It is two pages and two PHP scripts. It needs no service and no ARI:

```
browser ── callfilter.html / callfilter-phones.html
   │  GET state / POST one change          GET phone list
   ▼                                         ▼
callfilter.php                            phones.php
   │  asterisk -rx "database put|del rnnoise ..."     FreePBX devices, live registrations,
   │  audit line per change                           hot-desk users, CDR (read-only)
   ▼
AstDB family rnnoise  ◄── read on every outbound trunk call by
                          [macro-dialout-trunk-predial-hook] → [rnnoise-decide]
                          → Set(ASTRINOISE(rx)=...) on the caller's channel
```

## Design

**AstDB is the only state.** The dialplan reads AstDB on every outbound trunk call, and the page
writes AstDB. A change reaches the next new call at once; calls already up keep what they started
with. You can change the same keys by hand (`asterisk -rx 'database put rnnoise mode off'`) and the
page shows it on its next 5 s refresh.

**The PHP checks every value before it touches Asterisk.** It knows only these changes: `mode` off,
all or some; `strength` 1-100; `max` 0-500; `engine` rnnoise or deepfilter; `phones` with a list of
1-1000 phone numbers of 2-8 digits and a state of on, off or clear. Anything else gets a 400. The numbers go into
`asterisk -rx` only after those checks, and the whole command passes through `escapeshellarg`.

**Every change is logged.** Each accepted change adds a line with the time and the browser's address
to `/var/log/asterisk/rnnoise-dash.log`.

**A cross-site guard.** POSTs need the header `X-Requested-With: callfilter`. Only the page's own
`fetch()` sends it; a plain HTML form on another site cannot.

**Put the pages behind an IP allow-list or the PBX's own login.** The scripts have no login of their
own. On FreePBX an Apache `<Directory>` block with `Require ip <your admin networks>` works (see Install).

## Pages

**Call filter** (`callfilter.html`)
- Off / All calls / Some calls.
- Engine: RNNoise or DeepFilterNet. The DeepFilterNet button is greyed out when
  `libastrinoise_dfn.so` is not installed on the PBX.
- Strength, and the most cleaned calls at once.
- Module loaded or not, calls being cleaned now, work per 20 ms frame (average and slowest), late
  audio (must stay 0) and minutes cleaned, from `astrinoise show stats`, with a line per engine
  that has cleaned audio since the last reset.
- How to test it from a desk phone, and what it does and does not do.

**Phones** (`callfilter-phones.html`)
- Every FreePBX device with its name, the hot-desk user on it now, online state, last call and
  calls in the last 30 days, from `phones.php`.
- An ON/OFF button per phone, search, and "switch shown phones ON/OFF".
- "Active phones only" hides devices offline and unused for over 7 days, so a long list stays short.
- Phones in AstDB that FreePBX no longer lists (for example an agent number) still show, so you can
  switch them off.

## Which calls get cleaned

`[rnnoise-decide]` collects up to three keys for the calling channel: its PJSIP endpoint, the
hot-desk phone its user is logged on to, and the user number (`AMPUSER`). Then:

| mode | phone keys | result |
|---|---|---|
| off (or missing) | any | not cleaned |
| any | any key is `off` | not cleaned (off always wins) |
| all | none `off` | cleaned |
| some | at least one `on`, none `off` | cleaned |
| some | none `on` | not cleaned |

It never cleans a call on an emergency route, and it stops at `max` cleaned calls at once (counted
with `GROUP(rnnoise)`).

Test the rule without a phone and without touching `rnnoise/mode`:

```
asterisk -rx 'channel originate Local/some-1001@rnnoise-hook-test application Wait 1'
asterisk -rx 'database show rnnoise-test'      # some-1001 : 100 = cleaned, 0 = not
asterisk -rx 'database deltree rnnoise-test'
```

Use `all-`, `some-` or `off-`, then the phone, and add `-e` to test an emergency route.

## Sizing `max`

One cleaned call costs its `avg_us` per 20 ms of one core. With RNNoise at 0.3 ms (a recent desktop
CPU) that is 1.5 % of a core per call; at 1 ms (a 2010 Xeon) it is 5 %. DeepFilterNet costs several
times more: see the engine table in the README. Set `max` so the worst case leaves the box room for
everything else, and lower it before you switch a busy PBX to DeepFilterNet.

## Install

On the FreePBX box, as root, after the module is loaded and the dialplan is in place:

```
mkdir -p /var/www/html/astri-noise
install -m 644 -o asterisk -g asterisk dashboard/www/* /var/www/html/astri-noise/
cat > /etc/apache2/conf-available/astri-noise.conf <<'CONF'
<Directory /var/www/html/astri-noise>
  Require ip 127.0.0.1 192.0.2.0/24
</Directory>
CONF
a2enconf astri-noise && apachectl configtest && systemctl reload apache2
```

Replace `192.0.2.0/24` with your admin networks. FreePBX runs Apache as `asterisk`, which can run
`asterisk -rx` and read `/etc/freepbx.conf`, so the PHP needs no extra rights.

## History

The first dashboard drove an external relay. A Python controller chose between three engines
(Asterisk's DENOISE, RNNoise and NSNet2 in a Python process), hooked trunks over ARI, and read
changes from an inbox folder. Once the cleaning moved into Asterisk, AstDB and the predial hook
replaced all of that. The page layout, the per-phone list and All calls / Some calls stayed.
