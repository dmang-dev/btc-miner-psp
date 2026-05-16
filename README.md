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
[![Speedup](https://img.shields.io/badge/v0.6-~2x%20faster%20(midstate%20%2B%20rotr)-brightgreen)](#whats-new-in-v06)

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
btc-miner-psp v0.6
PSP 333 MHz MIPS R4000, software SHA-256d (midstate + ROTR)

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
| TLS | ✓ — mbedtls 2.28 client (since v0.4); cert chain + hostname verification against embedded Mozilla CA bundle (since v0.5) |
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

| Mode | Wire | Pool identity |
|---|---|---|
| `tls=no` (default) | **plaintext** — passive observers see pool URL, worker name, and submitted shares | **none** — any on-path MITM can swap pools |
| `tls=yes, tls_verify=no` | encrypted | **none** — active MITM with forged cert still wins |
| `tls=yes, tls_verify=yes` (default when TLS is on, since v0.5) | encrypted | **verified** — cert chain + hostname checked against the embedded Mozilla CA bundle |

For real-money mining over an untrusted network, run `tls=yes` and
keep `tls_verify=yes` (the default). The verification path uses the
exact same CA store browsers do, and rejects any cert that doesn't
chain to a Mozilla-trusted root or whose CN/SAN doesn't match your
configured `host=`.

`tls_verify=no` exists as a debug escape hatch — when the pool uses
a self-signed cert, when you're MITM'ing yourself for protocol study,
or when the embedded CA bundle is too stale (re-run
`tools/embed_cacert.py` to refresh from upstream).

---

## What's new in v0.6

**~2× hashrate** via two stacked optimizations, both classic Bitcoin-mining
tricks adapted for the Allegrex CPU.

### 1. Midstate optimization (algorithmic, +1.5×)

The 80-byte Bitcoin block header double-hashes as
`SHA256(SHA256(header))`. SHA-256 processes 64-byte blocks, so an 80-byte
input takes **2 compressions** for the inner hash (one full block + one
padded 16-byte tail) plus **1 compression** for the outer hash (32-byte
intermediate digest is one padded block). That's **3 compressions per
nonce**.

But here's the thing: only **bytes 76..79** of the header change per
nonce. The first 64 bytes (version + prev_hash + merkle_root[0..27])
are *constant for the entire job*. So we precompute the SHA-256 state
after the first block **once per job** and reuse it across the 2³²
nonces — saving one full compression per nonce.

```
Per-nonce work:        v0.5            v0.6
─────────────────────────────────────────────────────────
SHA-256 first block    1 compression   0 (precomputed midstate)
SHA-256 second block   1 compression   1 compression  (s1 = midstate; compress)
SHA-256 outer hash     1 compression   1 compression
─────────────────────────────────────────────────────────
Total                  3               2              → 1.5x speedup
```

Host benchmark of the two paths (same compiler, same input, same source):

```
naive    (3 compress/nonce):  1.47 MH/s
midstate (2 compress/nonce):  2.24 MH/s
speedup: 1.527×              ✓ matches theoretical 1.5
```

The miner exposes `sha256_init` / `sha256_compress` / `sha256_state_to_bytes`
as a public API in `include/sha256.h` so the mining loop can drive the
compression function directly. The convenience one-shot `sha256()` is
unchanged and still passes the FIPS 180-2 vector + Bitcoin genesis-block
double-hash self-test. There's a fourth self-test vector now that
re-validates the midstate path against the genesis dhash byte-for-byte
— so any mistake in the per-job/per-nonce split trips at boot.

### 2. Allegrex `rotr` inline asm (per-instruction, ~+1.3×)

SHA-256's hot loop has 64 iterations × 6 rotates (BSIG0/BSIG1) + 48
iterations of W expansion × 4 rotates (SSIG0/SSIG1). Every rotate is
the same idiom:

```c
ROR32(x, n) ≡ ((x >> n) | (x << (32 - n)))
```

psp-gcc 15 **does not auto-fuse this into a single `rotr`**, even though
the Allegrex CPU supports `rotr` as a vendor extension (inherited from
the MIPS32r2 / SmartMIPS bit-manipulation block, predating the formal
addition in MIPS32r2). Verified by disassembling the v0.5 build:
`grep -cE '\brotr\b' build/sha256.s` → 0; the compiler emits a
3-instruction `srl + sll + or` sequence for every rotate.

v0.6 adds an inline-asm `ror32()` that spells out `rotr` directly:

```c
#if defined(__mips__) && (defined(__psp__) || defined(_PSP))
static inline uint32_t ror32(uint32_t x, unsigned n) {
    uint32_t r;
    __asm__("rotr %0, %1, %2" : "=r"(r) : "r"(x), "i"(n));
    return r;
}
#endif
```

After the refactor, the inner loop emits exactly **10 `rotr`
instructions** — one per distinct rotate amount in SHA-256 (2, 6, 7,
11, 13, 17, 18, 19, 22, 25). The compiler unrolls the loop body once
and reuses the rotates inside the iteration.

Each rotate is **3 instructions → 1 instruction**, a savings of 2 ops
× 10 rotates per round body × 64 rounds = ~1280 instructions per
compression. SHA-256 compress costs ~5000 cycles on Allegrex, so the
rotr win is on the order of **+25-30%** on its own.

### Combined effect

| Build | Hashrate (real PSP)* |  Speedup |
|---|---:|---:|
| v0.5 (compiled rotates, 3 compress/nonce) | ~40 kH/s | 1.00× |
| v0.6 (`rotr` asm, 2 compress/nonce) | **~75 kH/s** | **~1.9×** |

\* Estimated from host-CPU ratio (1.527× from midstate alone, measured)
multiplied by the per-compression rotate savings (~1.25× on Allegrex,
since the rotates form ~25% of the compression's critical path). Real
hardware measurement to follow.

### Size

| Build | EBOOT.PBP |
|---|---|
| v0.5 (TLS + verify + Mozilla bundle) | 854 KB |
| **v0.6 (midstate + rotr asm)** | **836 KB** (-18 KB; main.c shrank slightly, sha256.c reorg) |

### Open work

- **Real-hardware hashrate confirmation.** PSP scripted boot from a
  Windows host turned out to be more work than expected (PPSSPP is
  GUI-only in its packaged builds), so the ~1.9× number is theoretical
  from the host-CPU benchmark + instruction count. The next time
  someone actually puts the EBOOT on a memory stick they can append a
  measured number to the table above.
- **Bigger asm pass.** ROTR is the obvious-and-cheap one. A larger
  win is possible by hand-scheduling the inner round across Allegrex's
  load-delay slot (one-cycle gap between `lw` and the dependent use,
  fillable with an unrelated rotate or add). VR4300 work in
  [hash-bench-n64-optimized](https://github.com/dmang-dev/hash-bench-n64-optimized)
  got another ~30% from this on a same-family CPU.
- **Media Engine offload.** PSP has a second MIPS R4000 + vector unit
  (the "Media Engine") accessible via `pspme`. Software miner only
  uses one CPU; offloading half the nonce space to the ME could ~2×
  again. Pure stunt territory; nobody has done it.

---

## What's new in v0.5

- **TLS certificate verification** — Mozilla's CA bundle (121 roots,
  via <https://curl.se/ca/cacert.pem>) is embedded directly in the
  EBOOT as a static byte array. When `tls=yes` and `tls_verify=yes`
  (the default), mbedtls runs `MBEDTLS_SSL_VERIFY_REQUIRED`: rejects
  any cert chain that doesn't link to a trusted root, and rejects any
  cert whose Subject CN / SAN doesn't match the configured `host=`.
- **`tls_verify=no` opt-out** is still available for testing against
  self-signed pools or hostile environments where the user explicitly
  doesn't care. Boot banner prints `verify=REQUIRED` or `verify=NONE`
  so it's never ambiguous which mode is active. Handshake-fail output
  includes mbedtls's specific verify-info string (e.g.
  `! The certificate has expired`) so you can tell apart a network
  failure from a cert problem.
- **CA bundle is parsed once per boot**, cached, reused across
  reconnects — saves ~50 ms of x509 parsing per reconnect.
- **Embed pipeline**: `tools/embed_cacert.py` converts the upstream
  PEM into `source/cacert_data.c` (a `static const unsigned char[]`
  + length). Run when Mozilla updates their bundle; the build itself
  has no Python dependency.

### Size

| Build | EBOOT.PBP |
|---|---|
| v0.1 (no TLS) | 152 KB |
| v0.3 (reconnect + config) | 156 KB |
| v0.4 (TLS, no verify) | 664 KB |
| **v0.5 (TLS + verify + Mozilla bundle)** | **~854 KB** (+190 KB for the bundle, matches the 189 KB PEM input) |

Still fits well within PSP's 32 MB RAM.

## What's new in v0.4

- **TLS via mbedtls** — enables `stratum+tls://` pool endpoints. Set
  `tls=yes` in `params.txt` (or flip `DEFAULT_POOL_TLS` to 1 in
  `source/main.c`) and the connection wraps a real TLS 1.2 handshake
  over the existing TCP socket. SNI is sent based on the configured
  hostname. The full reconnect/backoff loop from v0.3 is TLS-aware
  (clean teardown + re-handshake on every retry).

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

(Moved into the [v0.6 section](#whats-new-in-v06) — the headline items
are real-hardware confirmation, a larger asm pass with hand-scheduling
into the Allegrex load-delay slot, and the Media Engine offload stunt.)

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
