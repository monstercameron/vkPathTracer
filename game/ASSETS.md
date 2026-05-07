Below is a practical **asset list for Level 01: Relay Yard**, grouped by what you would actually need to build the vertical slice.

# Level 01 Asset List

## 1. Player Assets

| Asset                               | Type         | Notes                                       |
| ----------------------------------- | ------------ | ------------------------------------------- |
| Player soldier model                | 3D character | Top-down readable silhouette, tactical gear |
| Player rifle pose                   | Animation    | Idle with assault rifle                     |
| Player pistol pose                  | Animation    | Optional if sidearm is visible              |
| Player shotgun pose                 | Animation    | Optional but preferred                      |
| Walk/run animations                 | Animation    | 8-direction or blend-space movement         |
| Strafe animations                   | Animation    | Moving while aiming separately              |
| Fire weapon animation               | Animation    | Upper-body recoil animation                 |
| Reload rifle animation              | Animation    | Can be simple for demo                      |
| Reload shotgun animation            | Animation    | Shell-by-shell if possible                  |
| Grenade throw animation             | Animation    | Quick overhand throw                        |
| Dash/roll animation                 | Animation    | For Space evade                             |
| Hurt animation                      | Animation    | Small flinch                                |
| Death animation                     | Animation    | Optional ragdoll or collapse                |
| Player aiming reticle               | UI/VFX       | Ground-plane cursor/crosshair               |
| Player selection/indicator triangle | UI/VFX       | Small blue/white marker above player        |

---

## 2. Enemy Assets

### Rifleman

| Asset                | Type         | Notes                              |
| -------------------- | ------------ | ---------------------------------- |
| Enemy rifleman model | 3D character | Red/black or hostile color accents |
| Rifleman idle/patrol | Animation    | Basic patrol                       |
| Rifleman run         | Animation    | Move to cover                      |
| Rifleman fire        | Animation    | Burst fire                         |
| Rifleman reload      | Animation    | Optional                           |
| Rifleman death       | Animation    | Collapse/ragdoll-lite              |
| Enemy red marker     | UI/VFX       | Diamond above enemy when detected  |

### Rusher

| Asset                     | Type         | Notes                                |
| ------------------------- | ------------ | ------------------------------------ |
| Rusher model              | 3D character | Lighter armor, aggressive silhouette |
| Rusher sprint animation   | Animation    | Must read as fast                    |
| Rusher fire animation     | Animation    | SMG/shotgun close fire               |
| Rusher death animation    | Animation    | Quick collapse                       |
| Rusher radar icon variant | UI           | Red diamond with motion cue          |

### Heavy

| Asset                   | Type         | Notes                     |
| ----------------------- | ------------ | ------------------------- |
| Heavy model             | 3D character | Larger armored silhouette |
| Heavy LMG weapon        | 3D prop      | Big readable gun          |
| Heavy walk animation    | Animation    | Slow, weighted            |
| Heavy fire animation    | Animation    | Sustained firing          |
| Heavy stagger animation | Animation    | For grenade impact        |
| Heavy death animation   | Animation    | Dramatic fall             |
| Heavy warning icon      | UI           | Larger red diamond        |
| Heavy armor hit VFX     | VFX          | Sparks/armor impacts      |

---

## 3. Weapon Assets

| Asset                   | Type    | Notes                            |
| ----------------------- | ------- | -------------------------------- |
| Assault rifle model     | 3D prop | Player/enemy usable              |
| Pistol model            | 3D prop | Sidearm                          |
| Shotgun model           | 3D prop | Pickup and player weapon         |
| LMG model               | 3D prop | Heavy enemy weapon               |
| Grenade model           | 3D prop | Throwable and HUD icon           |
| Rifle pickup icon/model | 3D/UI   | Optional if rifle can be swapped |
| Shotgun pickup model    | 3D prop | Purple glow in Motor Pool        |
| Ammo box model          | 3D prop | Green glow                       |
| Medkit model            | 3D prop | Cyan/white glow                  |
| Armor plate model       | 3D prop | Blue glow                        |
| Grenade pickup model    | 3D prop | Green/lime glow                  |

