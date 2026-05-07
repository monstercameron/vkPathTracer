# Relay Yard Audio Asset Manifest

This manifest tracks the Level 01 audio set from `game/ASSETS.md` against the assets currently checked in under `assets/audio/` and the generated placeholder events declared by `assets/scenes/audio_lua_interaction_demo.json`.

Status values:

- `available`: file-backed audio exists and has attribution in `assets/audio/SOURCE.txt`.
- `placeholder`: scene event exists but uses a generated `tone:` URI.
- `missing`: required for the polished vertical slice but not authored yet.

## Weapons

| Event | Required asset | Status | Current source |
| --- | --- | --- | --- |
| `weapon.rifle.fire` | Assault rifle shot variants | placeholder | `tone:rifle.fire` |
| `weapon.pistol.fire` | Pistol shot variants | missing | None |
| `weapon.shotgun.fire` | Shotgun blast variants | missing | None |
| `weapon.lmg.fire` | LMG burst or loop | missing | None |
| `weapon.dry_fire` | Dry fire click | missing | None |
| `weapon.rifle.reload` | Rifle reload | missing | None |
| `weapon.pistol.reload` | Pistol reload | missing | None |
| `weapon.shotgun.reload_shell` | Shotgun shell insert | missing | None |
| `weapon.grenade.throw` | Grenade pin/throw | missing | None |
| `weapon.grenade.explosion` | Grenade explosion | missing | None |
| `weapon.bullet.whiz` | Bullet whiz | missing | None |

## Player And Enemy

| Event | Required asset | Status | Current source |
| --- | --- | --- | --- |
| `player.footstep.dirt` | Dirt/gravel footstep variants | available | `footstep_dirt_gravel_01.mp3`, `footstep_dirt_gravel_03.mp3`, `footstep_dirt_gravel_04.mp3`, `footstep_boots_dirt_01.mp3` |
| `player.footstep.metal` | Metal footstep variants | missing | None |
| `player.hurt` | Player hurt cue | missing | None |
| `player.armor_hit` | Armor hit cue | missing | None |
| `enemy.death` | Enemy death variants | missing | None |
| `enemy.alert` | Enemy alert bark | missing | None |
| `enemy.flank` | Enemy flank bark | missing | None |
| `enemy.reload` | Enemy reload bark | missing | None |
| `enemy.heavy.warning` | Heavy unit warning bark | missing | None |

## UI And Objectives

| Event | Required asset | Status | Current source |
| --- | --- | --- | --- |
| `ui.radar.ping` | Radar ping | placeholder | `tone:radar.ping` |
| `pickup.collect` | Pickup collect | placeholder | `tone:pickup.collect` |
| `objective.terminal.disabled` | Terminal disabled | placeholder | `tone:terminal.disabled` |
| `objective.update` | Objective update beep | missing | None |
| `objective.terminal.loop` | Terminal interaction loop | missing | None |
| `objective.alarm.siren` | Alarm siren | missing | None |
| `objective.extraction.flare` | Extraction flare | missing | None |
| `objective.mission_complete` | Mission complete sting | missing | None |

## Ambience And World Loops

| Event | Required asset | Status | Current source |
| --- | --- | --- | --- |
| `ambience.forest.loop` | Looping ambience test bed | available | `forest_ambience_loop.mp3` |
| `ambience.desert.wind` | Desert wind ambience | missing | None |
| `ambience.distant.combat` | Distant combat ambience | missing | None |
| `world.generator.hum` | Generator hum | placeholder | `tone:generator.hum` |
| `world.radio.chatter` | Radio chatter loop | missing | None |
| `world.fire.crackle` | Fire or smoke crackle | missing | None |

## Impacts

| Event | Required asset | Status | Current source |
| --- | --- | --- | --- |
| `impact.concrete` | Bullet impact concrete | missing | None |
| `impact.dirt` | Bullet impact dirt | missing | None |
| `impact.metal` | Bullet impact metal | missing | None |
| `impact.sandbag` | Bullet impact sandbag | missing | None |
| `impact.flesh` | Bullet impact flesh | missing | None |
| `impact.armor` | Bullet impact armor | missing | None |

## Release Gate

Before a polished Relay Yard audio pass ships, every scene-referenced audio file must have a row in `assets/audio/SOURCE.txt` with source URL, author, license, modification notes, and allowed usage. Placeholder `tone:` events are acceptable only for prototype scenes and must remain listed here until replaced.
