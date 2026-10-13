# Turbo

> Turbo hooks game functions directly through the [experimental SDK](../../sdk/experimental/README.md),
> so it only activates on Anymaker 0.1.24 / Steam build 25826614 and turns itself off on any other build.
> Turbo 0.1.0 is a prototype: it needs AnyAPI 0.35.0 or newer on that build. **Single player only** for now.

Turbo adds four exhaust-driven turbochargers: **Turbo 2x2**, **3x3**, **4x4** and **5x5**. Each is N cells wide,
N tall and 2 thick. The front cell is the stock impeller's red compressor housing, scaled to the size; the back
cell is a heat-rust turbine housing, the same impeller mirrored.

## Hooking one up

| Port | Where | Connect to |
| --- | --- | --- |
| Intake in (cold side) | Middle of the front face | Nothing needed: it draws from the open air. A pipe here can only add air. |
| Boost out (cold side) | Top corner of the front housing, facing sideways | The engine's air intake |
| Exhaust in (hot side) | Bottom corner of the back housing, facing the other way | The engine's exhaust |
| Exhaust out (hot side) | Middle of the back face | A gas vent (the tailpipe) |

Exhaust flowing from the hot inlet to the hot outlet spins the shaft. The shaft's spool sets how much pressure
the cold side pushes into the engine's intake. Engines already turn intake pressure into power: ambient gives
1x, and an intake pressure of 98 kPa (the game's scale) gives the full 2x.

Turbos 4x4 and 5x5 use large pipes; 2x2 and 3x3 use small pipes. No gas tank is needed. The compressor inlet is open to the air like a real turbo intake, because the game's vents and filters only pass about 13.5 L/s, far less than a spooled turbo draws.

## Sizes and lag

| Size | Exhaust for full boost | Spool-up time | Shaft speed at full boost |
| --- | --- | --- | --- |
| 2x2 | 10 L/s | about 0.5 s | 220,000 rpm |
| 3x3 | 22 L/s | about 0.9 s | 160,000 rpm |
| 4x4 | 40 L/s | about 1.5 s | 120,000 rpm |
| 5x5 | 65 L/s | about 2.4 s | 90,000 rpm |

Pick the size whose full-boost flow is close to your engine's exhaust L/s at the throttle you use. Boost starts
building at a quarter of that flow and reaches full boost at the rated flow. Bigger turbos take longer to spool
(more lag) and spool down at half their spool-up time.

A turbo that is too small for its engine chokes it: exhaust backs up, and above 39 kPa of exhaust pressure the
engine loses power, as it does with any restricted exhaust. A turbo that is too big barely spools.

## Data channels

Wire them from a data port, a data link or a microcontroller. Pressures use the game's kPa scale (9.8 per unit).

| Channel | Direction | Value |
| --- | --- | --- |
| `intake_pressure` | Output | Boost pressure at the cold outlet, kPa |
| `exhaust_pressure` | Output | Pressure at the hot inlet, kPa |
| `power_multiplier` | Output | The engine power multiplier this boost gives, 1 to 2 |
| `spool` | Output | Shaft spool, 0 to 1 |
| `rpm` | Output | Shaft speed |
| `exhaust_flow`, `intake_flow` | Output | Hot-side and cold-side flow, L/s |
| `exhaust_temperature` | Output | Gas temperature at the hot inlet, °C |
| `boost_limit` | Input | A wastegate: caps boost at this pressure in kPa (0 or unwired: no cap) |

## Getting it

- **Survival:** world loot at tech tier 3 in the gas category, like the stock impellers.
- **Sandbox and Creative:** with the gas parts.

## How it works

- The four part definitions are built in the DLL ([`turbo_logic.h`](../../native/turbo_logic.h)). At start-up they
  are written to `AnyAPI and Modding/Turbo/turbochargers.json` and added to the game's vehicle components right
  after the game adds its own. Their ids are `turbocharger_2` to `turbocharger_5`, class `gas_pump_centrifugal`
  (the stock impeller's), with four gas logic nodes instead of two.
- The meshes are built at start-up from the game's `rom/meshes/components/turbo_a.mesh` and written beside it as
  `turbo_turbocharger_N.mesh` ([`turbo_mesh.h`](../../native/turbo_mesh.h)). If the stock mesh is not in the
  expected format, that size uses the plain impeller look.
- The impeller's server create, destroy, tick and `get_data_f64` are hooked. When a turbo is created, the mod
  creates its hot side with the game's own functions: two gas contents and two gas nodes registered in the
  vehicle, so pipes and pressure treat them like any other port. Stock impellers are untouched.
- Each tick the turbo lets exhaust through its hot side in proportion to the pressure drop, spools from that
  flow with its size's lag, and runs its compressor with a head set by the spool (full spool gives the engine's
  full boost).
- The hot side's gas is not saved; it refills from the pipes after loading. Saves that contain a turbo need
  Turbo installed to load it.

Source: [`turbo_mod.cpp`](../../native/turbo_mod.cpp), bindings in
[`turbo_bindings.h`](../../native/turbo_bindings.h), tests in
[`tests/turbo_logic_test.cpp`](../../native/tests/turbo_logic_test.cpp).