---

## 4. Environment Assets

## Terrain

| Asset                       | Type             | Notes                             |
| --------------------------- | ---------------- | --------------------------------- |
| Desert dirt ground material | Material/texture | Main terrain                      |
| Gravel/dust decal set       | Decal            | Breaks up empty ground            |
| Tire track decals           | Decal            | Roads/motor pool                  |
| Oil stain decals            | Decal            | Near vehicles                     |
| Blood decals                | Decal            | Combat feedback                   |
| Scorch mark decals          | Decal            | Explosions                        |
| Bullet impact decals        | Decal            | Concrete/metal/sand/dirt variants |

## Cover Objects

| Asset                          | Type    | Notes                 |
| ------------------------------ | ------- | --------------------- |
| Concrete barrier short         | 3D prop | Core cover piece      |
| Concrete barrier long          | 3D prop | Used in kill lane     |
| Sandbag stack low              | 3D prop | Low cover             |
| Sandbag stack high             | 3D prop | Enemy firing nest     |
| Wooden crate small             | 3D prop | Destructible          |
| Wooden crate large             | 3D prop | Cover/filler          |
| Metal crate                    | 3D prop | Hard cover            |
| Cargo container                | 3D prop | High cover / boundary |
| Jersey barrier damaged variant | 3D prop | Visual variety        |
| Crate pile prefab              | Prefab  | Quick level dressing  |
| Sandbag nest prefab            | Prefab  | Enemy positions       |

## Military Props

| Asset                   | Type    | Notes                    |
| ----------------------- | ------- | ------------------------ |
| Parked military truck   | 3D prop | High cover               |
| Damaged armored vehicle | 3D prop | Player start cover       |
| Burned-out car/truck    | 3D prop | Cover + atmosphere       |
| Generator unit          | 3D prop | Near terminals           |
| Fuel barrel red         | 3D prop | Explosive                |
| Fuel barrel blue/gray   | 3D prop | Non-explosive dressing   |
| Floodlight pole         | 3D prop | Lighting prop            |
| Chain-link fence        | 3D prop | Boundary                 |
| Fence gate              | 3D prop | Extraction path          |
| Guard tower             | 3D prop | Landmark                 |
| Watch post shack        | 3D prop | Supply yard              |
| Military tent           | 3D prop | Motor pool/command area  |
| Antenna mast            | 3D prop | Background               |
| Satellite dish          | 3D prop | Final objective landmark |
| Communications tower    | 3D prop | Main visual goal         |
| Cable bundles           | 3D prop | Runs to terminals        |
| Warning signs           | 3D prop | “Restricted Area,” etc.  |

---

## 5. Objective Assets

| Asset                      | Type     | Notes                               |
| -------------------------- | -------- | ----------------------------------- |
| Terminal Alpha model       | 3D prop  | Small control box                   |
| Terminal Bravo model       | 3D prop  | Near comms truck                    |
| Terminal Charlie model     | 3D prop  | Tower base, more important          |
| Terminal active material   | Material | Yellow glow/emissive                |
| Terminal disabled material | Material | Dark/sparking state                 |
| Terminal interaction ring  | UI/VFX   | Hold E progress                     |
| Objective marker           | UI       | Yellow square/diamond               |
| Comms array damaged state  | 3D/VFX   | Sparks/smoke after completion       |
| Extraction flare           | 3D/VFX   | Purple or blue flare                |
| Extraction zone marker     | UI/VFX   | Ground circle or holographic marker |

---

## 6. VFX Assets

## Weapon VFX

| Asset                  | Type | Notes                  |
| ---------------------- | ---- | ---------------------- |
| Rifle muzzle flash     | VFX  | Short burst            |
| Pistol muzzle flash    | VFX  | Smaller                |
| Shotgun muzzle flash   | VFX  | Wide flash             |
| LMG muzzle flash       | VFX  | Sustained heavy fire   |
| Bullet tracer          | VFX  | Yellow/orange line     |
| Enemy tracer           | VFX  | Slightly red/orange    |
| Bullet impact dirt     | VFX  | Dust puff              |
| Bullet impact concrete | VFX  | Chips/sparks           |
| Bullet impact metal    | VFX  | Sparks                 |
| Bullet impact sandbag  | VFX  | Fabric/dust puff       |
| Hit marker spark/blood | VFX  | Enemy impact feedback  |
| Armor impact spark     | VFX  | Heavy/player armor hit |

