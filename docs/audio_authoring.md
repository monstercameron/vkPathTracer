# Audio Authoring

The engine audio path is intentionally small: scene files declare audio assets and listener/emitter components, Lua scripts post named events, and `IAudioSystem` resolves those events through either the `noop` backend or the miniaudio-backed runtime when audio is enabled.

## Build And Runtime Controls

- `PT_ENABLE_AUDIO=OFF` keeps miniaudio out of the build.
- `PT_ENABLE_AUDIO=ON` enables the audio runtime.
- `PT_ENABLE_MINIAUDIO=ON` allows the miniaudio backend to be built when audio is enabled.
- `PT_AUDIO_BACKEND=auto|miniaudio|noop` selects the configured default backend policy.
- `--audio-backend noop` initializes diagnostics and event resolution without opening an output device.
- `--audio-backend miniaudio` or `--audio-backend auto` uses the default miniaudio output device when available.
- `--audio-mute` keeps event dispatch active while forcing the runtime through the no-output path.

Benchmarks and CI should use `--audio-backend noop` or `--audio-mute` so render timing does not depend on hardware devices, operating-system mixer latency, or missing speakers.

## Supported Asset URIs

Scene audio assets use the existing asset list. Any asset whose `type` contains `audio` or `sound` is picked up by `AudioSystem::load_scene_audio`.

Supported URI styles:

- Relative files such as `../audio/footstep_dirt_gravel_01.mp3`.
- Absolute file paths for local debugging.
- Generated placeholders such as `tone:rifle.fire` and `tone:pickup.collect`.

The miniaudio path delegates file decoding to miniaudio. The current demo uses MP3 files and generated tones. Missing files do not crash playback; they log a warning and resolve to a generated placeholder tone.

## Event Assets

The event name is the normalized `name` field. Multiple assets with the same normalized name become variants of the same event.

Minimal non-spatial UI event:

```json
{
  "name": "ui.radar.ping",
  "type": "audio/event/ui",
  "uri": "tone:radar.ping"
}
```

Spatial generator event:

```json
{
  "name": "world.generator.hum",
  "type": "audio/event/spatial",
  "uri": "tone:generator.hum"
}
```

Random-variant weapon event:

```json
[
  {
    "name": "weapon.rifle.fire",
    "type": "audio/event/spatial",
    "uri": "../audio/rifle_fire_01.wav"
  },
  {
    "name": "weapon.rifle.fire",
    "type": "audio/event/spatial",
    "uri": "../audio/rifle_fire_02.wav"
  }
]
```

Looping ambience event:

```json
{
  "name": "ambience.forest.loop",
  "type": "audio/stream/ambience",
  "uri": "../audio/forest_ambience_loop.mp3"
}
```

## Buses

The runtime currently derives a default logical bus from the asset type:

- `sfx`: default for event assets.
- `ui`: asset type contains `ui`.
- `ambience`: asset type contains `stream`, `music`, or `ambience`.
- `voice`: asset type contains `voice`.

These buses currently apply default gain only. Full editor-visible mixer state, muting, soloing, and fades remain backlog work.

## Components

`AudioListenerComponent` marks the active listener entity:

```json
"audio_listener": {
  "enabled": true,
  "primary": true
}
```

`AudioEmitterComponent` starts positional or non-positional events from scene entities:

```json
"audio_emitter": {
  "event": "ambience.forest.loop",
  "bus": "ambience",
  "enabled": true,
  "autoplay": true,
  "loop": true,
  "spatial": true,
  "volume": 0.65,
  "pitch": 1.0,
  "min_distance": 2.0,
  "max_distance": 18.0
}
```

Autoplay emitters post during scene audio load. Runtime listener pose is updated from the Qt camera each frame, and game scripts can also move a listener entity in game mode.

## Lua API

Scripts can post an event through the game-mode-only script context:

```lua
ctx.audio:post_event("player.footstep.dirt", {
  entity = player.id,
  position = player.transform.translation,
  spatial = true,
  volume = 0.85,
  pitch = 1.0,
})
```

Scripts do not receive backend objects or file paths. Use event names and entity-relative positions.

## Adding A Pickup Sound

1. Add the file under `assets/audio/`.
2. Record source URL, author, license, modification notes, and allowed usage in `assets/audio/SOURCE.txt`.
3. Add a scene asset:

```json
{
  "name": "pickup.collect",
  "type": "audio/event/ui",
  "uri": "../audio/pickup_collect.wav"
}
```

4. Post it from Lua when the pickup state changes:

```lua
ctx.audio:post_event("pickup.collect", {
  entity = pickup.id,
  spatial = false,
  volume = 0.9,
})
```

5. Run the smoke test with the no-op backend path and open `assets/scenes/audio_lua_interaction_demo.json` in Qt for a manual real-device pass.

## Troubleshooting

- Event does not play: check that the scene asset `name` normalizes to the event name used by Lua or the emitter.
- File missing: check paths relative to the scene file, not the executable.
- No audible output: check `--audio-backend`, `--audio-mute`, `PT_ENABLE_AUDIO`, and the log line from audio initialization.
- Scripts do nothing in render/editor mode: press `F1` to enter game mode; scripts are intentionally blocked outside game mode.
