# Native Sequoia adapter

The default build is the full native kext (`make NATIVE=1 DIAGNOSTIC_STAGE=0 kext`),
using stock macOS Wi-Fi control interfaces. No companion app is shipped.
A threaded radio/BSD workloop and distinct WCL workqueue have separate ownership
and synchronization; see [the current contract](../NATIVE-CONTRACT.md).

The early stage-3 workloop correction from main is preserved along with subsequent
PM, termination, queue and connection fixes. The optional `DIAGNOSTIC_STAGE=3`
controller-only build exits before opening PCI, loading firmware or starting DMA.
It must not be confused with the full driver or installed as a functional release.

Native scanning and WPA2 traffic have been observed; current stability fixes need
hardware validation. WPA3/PMF, AWDL/AirDrop and other macOS ABI adapters remain
incomplete. See [connection audit](../NATIVE-CONNECTION-AUDIT.md),
[compatibility](../COMPATIBILITY.md) and [WPA3](../WPA3-CONTRACT.md).
