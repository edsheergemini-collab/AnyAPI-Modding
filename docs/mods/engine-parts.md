# AnyEngineParts

> AnyEngineParts hooks game functions directly through the [experimental SDK](../../sdk/experimental/README.md),
> so it only activates on Anymaker 0.1.25 / Steam build 25904830 and turns itself off on any other build.
> **Prototype, single player only** for now: it needs AnyAPI 0.37.0 or newer, and is built from source, not in
> the catalog yet.

AnyEngineParts adds engine parts to the game, in one mod (`AnyEngineParts.dll`):

- [Turbochargers](turbo.md) in four sizes (2x2x2 to 5x5x2), driven by the engine's exhaust, with Small and Large
  inline blow-off valves, a wastegate with microcontroller-controlled antilag, boost data channels and
  synthesized whistle, surge, blow-off and antilag sounds.
- A [rotary engine](rotary.md), stretchable from 1 to 4 rotors in the small engine's footprint, with its own
  housing mesh and synthesized sound. It works alongside EngineSound.

It replaces the separate Turbo and Rotary prototypes. The part ids are unchanged, so vehicles built with those
load as before. Remove `Turbo.dll` and `Rotary.dll` when installing it, or their parts are hooked twice.

The two halves start on their own: if one cannot find the game functions it needs, the other still works. Their
lines in `anymaker_modding.log` keep the tags `turbo` and `rotary`.

Source: [`engine_parts_mod.cpp`](../../native/engine_parts_mod.cpp), which starts
[`turbo_mod.cpp`](../../native/turbo_mod.cpp) and [`rotary_mod.cpp`](../../native/rotary_mod.cpp).