## Explosion VFX

| Asset                | Type       | Notes                  |
| -------------------- | ---------- | ---------------------- |
| Grenade explosion    | VFX        | Flash, dust, smoke     |
| Barrel explosion     | VFX        | Fireball + shockwave   |
| Smoke plume          | VFX        | From destroyed props   |
| Lingering dust cloud | VFX        | After explosion        |
| Screen shake effect  | Camera/VFX | Small controlled shake |

## Pickup VFX

| Asset                   | Type | Notes                    |
| ----------------------- | ---- | ------------------------ |
| Green pickup glow       | VFX  | Ammo/grenade             |
| Cyan pickup glow        | VFX  | Medkit                   |
| Blue pickup glow        | VFX  | Armor                    |
| Purple weapon glow      | VFX  | Shotgun                  |
| Pickup sparkle/pulse    | VFX  | Makes items readable     |
| Pickup collection burst | VFX  | Small pop when collected |

## Objective VFX

| Asset                   | Type   | Notes                 |
| ----------------------- | ------ | --------------------- |
| Radar pulse ring        | VFX/UI | Scan effect           |
| Terminal spark effect   | VFX    | When sabotaged        |
| Terminal shutdown burst | VFX    | Small electric pop    |
| Comms tower smoke       | VFX    | After final objective |
| Extraction flare smoke  | VFX    | End zone visibility   |

---

## 7. UI Assets

## HUD

| Asset                         | Type | Notes                          |
| ----------------------------- | ---- | ------------------------------ |
| Objective panel background    | UI   | Top-left black translucent box |
| Objective icon                | UI   | Yellow diamond                 |
| Terminal count indicator      | UI   | 0/3, 1/3, etc.                 |
| Health bar                    | UI   | Bottom-left                    |
| Armor bar                     | UI   | Bottom-left                    |
| Weapon panel                  | UI   | Bottom-right                   |
| Ammo number font/style        | UI   | Large readable digits          |
| Grenade icon                  | UI   | Bottom-right                   |
| Reload progress indicator     | UI   | Could be radial                |
| Low ammo warning              | UI   | Small alert                    |
| Damage direction indicator    | UI   | Edge flash                     |
| Hit marker                    | UI   | Small cross/hit confirmation   |
| Interaction prompt            | UI   | “Hold E”                       |
| Interaction progress bar/ring | UI   | For terminals                  |
| Mission complete screen       | UI   | End-of-level report            |
| Pause menu                    | UI   | Minimum viable menu            |

## Radar UI

| Asset                         | Type | Notes                      |
| ----------------------------- | ---- | -------------------------- |
| Circular radar frame          | UI   | Top-right                  |
| Radar grid lines              | UI   | Subtle                     |
| Player triangle icon          | UI   | Blue/white                 |
| Enemy red diamond icon        | UI   | Rifleman/rusher            |
| Heavy red icon                | UI   | Larger red diamond         |
| Objective yellow square icon  | UI   | Terminal/objective         |
| Extraction purple square icon | UI   | Extraction                 |
| Pickup green square icon      | UI   | Nearby pickups             |
| Radar pulse animation         | UI   | Sweeping or expanding ring |
| Radar legend panel            | UI   | Optional for demo clarity  |
| Direction arrow to objective  | UI   | Can sit on radar edge      |

---

## 8. Audio Assets

## Weapons

