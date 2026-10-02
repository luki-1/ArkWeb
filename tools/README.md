# ArkWeb static-analysis tools

These need Python 3.12 with `pefile`, `capstone` and `numpy`. Importing `pe_tools` drops the process
to idle priority on one core.

| Script | Does |
|---|---|
| `dump_rtti.py <exe> <out.json>` | MSVC x64 RTTI: every class, its vtables and its bases |
| `dump_strings.py <exe> <out.txt>` | ASCII and UTF-16 strings with RVAs |
| `ue3_natives.py <exe> <out.json>` | UE3 `UClassexecFunc` native table, mapped to function RVAs |
| `strref.py <exe> <regex>` | Functions that reference strings matching the regex |
| `callers.py <exe> <rva>...` | Direct call/jmp sites of a function |
| `dis.py <exe> <rva>...` | Disassembles a function, with string, vtable and native-name hints |

Output goes to `../recon/`. Findings are in `../recon/PHASE0.md`.
