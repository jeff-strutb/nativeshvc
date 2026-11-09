# The contract

`docs/api.md` is the contract between a game and the engine. `NSHVC_VERSION_MAJOR`, `_MINOR` and `_PATCH` in `nshvc/nshvc.h`
say which version a build is; this one is 4.4.

- A **major** release is the only one that may change or remove anything a game uses.
- A **minor** release may only add calls, settings, or fields at the end of a struct that a zeroed struct leaves off.
- A **patch** release fixes where the engine differs from the SNES's rules, checked against bsnes.

Every struct a game fills is zeroed to mean the default.