| Asset                     | Type  | Notes               |
| ------------------------- | ----- | ------------------- |
| Assault rifle shot        | Audio | Several variations  |
| Pistol shot               | Audio | Several variations  |
| Shotgun blast             | Audio | Heavy and punchy    |
| LMG fire loop/burst       | Audio | Heavy enemy         |
| Dry fire click            | Audio | Empty mag           |
| Rifle reload              | Audio | Magazine swap       |
| Pistol reload             | Audio | Quick reload        |
| Shotgun reload shell      | Audio | Shell insert loop   |
| Grenade throw             | Audio | Pin/throw           |
| Grenade explosion         | Audio | Big but clean       |
| Bullet whiz               | Audio | Near-miss feedback  |
| Bullet impact concrete    | Audio | Cover feedback      |
| Bullet impact dirt        | Audio | Ground impact       |
| Bullet impact metal       | Audio | Vehicle/metal cover |
| Bullet impact flesh/armor | Audio | Enemy hit feedback  |

## Player/Enemy

| Asset                  | Type  | Notes                  |
| ---------------------- | ----- | ---------------------- |
| Player footstep dirt   | Audio | Loop/variants          |
| Player footstep metal  | Audio | Optional               |
| Player hurt sound      | Audio | Subtle                 |
| Player armor hit sound | Audio | Distinct from health   |
| Enemy death sounds     | Audio | Short variants         |
| Enemy alert bark       | Audio | “Contact!”             |
| Enemy flank bark       | Audio | “Moving left!”         |
| Enemy reload bark      | Audio | Optional               |
| Heavy warning bark     | Audio | “Heavy unit deployed!” |

## UI/Objective

| Asset                       | Type  | Notes                      |
| --------------------------- | ----- | -------------------------- |
| Radar ping                  | Audio | Every pulse or scan        |
| Objective update beep       | Audio | When objective changes     |
| Pickup collect sound        | Audio | Different per pickup class |
| Terminal interact beep loop | Audio | While holding E            |
| Terminal disabled sound     | Audio | Electronic shutdown        |
| Alarm siren                 | Audio | After terminal sabotage    |
| Extraction flare sound      | Audio | Burning flare              |
| Mission complete sting      | Audio | Short victory cue          |

## Ambience

| Asset                   | Type  | Notes                   |
| ----------------------- | ----- | ----------------------- |
| Desert wind ambience    | Audio | Low background          |
| Distant combat ambience | Audio | Very subtle             |
| Generator hum           | Audio | Near terminals          |
| Radio chatter loop      | Audio | Near comms area         |
| Fire/smoke crackle      | Audio | Burned vehicles/barrels |

---

# 9. Materials and Shaders

| Asset                           | Type        | Notes                       |
| ------------------------------- | ----------- | --------------------------- |
| Dirt ground material            | Material    | Main level surface          |
| Sandbag material                | Material    | Tan fabric                  |
| Concrete material               | Material    | Barriers/walls              |
| Damaged concrete material       | Material    | Variant                     |
| Military green vehicle material | Material    | Trucks                      |
| Rust/burnt metal material       | Material    | Wrecks                      |
| Wood crate material             | Material    | Crates                      |
| Metal crate material            | Material    | Supply boxes                |
| Emissive green material         | Material    | Ammo/pickup glow            |
| Emissive blue material          | Material    | Armor glow                  |
| Emissive purple material        | Material    | Weapon pickup               |
| Emissive yellow material        | Material    | Objective terminals         |
| Emissive red material           | Material    | Enemy markers/lights        |
| Glass/plastic HUD material      | UI/Material | For tactical overlays       |
| Decal material set              | Material    | Blood, scorch, bullet marks |

---

# 10. Prefabs / Blueprint Objects

These are not just visual assets. These are reusable gameplay objects.

