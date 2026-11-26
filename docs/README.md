# Documentation index

Everything in this folder is engineering documentation for Ath10kMac — the reverse engineering of
Apple's Wi-Fi stack, the contracts this driver implements, and the measurements behind every claim in
the [README](../README.md).

If you are new here, read in this order:

| # | Document | Why |
|---|---|---|
| 1 | [handoff-apple-join-conditions.md](handoff-apple-join-conditions.md) | **The core document.** Every condition Apple requires before it admits a join — airportd → `Apple80211Associate2` → `IO80211Family` → driver — with the disassembly address and the measured value for each. Also the community comparison (AirportItlwm) and the exact point the project is stuck on. |
| 2 | [apple-vs-ours.md](apple-vs-ours.md) | Per-API diff: what the live stack expects vs. what this driver answers, and which of the two was wrong. |
| 3 | [reverse-engineering-playbook.md](reverse-engineering-playbook.md) | Toolchain and method: dumping a function out of the dyld shared cache, decoding chained fixups, resolving `__cfstring`/selref, reading a vtable by the right offset rule, and the traps that cost whole rounds. |
| 4 | [join-flow-reverse.md](join-flow-reverse.md) | The userspace call chain of one join, down to `Apple80211Associate2` and its own two `-3903` exits. |
| 5 | [HANDOFF.md](HANDOFF.md) | The full engineering log, round by round (currently 44 rounds), with the measurement that justified each change. Long, but it is the audit trail. |

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
| [legacy-background.md](legacy-background.md) | The Ethernet-backed era of this project. |
| [native-sequoia.md](native-sequoia.md) | The Sequoia native line and its ABI work. |
| [HANDOFF-v84.md](HANDOFF-v84.md) | Snapshot of the handoff at v84, kept for diffing. |

### Per-machine notes

| Document | Contents |
|---|---|
| [windows-build.md](windows-build.md) | Cross-compiling the kext from Windows. |
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
