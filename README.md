# btc-miner-psp

A **Bitcoin pool miner** for the **Sony PlayStation Portable** (PSP-1000
through PSP-3000). Connects to a public stratum-v1 mining pool over
the PSP's built-in 802.11b WiFi, sweeps the nonce space with software
double-SHA-256 on the 333 MHz MIPS R4000 CPU, and submits any shares
it finds.

**Hashrate**: ~30-50 kH/s.

**Probability of finding a share at modern Bitcoin difficulty**: not
zero in any strict mathematical sense, but small enough that the PSP
will time out before the pool ever validates a submission. This is a
**functioning miner that runs the real protocol against real pools** —
it just doesn't earn anything. The PSP is roughly **10^9 times slower**
than a single modern Bitmain S21 ASIC. That gap is the whole point of
the project.

[![PSP](https://img.shields.io/badge/PSP-1000%20%2F%202000%20%2F%203000-blue)](#)
[![EBOOT.PBP](https://img.shields.io/badge/EBOOT.PBP-prebuilt%20%26%20committed-success)](EBOOT.PBP)
[![Toolchain](https://img.shields.io/badge/toolchain-pspdev%20v20260501-orange)](https://pspdev.github.io/)

---

## Why the PSP

The PSP (December 2004) is the oldest mainstream portable game console
with **native 802.11b WiFi and arbitrary TCP/IP**. The Nintendo DS (also
2004) shipped with WiFi a month earlier but has only 4 MB RAM versus
the PSP's 32 MB, which makes a real JSON-RPC client uncomfortably tight
on the DS. The PSP also has a ~10× faster CPU and a much more mature
homebrew toolchain (`pspdev`).

See [the cross-platform analysis in `hash-bench`](https://github.com/dmang-dev/hash-bench)
for the broader context of "what can old game CPUs do with modern
cryptographic workloads".

---

## What it does (boot-to-mining sequence)

1. **Boot** the PSP, load `EBOOT.PBP` from
   `/PSP/GAME/btc-miner-psp/`.
2. The miner initializes pspDebugScreen, runs a SHA-256 self-test
   against the Bitcoin genesis-block header (proves the
   implementation is byte-correct before going on the wire).
3. Loads `PSP_NET_MODULE_COMMON` + `PSP_NET_MODULE_INET`, initializes
   `sceNet`, `sceNetInet`, `sceNetApctl`, `sceNetResolver`.
4. Calls `sceNetApctlConnect(1)` — connects via **WLAN profile 1**
   (you must have at least one network profile configured in
   XMB → Settings → Network Settings before running).
5. Resolves the pool hostname via the PSP kernel DNS resolver.
6. Opens a TCP socket to the pool, sends `mining.subscribe` +
   `mining.authorize`.
7. Waits for the first `mining.notify`, parses the job, builds an
   80-byte block header from coinbase + merkle branches.
8. Enters the nonce sweep loop:
   - Increments nonce, computes `SHA256(SHA256(header))`.
   - Compares output to the diff-1 target.
   - On every 16,384th nonce: updates the on-screen hashrate / total
     stats; non-blocking peek for new job notifications from pool.
   - On a found share (statistically improbable): submits to pool
     via `mining.submit`.

You'll see something like this on the PSP screen during mining:

```
btc-miner-psp v0.4
PSP 333 MHz MIPS R4000, software SHA-256d

SHA-256 self-test passed
Loading net modules... ok
sceNetInit... ok
sceNetInetInit... ok
sceNetApctlInit... ok
sceNetResolverInit... ok
Connecting WLAN profile 1...
connected.
Resolving solo.ckpool.org... 172.81.181.34
  subscribe rsp: {"id":1,"result":[[["mining.set_difficulty"...
  e1_len=4 e2_size=4
  authorize rsp: {"id":2,"result":true,"error":null,...
Stratum subscribed + authorized.

Mining job 6c4
ntime=68234DA1 nbits=170D86A3

Hashrate: 31420 H/s
Total:    187904 hashes
Last nce: 0002DDFF
```

---

## Try it

Pre-built `EBOOT.PBP` (152 KB) committed at the repo root. To run:

### Real PSP hardware
1. Copy `EBOOT.PBP` to `/PSP/GAME/btc-miner-psp/EBOOT.PBP` on your
   memory stick.
2. Configure a WLAN profile in **Settings → Network Settings** (the
   miner uses profile 1 by default).
3. Configure your pool — either edit the `DEFAULT_POOL_*` constants
   in `source/main.c` and rebuild, **or** drop a
   [`params.txt`](params.txt.example) on the memstick at
   `ms0:/PSP/SAVEDATA/btc-miner-psp/params.txt` (no rebuild required).
   The compiled default points at `solo.ckpool.org:3333` with a
   placeholder address.
4. Launch from PSP XMB → Game.

### PPSSPP emulator (no real hardware)
1. PPSSPP supports network emulation. Settings →  Networking → enable
   "Enable networking / wifi simulation" plus "Enable built-in PRO
   ad hoc server" (not needed for stratum but good baseline).
2. Open `EBOOT.PBP` from the PPSSPP file picker.
3. PPSSPP will use your host's network for `sceNet*` calls.

### JPCSP emulator (Java)
- Same as PPSSPP; JPCSP has more accurate network emulation but
  slower CPU emulation (so the hashrate display will be much
  smaller than real PSP).

---

## Build from source

Requires the **pspdev toolchain** (psp-gcc 15+, pspsdk, pspsdk-pack).
On Windows we install via WSL Ubuntu:

```bash
# in WSL:
curl -L -o pspdev.tar.gz \
  https://github.com/pspdev/pspdev/releases/download/v20260501/pspdev-ubuntu-latest-x86_64.tar.gz
tar xzf pspdev.tar.gz -C $HOME
export PSPDEV=$HOME/pspdev
export PATH=$PSPDEV/bin:$PATH

# build:
cd /mnt/i/btc-miner-psp  # or wherever
make
```

On Linux/macOS the same install works directly. The bundled
[`build.bat`](build.bat) Windows wrapper invokes WSL and copies the
output back to the project root.

### Build artifacts

| File | Purpose |
|---|---|
| `EBOOT.PBP` | bootable PSP package — the deliverable |
| `PARAM.SFO` | metadata block embedded in EBOOT.PBP (title etc.) |
| `btc-miner-psp.elf` | unstripped psp-gcc ELF (~480 KB) |
| `build/*.o` | compiled object files |

---

## Pool configuration

Two ways, layered:

**1. params.txt on the memstick (no rebuild).** Copy
[`params.txt.example`](params.txt.example) to
`ms0:/PSP/SAVEDATA/btc-miner-psp/params.txt` and edit. Format:

```
host=solo.ckpool.org
port=3333
user=bc1qexamplebtcaddressgoeshere.psp
pass=x
```

Each key is independent — leave a line out to keep the compiled
default for that field. Comments start with `#`. Unknown keys are
silently ignored (so future versions can add keys without breaking
old configs).

The miner prints the effective config + which keys came from
params.txt at boot:

```
config: loaded from params.txt: host port user
  pool: solo.ckpool.org:3333 user=bc1q...psp
```

**2. Compiled defaults.** Edit `source/main.c`'s `DEFAULT_POOL_*`
constants and rebuild. Used when params.txt is missing or doesn't
override the key.

```c
#define DEFAULT_POOL_HOST    "solo.ckpool.org"
#define DEFAULT_POOL_PORT    3333
#define DEFAULT_POOL_USER    "bc1qexamplebtcaddressgoeshere.psp"
#define DEFAULT_POOL_PASS    "x"
```

**Default points at ckpool's solo-mining endpoint** — a real public
pool that accepts connections from any miner. Replace `bc1qexample...`
with your own bech32 receiving address before the unlikely-but-possible
event you find a block.

Pools known to accept low-hashrate miners without auto-disconnecting:
- `solo.ckpool.org:3333` — solo mining, payout to BTC address
- `stratum.solomining.io:3333` — same idea, different operator
- `public-pool.io:21496` — public good pool, no payout floor

Avoid pools that enforce a minimum hashrate (most large pools); they
will disconnect the PSP within seconds.

---

## Hashrate napkin math

| Workload | Roundtrip cycles | Per-second @ 333 MHz |
|---|---:|---:|
| One SHA-256 compress (64 B block) | ~5,000 cycles | ~66 k blocks/sec |
| One SHA-256 of 80 B header (2 blocks: 1 full + 1 pad) | ~10,000 cycles | ~33 k hashes/sec |
| One **double** SHA-256 (Bitcoin work unit) | ~15,000 cycles | ~22 k H/s (rough) |

Measured on PPSSPP: **~30-50 kH/s** depending on PPSSPP's JIT mode
(closer to 50 in JIT, ~10 with pure interpreter). Real hardware
should sit near the upper bound — possibly higher if the icache stays
warm in the nonce loop.

**Reality check**: current Bitcoin network difficulty is ~70 trillion.
The expected time to find a single block at this hashrate is:

```
expected_time = 2^32 * difficulty / hashrate
              = 2^32 * 70e12 / 40000
              = 7.5e15 seconds
              = 240 million years
```

(per block, on the entire network's behalf — we'd find one block in
those 240 million years.)

Pool **share difficulty** is much lower (often 1.0 or 2^15), and pools
will set per-miner variable difficulty based on observed hashrate.
Even at pool diff 1, ~38 hours per share is the bare minimum; most
pools will drop the connection long before that.

This is the **academic / educational miner**, not the
mortgage-payment miner.

---

## What's correct vs. what's approximate

| Property | This implementation |
|---|---|
| SHA-256 (FIPS 180-4) | ✓ correct — passes RFC test vectors + genesis block double-hash in self-test |
| Stratum v1 subscribe / authorize / notify / submit | ✓ correct — speaks the wire protocol; tested against ckpool |
| Coinbase + merkle root construction | ✓ correct — passes round-trip against known testnet jobs |
| Block header byte order (LE/BE) | ✓ correct — produces hashes consistent with bitcoin-core |
| extranonce2 increment | ✓ correct but trivial — bumps by 1 per nonce-space slice |
| Variable difficulty (`mining.set_difficulty`) | ✓ — full 256-bit `hash <= target` check; target recomputed from pool diff via `target_from_difficulty()`; updates live mid-job without rebuild (since v0.2) |
| `mining.set_extranonce` | ✓ — subscription state updated in-place by `stratum_poll_nonblock`; the next `mining.notify` builds the job against the new extranonce1/2 automatically (since v0.2) |
| Reconnect on socket drop | ✓ — exponential backoff (1-60s); silently-dropped TCP detected via `MSG_PEEK` probe every 16k iters (since v0.3) |
| TLS | ✗ no TLS support; plain TCP only |
| RFC 6979 deterministic k | n/a — we don't sign anything, only hash |
| Job switching on `clean_jobs:true` | ✓ correct — `mining_loop` returns on new job |
| Multi-job preemption (mid-sweep) | ✓ correct — non-blocking poll every 16k hashes |

For v0.2 the variable-difficulty handling and `set_extranonce` should
land; reconnect logic and TLS support are stretch goals.

---

## Layout

```
source/
  main.c            boot, network init, mining loop, stats display
  sha256.c          FIPS 180-4 SHA-256 + self-test (genesis-block dhash)
  stratum.c         minimal Stratum-v1 client (JSON-RPC line protocol)
include/
  sha256.h
  stratum.h
build/              psp-gcc object files (gitignored)
EBOOT.PBP           prebuilt PSP package — drop on memstick
PARAM.SFO           metadata block (auto-generated)
Makefile            psp-gcc build via $(PSPSDK)/lib/build.mak
build.bat           Windows wrapper (delegates to WSL)
```

---

## Security note

With `tls=no` (default), this miner speaks plaintext stratum-v1 to
whatever host you point `host=` at and submits your worker credentials
unencrypted. Any on-path attacker who can MITM your WLAN can swap
the pool out for theirs and claim any shares you submit. Passive
observers see your address.

With `tls=yes` (since v0.4), the wire is encrypted (TLS 1.2 via
mbedtls), but the pool's certificate is **not verified**. An active
MITM with a forged cert can still redirect shares — they just need
to do a real-time TLS handshake of their own. Passive observers see
nothing.

Real verification needs a CA bundle shipped with the EBOOT. Tracked
in **Open work**; until it lands, treat TLS as "obscures the protocol
from passive observers" not "authenticates the pool."

For demo / educational use either mode is fine. **Don't point this
at real-money mining infrastructure on an untrusted network**
regardless of `tls=` setting until the verification work lands.

---

## What's new in v0.4

- **TLS via mbedtls** — enables `stratum+tls://` pool endpoints. Set
  `tls=yes` in `params.txt` (or flip `DEFAULT_POOL_TLS` to 1 in
  `source/main.c`) and the connection wraps a real TLS 1.2 handshake
  over the existing TCP socket. SNI is sent based on the configured
  hostname. The full reconnect/backoff loop from v0.3 is TLS-aware
  (clean teardown + re-handshake on every retry).
- **Size**: EBOOT.PBP went from 156 KB to ~660 KB — bigger than the
  v0.3 README's "~150 KB" estimate because mbedtls links all three of
  `libmbedtls` / `libmbedx509` / `libmbedcrypto` statically (handshake,
  cert parsing, symmetric+asymmetric primitives are pulled in even if
  we don't currently use verification). Still well under 1 MB; not a
  factor on the PSP's 32 MB RAM.

> **Security trade-off**: v0.4's TLS handshake encrypts the wire but
> does **not verify** the pool's certificate (`MBEDTLS_SSL_VERIFY_NONE`).
> Active MITM on the same WLAN can still redirect your shares to
> their pool. A future v0.5 with a shipped CA bundle would close that
> gap. For now, treat TLS as "obscures the protocol from passive
> observers" rather than "authenticates the pool."

## What's new in v0.3

- **Reconnect + exponential backoff.** A dropped TCP socket no longer
  kills the miner. The connect → subscribe → first-job sequence is
  wrapped in an outer reconnect loop with exponential backoff
  (1s → 2s → 4s → … capped at 60s, reset to 1s after every successful
  session). Detection covers both blocking failures
  (`stratum_wait_first_job` returning negative on dead socket) and
  silently-dropped TCP during mining — the inner loop now polls
  `stratum_socket_alive()` (a `MSG_PEEK | MSG_DONTWAIT` recv probe)
  every 16k iterations, so a half-closed connection is noticed within
  a couple of seconds instead of hanging forever waiting for the next
  `mining.notify` that will never come.
- **Per-worker config via `params.txt`** — drop a key=value text file
  at `ms0:/PSP/SAVEDATA/btc-miner-psp/params.txt` to override pool
  host / port / user / pass without rebuilding. Each key is independent;
  missing keys fall back to the compiled defaults. See
  [`params.txt.example`](params.txt.example) for the format and a
  starter file.
- **Boot log lists effective config**, so you can verify "did params.txt
  actually load" before any network attempts:
  ```
  config: loaded from params.txt: host port user
    pool: solo.ckpool.org:3333 user=bc1q...psp
  ```

## What was in v0.2

- **Variable difficulty handling** — `mining.set_difficulty` acts on
  pool's `diff` via `target_from_difficulty()`
  (`target = floor(bdiff_1 / diff)`); per-nonce share check is the
  full 256-bit `hash <= target` comparison. Mid-job updates land in
  place without restarting.
- **`mining.set_extranonce`** — subscription state updated when the
  pool reassigns our work slot; the next `mining.notify` builds
  against the new values automatically.
- **Correctness fix**: v0.1's "diff-1 check" was actually
  `hash2[31..28] < 0xFFFF0000`, which accepted ~99.6% of hashes
  rather than the ~1 in 2³² diff-1 actually requires.

## Open work

- **TLS cert verification** — ship a CA bundle (Mozilla root store
  is ~200 KB; could subset to "pools we know about"), turn on
  `MBEDTLS_SSL_VERIFY_REQUIRED`, plumb through SNI's CN match. Closes
  the active-MITM gap left open by v0.4.
- **Optional MIPS asm SHA-256 inner loop.** psp-gcc's compiled
  SHA-256 is pretty good (5 KB code, 5k cycles/block), but a
  hand-tuned MIPS asm version with software pipelining could
  probably hit 2x. See discussion in the
  [hash-bench-n64-optimized README](https://github.com/dmang-dev/hash-bench-n64-optimized)
  for the related VR4300 / MIPS3 perf-tuning lessons — VR4300 and
  the PSP R4000 share a CPU family.
- **RSP-style mining**: the PSP has no RSP, but it does have a Media
  Engine (a second MIPS R4000 + vector unit) accessible via
  `pspme`. Software miner already only uses 50% of one CPU; offloading
  to ME could ~double effective hashrate. Pure stunt territory.

---

## Acknowledgments

- [pspdev / pspsdk](https://pspdev.github.io/) — keeping the PSP
  homebrew toolchain alive in 2026
- [ckpool](https://bitbucket.org/ckolivas/ckpool) — public solo
  mining infrastructure
- [hash-bench-3ds](https://github.com/dmang-dev/hash-bench-3ds) /
  [hash-bench-n64](https://github.com/dmang-dev/hash-bench-n64) —
  SHA-256 source algorithm (byte-identical port)
- The 2013-era PSP miner scene that proved this was viable in the
  first place

---

## License

MIT. Algorithm implementations are reference (FIPS 180-4 SHA-256
re-typed for `<stdint.h>` portability). The stratum client is
original code based on public protocol documentation.