| Prefab                      | Includes                                                  |
| --------------------------- | --------------------------------------------------------- |
| Player prefab               | Model, movement, aim, health, weapon slots, camera target |
| Rifleman prefab             | Model, AI, rifle, health, radar icon                      |
| Rusher prefab               | Model, AI, weapon, health, radar icon                     |
| Heavy prefab                | Model, AI, LMG, armor, warning icon                       |
| Assault rifle weapon prefab | Model, stats, muzzle flash, audio                         |
| Pistol weapon prefab        | Model, stats, muzzle flash, audio                         |
| Shotgun weapon prefab       | Model, stats, spread pattern, audio                       |
| Grenade prefab              | Model, throw arc, timer, explosion                        |
| Ammo pickup prefab          | Mesh, glow, pickup logic, UI label                        |
| Medkit pickup prefab        | Mesh, glow, pickup logic, UI label                        |
| Armor pickup prefab         | Mesh, glow, pickup logic, UI label                        |
| Shotgun pickup prefab       | Mesh, glow, swap logic, UI label                          |
| Terminal prefab             | Mesh, interaction logic, states, objective link           |
| Extraction zone prefab      | Marker, timer, win condition                              |
| Cover prefab                | Collider, cover metadata, durability optional             |
| Explosive barrel prefab     | Mesh, health, explosion trigger                           |
| Radar manager prefab        | Tracks enemies, pickups, objectives                       |
| Objective manager prefab    | Mission stage state machine                               |
| Spawner prefab              | Reinforcement trigger logic                               |

---

# 11. Animation List

## Player Animations

```text
Idle rifle
Idle pistol
Idle shotgun
Walk forward
Walk backward
Strafe left
Strafe right
Run
Dash
Rifle fire
Pistol fire
Shotgun fire
Rifle reload
Pistol reload
Shotgun reload shell
Grenade throw
Interact with terminal
Hurt flinch
Death
```

## Enemy Animations

```text
Idle
Patrol walk
Run to cover
Crouch/cover idle
Fire rifle
Fire SMG/shotgun
Fire LMG
Reload
Flinch
Grenade panic/dive optional
Death forward
Death backward
Heavy stagger
Heavy death
```

---

# 12. Level-Specific Set Dressing

These are the visual objects that make the outpost feel real.

| Asset                     |   Count Estimate |
| ------------------------- | ---------------: |
| Concrete barriers         |            30–45 |
| Sandbag stacks            |            25–40 |
| Wooden crates             |            40–70 |
| Metal crates              |            15–25 |
| Military trucks           |              3–5 |
| Damaged vehicles          |              2–3 |
| Red explosive barrels     |              4–6 |
| Non-explosive barrels     |            10–20 |
| Chain-link fence segments |            20–35 |
| Floodlight poles          |             6–10 |
| Cargo containers          |              4–8 |
| Guard tower               |                1 |
| Military tents            |              2–4 |
| Generator props           |              3–5 |
| Satellite dishes          |              2–3 |
| Comms tower               |                1 |
| Warning signs             |             5–10 |
| Cable props               |            10–20 |
| Dirt/debris decals        |           80–150 |
| Bullet/scorch decals      | runtime + placed |

---

# 13. Minimum Asset Set to Actually Build the Demo

This is the **lean version** I would build first.

```text
1 player soldier
3 enemy variants or 1 enemy model with 3 material/scale variants
3 weapons: rifle, pistol, shotgun
1 grenade
5 pickups: ammo, medkit, armor, grenade, shotgun
3 terminal props
1 comms tower
1 extraction marker
6 cover props: concrete barrier, sandbags, crate, truck, cargo container, barrel
1 terrain material
1 fence set
1 guard tower
1 military tent
1 radar UI
1 objective UI
1 health/armor/ammo HUD
Core muzzle flash, tracer, impact, explosion, smoke, pickup glow VFX
Core weapon, pickup, enemy, terminal, and radar sounds
```

With that, you can build the full first level without needing a giant asset library.

---

# 14. Priority Order

Build/buy assets in this order:

```text
1. Player + basic rifle enemy
2. Terrain + cover props
3. Rifle/pistol/shotgun weapons
4. Pickups
5. HUD/radar
6. Terminal/objective props
7. Muzzle flash/tracer/impact VFX
8. Enemy variants: rusher and heavy
9. Vehicles/tower/tents/fences
10. Audio polish
11. Set dressing/decal variety
12. Mission complete screen
```

The absolute critical path is:

> **Player, enemies, weapons, cover, pickups, radar, objectives, terminals, extraction.**

Everything else improves presentation, but those are the assets required for the level to function.
