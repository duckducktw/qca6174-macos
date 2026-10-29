# QCA6174 macOS Driver Project

Read `TASK_QCA6174_DRIVER.md` completely before changing files.

- Truthful milestones matter more than appearance. Loading a kext is not Wi-Fi functionality.
- Keep host-side parsing/testable logic separate from macOS IOKit glue.
- Never modify a mounted EFI, NVRAM, or remote machine from this repository.
- Do not commit or push; the parent will review first.
