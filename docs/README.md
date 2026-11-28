# Documentation index

Everything in this folder is engineering documentation for Ath10kMac — the reverse engineering of
Apple's Wi-Fi stack, the contracts this driver implements, and the measurements behind every claim in
the [README](../README.md).

## Read first

| # | Document | Why |
|---|---|---|
| 0 | [handoff-current-status.md](handoff-current-status.md) | **Entry point.** Where the project actually is: verified numbers with their source, the eight rules that must not be broken and why, the traps, the open work with acceptance criteria, and the runbook. |
| 1 | [handoff-apple-join-conditions.md](handoff-apple-join-conditions.md) | **The core engineering problem.** Every condition Apple requires before it admits a join — airportd → `Apple80211Associate2` → `IO80211Family` → driver — with the disassembly address and the measured value for each, plus the comparison against AirportItlwm. |
| 2 | [apple-vs-ours.md](apple-vs-ours.md) | Per-API diff: what the live stack expects vs. what this driver answers, and which of the two was wrong. |
| 3 | [reverse-engineering-playbook.md](reverse-engineering-playbook.md) | Toolchain and method: dumping a function out of the dyld shared cache, decoding chained fixups, resolving `__cfstring`/selref, reading a vtable by the right offset rule, and the traps that cost whole rounds. |
| 4 | [join-flow-reverse.md](join-flow-reverse.md) | The userspace call chain of one join, down to `Apple80211Associate2` and its own two `-3903` exits. |
| 5 | [HANDOFF.md](HANDOFF.md) | The round-by-round engineering log (77 rounds, §6.1–§6.67), with the measurement that justified each change. Long; read it for backstory, not for current state. |

## Reference by topic

### Contracts the driver implements

| Document | Contents |
|---|---|
| [../NATIVE-CONTRACT.md](../NATIVE-CONTRACT.md) | The IO80211 contract this kext implements: request entry points, vtable expectations, event posting. |
| [../WPA3-CONTRACT.md](../WPA3-CONTRACT.md) | WPA3-Personal / PMF requirements and what is wired. |
| [../SAE-CRYPTO-BACKEND.md](../SAE-CRYPTO-BACKEND.md) | In-kernel SAE crypto: symbol binding, P-256 backend, what is verified. |
| [../INITIAL-POWER-CONTRACT.md](../INITIAL-POWER-CONTRACT.md) | Power-on ordering, recovery after radio/VM restart. |
| [../COMPATIBILITY.md](../COMPATIBILITY.md) | macOS release matrix and ABI caveats. |

### Verification and limits

| Document | Contents |
|---|---|
| [hardware-verification.md](hardware-verification.md) | What was verified, on which machine (VM vs. laptop), with which command. The `Ath10k*` property field list. |
| [../NATIVE-CONNECTION-AUDIT.md](../NATIVE-CONNECTION-AUDIT.md) | Connection-state audit. |
| [../OFFLINE-VALIDATION-ERRATUM.md](../OFFLINE-VALIDATION-ERRATUM.md) | Why an offline kernel-collection check is not proof of a working kext. |

### History

| Document | Contents |
|---|---|
| [archive/legacy-background.md](archive/legacy-background.md) | The Ethernet-backed era of this project. Dead code path, kept for context. |
| [archive/native-sequoia.md](archive/native-sequoia.md) | The Sequoia native line and its ABI work. The live target is Ventura 13.7. |
| [archive/HANDOFF-v84.md](archive/HANDOFF-v84.md) | Snapshot of the handoff at v84 (2026-11-23). Superseded by [handoff-current-status.md](handoff-current-status.md). |

### Test rig

| Document | Contents |
|---|---|
| [archive/windows-build.md](archive/windows-build.md) | Cross-compiling the kext from Windows. Not the path in use: builds run on the Acer host. |
| [hostapd-qca.conf](hostapd-qca.conf) | The test AP used for every measurement in the log. |

## Reusable tools

All measurement tools live in [`../tools/`](../tools). The ones that matter most:

| Tool | Purpose |
|---|---|
| `a2push.c` | Push `ASSOCIATE(20)` through the `apple80211_ref` socket — proves whether the kernel path accepts a join at all. |
| `a2join.c`, `a2mon.c` | Call `Apple80211Associate2` directly, with and without event monitoring. |
| `a2dict.c` | Add one dictionary key at a time to find which one changes `Apple80211Associate2`'s behaviour. |
| `a2fd.c`, `refflags.c` | Dump the `apple80211_ref` layout and locate the capability mirror inside it. |
| `dump_a2.c`, `imgdump2.c` | Dump a function or an `__TEXT` segment out of the dyld shared cache for offline disassembly. |
| `resolve.py`, `annotate.py` | Resolve `__cfstring`/selref targets in a userspace binary (including chained fixups) and annotate an `otool -tV` range. |
| `cwscan.m`, `cwjoin.m` | CoreWLAN scan/join, i.e. the real airportd path. |
| `joincheck.sh`, `after.sh`, `whyfail.sh` | One join attempt → cwjoin verdict + driver breadcrumbs + airportd's own log lines. |

## Conventions used in the documents

- **Measurement, then change.** Every behavioural change in the log names the property, counter or log
  line that justified it. If a hypothesis was disproved, it stays in the document as disproved.
- **Addresses are image offsets** from the kernel cache (`0x1xxxxxx` live, `0x2xxxxxx` legacy) or from
  IO80211.framework (`0x8xxx`), not absolute runtime addresses; ASLR changes the latter on every boot.
- **vtable offsets are `[vptr + (ZTV index − 2) × 8]`.** Using `index × 8` silently addresses the wrong
  function — that mistake cost a round.
- **`Ath10k*` ioreg properties** are the interface between the driver and this documentation. When a
  document says "measured", it means one of those properties or a serial line.

## Where things live

```
docs/
  README.md                        this index
  handoff-current-status.md         entry point — read this first
  handoff-apple-join-conditions.md  the open engineering problem, layer by layer
  HANDOFF.md                       round-by-round log (§6.x), audit trail
  apple-vs-ours.md                 per-API diff against the live stack
  join-flow-reverse.md             one join, userspace to driver
  reverse-engineering-playbook.md  how the reversing was done
  hardware-verification.md         what was verified on which machine, `Ath10k*` property list
  hostapd-qca.conf                 the test AP every measurement in the log came from
  archive/                         superseded and off-target documents (see the History table)
```
