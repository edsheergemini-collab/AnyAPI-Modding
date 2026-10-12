# ThirdPerson

> ThirdPerson hooks a game function directly through the [experimental SDK](../../sdk/experimental/README.md),
> so it only activates on Anymaker 0.1.24 / Steam build 25826614 and turns itself off on any other build.
> Prototype: built from source, not in the catalog yet.

ThirdPerson adds a third-person camera with an adjustable follow distance.

## Using it

| Control | What it does |
| --- | --- |
| **F5** | Switch between first and third person |
| **Alt + mouse wheel** | In third person, move the camera closer or further (1 to 20 m, 0.5 m per notch) |

A short popup in the lower middle of the screen shows the camera mode and distance after each change.
With [AnyHelpers](helpers.md) installed, F5 can be rebound in **Controls → ThirdPerson**, and **Settings → ThirdPerson** has:

| Setting | Default | Meaning |
| --- | --- | --- |
| Start in third person | Off | Use third person from the moment the game starts |
| Follow distance (m) | 4.0 | The distance used when you switch to third person. Alt + wheel changes it until you next apply settings |

Without AnyHelpers the mod still works with F5 and a 4 m default distance.

## How it works

Anymaker 0.1.24 already contains a third-person camera that only its developer tools switch on: the client
scene has a "camera first person" flag, a "render the player in third person" flag and a third-person camera
distance, and `client_scene.get_camera_transform` (used by rendering, the crosshair and interaction rays)
applies them. ThirdPerson hooks that function and sets those three values, so the game draws your character's
body and aims interactions from the camera with its own rules.

For the first frames in third person the mod checks the camera's offset against your character's facing. If the
game's offset points forward it flips the distance; if the game does not move the camera at all, the mod moves it
back along the view itself. Both cases are written to `anymaker_modding.log`.

## Known limits

- The camera does not avoid walls yet, so it can clip through them in tight spaces. Lower the distance indoors.
- Aiming and interactions use the camera, as in the game's own third-person mode, so very close objects can be
  harder to target than in first person.
- Only your own view changes. Other players are not affected and do not need the mod.
- The client-scene field offsets come from the 0.1.24 SDK reference; after a game update, re-check them as well as
  the binding in `native/thirdperson_bindings.h`.
