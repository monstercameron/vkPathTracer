# First Level Design: **Signal Break — Level 01: Relay Yard**

The last image is the right target: **3D isometric / top-down tactical arcade shooter**, not first-person military realism and not flat 2D pixel art. The camera should sit high above the player at a tilted angle, with clear readable silhouettes, visible cover, glowing pickups, tracer fire, and a modern HUD/radar layer.

The first level should be a **vertical slice**: one small mission that proves movement, mouse aim, cover, pickups, radar guidance, weapon tradeoffs, and objective flow.

---

# Game Identity

## Working Title

**Signal Break**

## Genre

Top-down / isometric 3D military shooter.

## Core Fantasy

You are a lone special-operations soldier assaulting a lightly fortified enemy communications outpost. The battlefield is open, but the player is guided by a **helmet-mounted tactical radar** that marks objectives, enemy activity, pickups, and extraction.

## Target Feel

Fast, readable, tactical, and violent, but not simulation-heavy.

The player should constantly make simple tactical choices:

> “Do I push to the next cover, flank around the truck, grab the ammo, switch to shotgun, or follow the radar straight to the objective?”

---

# High-Level First Level Summary

## Mission Name

**Relay Yard**

## Mission Objective

Destroy an enemy communications relay by disabling **3 terminals**, then reach extraction.

## Level Duration

**8–12 minutes** for a first-time player.

## Player Start

Southwest edge of the outpost, behind a damaged armored vehicle.

## Final Objective

Reach the northeast comms tower, disable the relay, then escape to an extraction marker on the east side of the map.

## Level Completion

The level ends when the player enters the extraction zone after destroying the comms array.

---

# First Level Design Goals

This level should teach the whole game without feeling like a tutorial.

It should teach:

1. **Move and aim independently**
2. **Use mouse aim to engage enemies**
3. **Use cover to survive**
4. **Read the radar**
5. **Collect ammo, medkits, armor, grenades, and weapons**
6. **Understand weapon range and mobility tradeoffs**
7. **Complete multi-step objectives**
8. **Survive an extraction push**

The first level should feel like a complete short mission, not a tech demo.

---

# Core Game Rules

## Camera Rule

The game uses a fixed-angle top-down 3D camera.

Recommended camera:

```text
Camera angle: 55–65 degrees downward
Camera distance: medium-high
Camera movement: follows player smoothly
Camera offset: slightly toward mouse cursor
```

The camera should show enough space ahead of the player to make aiming and radar decisions meaningful.

## Movement Rule

The player moves with **WASD** and aims with the mouse.

The player can move in one direction while shooting in another.

Example:

```text
WASD = movement direction
Mouse = aim direction
Left Click = fire
Right Click = precision aim / slower movement
R = reload
E = interact / pickup / disable terminal
Space = dash
G = grenade
```

## Aiming Rule

The character rotates toward the mouse cursor. The weapon fires toward the cursor.

A small crosshair sits on the ground plane. The crosshair should change depending on what it is over:

| Crosshair State | Meaning                  |
| --------------- | ------------------------ |
| White           | Empty ground             |
| Red             | Enemy target             |
| Yellow          | Objective                |
| Green           | Pickup                   |
| Gray / blocked  | Shot obstructed by cover |

## Shooting Rule

Bullets travel in the aimed direction and collide with enemies, cover, vehicles, props, or map boundaries.

For the first level, use fast visible tracers rather than invisible hitscan. The player should see where shots are going.

## Reload Rule

Weapons have magazines and reserve ammo.

Reloading takes time and leaves the player vulnerable. Reload can be canceled by dashing or switching weapons, but the reload does not complete if canceled.

## Pickup Rule

Pickups are collected by walking over them or pressing **E** when nearby.

For the first level, make pickups obvious and readable. They should glow with colored outlines.

| Pickup             | Color        |
| ------------------ | ------------ |
| Ammo               | Green        |
| Medkit             | Cyan / white |
| Armor              | Blue         |
| Shotgun            | Purple       |
| Grenade            | Lime green   |
| Objective terminal | Yellow       |

## Cover Rule

Cover physically blocks bullets.

The game should not use complex sticky cover in level one. The rule should be simple:

> If an object is between the shooter and the target, the bullet hits the object.

The player learns cover naturally by hiding behind sandbags, trucks, concrete blocks, crates, and guard structures.

## Health Rule

The player has **health** and **armor**.

Armor absorbs damage first. Health damage matters more and is harder to recover.

Recommended values:

```text
Player Health: 100
Player Armor: 75
Downed / death state: 0 health
Armor pickup: +40 armor, max 100
Medkit: +50 health, max 100
```

## Death Rule

If health reaches zero, the level restarts from the latest checkpoint.

Checkpoints should occur after:

1. First patrol cleared
2. First terminal disabled
3. Second terminal disabled
4. Third terminal disabled before extraction

---

# HUD Design

The HUD should be modern, tactical, and clean.

## Top Left: Objective Panel

Example:

```text
OBJECTIVE
DESTROY COMMS ARRAY

0/3 TERMINALS DISABLED

> Reach Terminal Alpha
```

The objective panel should update as the player progresses.

## Top Right: Tactical Radar

The radar is one of the main features. It should guide the player through the open battlefield.

Radar elements:

| Icon                  | Meaning             |
| --------------------- | ------------------- |
| Blue / white triangle | Player              |
| Red diamonds          | Enemies             |
| Yellow square         | Objective           |
| Purple square         | Extraction          |
| Green square          | Pickup              |
| White pulse ring      | Gunfire / noise     |
| Red wedge             | Enemy alert cone    |
| Dashed yellow line    | Objective direction |

Radar should not behave like a full map. It should feel like a tactical scanner.

Recommended first-level radar rules:

```text
Radar radius: 35 meters
Radar pulse interval: every 2 seconds
Objectives: always visible
Extraction: appears after comms array is destroyed
Enemies: visible if nearby, firing, alerted, or recently scanned
Pickups: visible only within radar radius
```

This keeps the open map readable without removing tension.

## Bottom Left: Player Status

Display:

```text
Health bar
Armor bar
Optional stamina / dash cooldown
```

## Bottom Right: Weapon Panel

Display:

```text
Current weapon
Ammo in magazine / reserve ammo
Fire mode
Grenade count
Reload indicator
```

Example:

```text
ASSAULT RIFLE
23 / 90

GRENADES: 02
```

## Center Screen: Combat Information

Use only lightweight indicators:

* Hit marker
* Low ammo warning
* Reload progress ring
* Pickup label
* Objective interaction progress
* Directional damage indicator

Avoid cluttering the center of the screen.

---

# Player Loadout for Level 01

The player starts with:

```text
Primary: Assault Rifle
Secondary: Pistol
Grenades: 2
Health: 100
Armor: 50
```

The level contains a shotgun pickup to introduce weapon swapping.

---

# Weapon Design

For the first level, use only three weapons. This is enough to prove the combat system.

## 1. Pistol

Reliable fallback weapon.

```text
Damage: 18
Magazine: 12
Reserve ammo: infinite or 48
Fire rate: medium
Reload speed: fast
Range: medium
Movement penalty: none
Accuracy: high
Role: backup weapon
```

Use case:

* When primary ammo is empty
* When the player wants mobility
* Finishing low-health enemies

## 2. Assault Rifle

Main all-purpose weapon.

```text
Damage: 23
Magazine: 30
Reserve ammo: 90
Fire rate: high
Reload speed: medium
Range: medium-long
Movement penalty: small
Accuracy: medium
Recoil/bloom: increases during sustained fire
Role: default combat weapon
```

Use case:

* Mid-range firefights
* General outpost combat
* Suppressing enemies behind cover

## 3. Shotgun

High-risk close-range weapon.

```text
Damage: 8 pellets x 10 damage
Magazine: 6
Reserve ammo: 24
Fire rate: low
Reload speed: shell-by-shell
Range: short
Movement penalty: small
Spread: wide
Role: close quarters and rushers
```

Use case:

* Clearing enemies around crates and tents
* Defending during extraction
* Killing rushers quickly

---

# Grenade Rules

Grenades should be simple, powerful, and readable.

```text
Throw button: G
Explosion delay: 1.2 seconds
Blast radius: 4 meters
Damage: 120 at center, falloff toward edge
Cover damage: high
Enemy warning: enemies attempt to dive or move away
Starting grenades: 2
Maximum grenades: 3
```

Grenades should destroy crates, damage sandbags, and flush enemies from cover.

---

# Enemy Types in Level 01

Use three enemy types. More than that will distract from the core mechanics.

---

## 1. Rifleman

Basic enemy shooter.

```text
Health: 45
Weapon: rifle
Damage: 12 per hit
Behavior: takes cover, fires short bursts
Detection range: medium
Radar icon: red diamond
```

Purpose:

* Teaches basic shooting and cover
* Forms most of the level’s enemy population

---

## 2. Rusher

Fast aggressive enemy.

```text
Health: 35
Weapon: SMG or shotgun
Damage: high at close range
Behavior: advances quickly, flanks, pressures player
Detection range: medium
Radar icon: red diamond with small motion trail
```

Purpose:

* Forces player to move
* Prevents camping behind one cover object
* Makes the shotgun pickup valuable

---

## 3. Heavy

Armored suppressive enemy.

```text
Health: 120
Armor: 50
Weapon: LMG
Damage: moderate
Fire rate: high
Movement: slow
Behavior: suppresses player position, advances slowly
Weakness: grenades and flanking
Radar icon: larger red diamond
```

Purpose:

* Acts as the first mini-boss
* Teaches grenades, flanking, and ammo management

The heavy should appear only once in the first level, near the final terminal.

---

# Enemy AI Rules

The AI does not need to be hyper-realistic. It needs to be readable.

## AI States

```text
Patrol
Suspicious
Alert
Take Cover
Attack
Flank
Retreat
Dead
```

## AI Detection

Enemies detect the player through:

| Detection Type | Rule                                               |
| -------------- | -------------------------------------------------- |
| Line of sight  | Enemy sees player within view cone                 |
| Sound          | Gunfire alerts enemies nearby                      |
| Proximity      | Player gets too close                              |
| Radar event    | Optional scripted enemy awareness after objectives |

Recommended values:

```text
Enemy vision range: 22 meters
Enemy vision cone: 120 degrees
Gunshot hearing radius: 30 meters
Suppressed weapon hearing radius: not needed in level 1
Suspicion duration: 3 seconds
Alert memory: 8 seconds
```

## AI Cover Rule

When alerted, riflemen should look for nearby cover.

Priority:

1. Cover between enemy and player
2. Cover close to current position
3. Cover that allows shooting angle
4. Cover near allies

Do not let every enemy perfectly choose cover. Some should make mistakes and get caught in the open.

## AI Flanking Rule

Only rushers and some riflemen flank.

Basic rule:

```text
If enemy has been behind cover for 4+ seconds
And player is also behind cover
And flank route exists
Then enemy attempts side movement
```

The player should notice this on radar before being surprised.

---

# First Level Map Design

## Map Name

**Relay Yard**

## Map Theme

A desert military communications outpost with:

* Sandbag positions
* Concrete barriers
* Supply crates
* Cargo containers
* Guard tower
* Military trucks
* Satellite dishes
* Comms tower
* Fuel barrels
* Fences
* Dirt roads
* Floodlights
* Temporary command tents

## Map Size

Recommended size:

```text
Width: 100 meters
Height: 75 meters
```

This is large enough to feel open but small enough for a demo.

---

# Level Layout Overview

The map should be shaped like a loose diagonal push from southwest to northeast.

```text
                         NORTH
                           ↑

        [Comms Tower / Final Terminal]
                   ▲
                   |
       [Relay Yard / Heavy Fight]
                   |
       [Motor Pool / Shotgun Pickup]
                   |
       [Central Kill Lane]
             /           \
 [Supply Yard]       [Side Flank Path]
             \           /
          [Insertion Zone]

                           → EAST
```

The radar should constantly pull the player northeast toward the comms tower.

---

# Detailed Level Zones

## Zone A: Insertion Zone

### Purpose

Teach movement, aiming, and the HUD without pressure.

### Location

Southwest corner.

### Environment

* Wrecked armored vehicle
* Low concrete wall
* A few crates
* Dirt road leading north
* First objective marker visible on radar

### Player Starts With

```text
Assault rifle: 30 / 90
Pistol: 12 / 48
Grenades: 2
Health: 100
Armor: 50
```

### Tutorial Prompts

Keep these small and diegetic:

```text
WASD: Move
Mouse: Aim
Left Click: Fire
R: Reload
Use radar to locate Terminal Alpha
```

### Enemies

None at the very start.

After the player moves forward, spawn or reveal:

```text
2 riflemen
```

They should be positioned poorly, partially in the open, so the player gets an easy first kill.

### Pickup

One small ammo box after the first engagement.

### Exit Condition

Player clears the first two enemies and moves toward the supply yard.

---

## Zone B: Supply Yard

### Purpose

Teach pickups and basic cover.

### Environment

* Stacked crates
* Sandbags
* Ammo box
* Medkit
* Parked truck
* Watchtower in the background

### Enemies

```text
3 riflemen
```

Enemy layout:

* One behind sandbags
* One patrolling near crates
* One on the far side of the truck

### Pickups

```text
Ammo box
Medkit
```

### Objective Behavior

Radar shows:

```text
Yellow marker: Terminal Alpha
Green marker: nearby ammo
Green marker: nearby medkit
```

### Combat Lesson

The player should learn:

* Bullets hit cover
* Enemies use cover
* Moving around cover creates better firing angles
* Pickups are worth moving toward

### Exit Condition

Player reaches Terminal Alpha.

---

## Zone C: Terminal Alpha

### Purpose

Teach objective interaction.

### Environment

A small comms control box beside a generator.

### Interaction Rule

The player holds **E** to disable the terminal.

```text
Hold E: 3 seconds
Taking damage interrupts interaction
Progress resumes from 50% if interrupted
```

### Objective Text

Before interaction:

```text
DISABLE TERMINAL ALPHA
Hold E to sabotage uplink
```

After interaction:

```text
TERMINAL ALPHA DISABLED
1/3 TERMINALS DESTROYED
Reach Terminal Bravo
```

### Enemy Trigger

When Terminal Alpha is disabled:

```text
2 riflemen enter from north road
1 rusher enters from side path
```

This is the first time the level pushes back after an objective.

### Radar Behavior

A white pulse appears from the north to indicate enemy movement.

---

## Zone D: Central Kill Lane

### Purpose

Teach radar-guided navigation through open ground.

### Environment

A wide exposed dirt lane with scattered cover.

Objects:

* Concrete blocks
* Burned-out car
* Sandbags
* Crates
* Red explosive barrels
* Military truck

### Enemies

```text
4 riflemen
1 rusher
```

Enemy layout:

* Two riflemen behind central barriers
* One rifleman near truck
* One rifleman on right-side sandbags
* One rusher waits behind crates and charges after gunfire starts

### Pickups

```text
Armor plate in the middle-left route
Grenade pickup near the risky right-side route
```

### Design Choice

The level should offer two approaches:

## Direct Route

Shorter but more dangerous.

* More enemies
* Less cover
* Better line of sight
* Faster objective access

## Flank Route

Longer but safer.

* More cover
* More pickups
* Better angle on enemies

The radar should subtly guide the player, not force them.

Example:

```text
Yellow objective marker points forward
Green armor pickup appears on left radar edge
Red enemy pings cluster in the direct route
```

This teaches the player to read the radar tactically.

---

## Zone E: Motor Pool

### Purpose

Introduce weapon swapping and close-range combat.

### Environment

* Parked military vehicles
* Cargo crates
* Fuel barrels
* Maintenance tent
* Narrower cover gaps

### Weapon Pickup

The player finds a shotgun here.

Pickup label:

```text
SHOTGUN
Close-range power weapon
Press E to swap
```

### Weapon Swap Rule

The player can carry:

```text
1 primary weapon
1 sidearm
Grenades
```

When picking up the shotgun, the player swaps their assault rifle unless the game supports two primaries.

For the demo, I recommend allowing:

```text
Primary Slot 1: Assault Rifle
Primary Slot 2: Shotgun
Sidearm: Pistol
```

That is more fun for a demo and lets players experiment.

### Enemies

```text
2 riflemen
2 rushers
```

The rushers should make the shotgun feel immediately useful.

### Pickup

```text
Shotgun
Armor plate
Ammo box
```

### Combat Lesson

The player learns:

* Shotgun dominates close range
* Rifle is safer at mid-range
* Weapon choice matters
* Cover spacing affects weapon usefulness

---

## Zone F: Terminal Bravo

### Purpose

Combine objective interaction with enemy pressure.

### Environment

A guarded terminal near a comms truck and stacked supply crates.

### Enemies

```text
3 riflemen
1 rusher
```

### Objective Interaction

Same as Terminal Alpha:

```text
Hold E for 3 seconds
Damage interrupts
```

### Enemy Reinforcement Trigger

When the player starts interacting with the terminal:

```text
1 rusher enters from behind
2 riflemen reposition from the north
```

Do not spawn too many enemies. The goal is tension, not frustration.

### Radar Behavior

Radar should flash:

```text
HOSTILE MOVEMENT DETECTED
```

Red pings appear behind the player.

This teaches the player to check radar during objectives.

### Completion Text

```text
TERMINAL BRAVO DISABLED
2/3 TERMINALS DESTROYED
Reach Terminal Charlie at the tower base
```

---

## Zone G: Relay Yard

### Purpose

Main combat arena.

### Environment

This is the largest fight space in the level.

Objects:

* Sandbag nests
* Concrete barriers
* Tower supports
* Satellite dish base
* Generator units
* Fence sections
* Floodlight poles
* Crates
* Explosive barrels
* One large truck as high cover

### Enemies

Initial group:

```text
4 riflemen
1 rusher
```

After player enters deeper:

```text
1 heavy
2 riflemen
```

### Heavy Introduction

The heavy should enter with a clear warning.

Visual:

* Larger silhouette
* Heavy armor
* LMG muzzle flash
* Red radar icon larger than normal enemies

Audio/UI:

```text
WARNING: HEAVY UNIT DETECTED
```

### Heavy Combat Design

The heavy should not simply rush the player. It should:

* Fire long suppressive bursts
* Move slowly between cover
* Force player to reposition
* Be vulnerable to grenades
* Be easier to kill from the side or rear

### Pickups

```text
Ammo box
Grenade pickup
Medkit behind risky cover
```

The medkit should be visible but dangerous to reach.

### Combat Lesson

The player learns the full combat loop:

* Use cover
* Watch radar
* Manage ammo
* Switch weapons
* Use grenades
* Flank heavy enemies
* Push to objective

---

## Zone H: Terminal Charlie / Comms Tower Base

### Purpose

Final objective interaction.

### Environment

At the base of the communications tower.

The terminal should be visually important:

* Yellow glow
* Cables running to tower
* Radar pulse effect
* Sparks after sabotage
* Loud alarm after completion

### Objective Interaction

This one takes longer:

```text
Hold E for 5 seconds
Damage interrupts
Progress resumes from 50%
```

### During Interaction

Spawn light pressure, not a huge wave:

```text
2 riflemen from west
1 rusher from south
```

The player should need to clear enemies before safely finishing the interaction.

### Completion Event

When Terminal Charlie is disabled:

```text
COMMS ARRAY DESTROYED
3/3 TERMINALS DISABLED
EXTRACTION AVAILABLE
```

The comms tower emits sparks and smoke. The radar changes from yellow objective guidance to purple extraction guidance.

---

## Zone I: Extraction Route

### Purpose

End the level with urgency.

### Location

East side of the map, beyond a fenced road.

### Environment

* Open dirt road
* Concrete barriers
* Smoke
* Emergency lights
* Extraction flare
* Optional helicopter shadow or armored evac vehicle

### Objective

```text
REACH EXTRACTION
```

Once the player enters the extraction zone:

```text
Hold position for 20 seconds
```

### Enemy Pressure

During extraction:

```text
Wave 1: 3 riflemen
Wave 2: 2 rushers
Final pressure: 1 heavy OR 3 riflemen, depending on difficulty
```

For level one, avoid another full heavy fight unless the player is doing well.

### Extraction Rule

The player does not need to kill every enemy. They only need to survive until extraction completes.

### End Screen

```text
MISSION COMPLETE

Comms array destroyed
Enemies neutralized: X
Damage taken: X
Accuracy: X%
Time: X:XX
Pickups collected: X/X
```

---

# Objective Flow

The level objectives should update clearly.

## Objective 1

```text
Reach Terminal Alpha
```

## Objective 2

```text
Disable Terminal Alpha
0/3 terminals disabled
```

## Objective 3

```text
Reach Terminal Bravo
1/3 terminals disabled
```

## Objective 4

```text
Disable Terminal Bravo
1/3 terminals disabled
```

## Objective 5

```text
Reach Terminal Charlie
2/3 terminals disabled
```

## Objective 6

```text
Destroy Comms Array
3/3 terminals disabled
```

## Objective 7

```text
Reach Extraction
```

## Objective 8

```text
Hold Extraction Zone
```

## Mission Complete

```text
Signal Break successful
```

---

# Full First Level Encounter List

## Encounter 1: First Contact

```text
Location: Insertion road
Enemies: 2 riflemen
Purpose: teach shooting
Pickups: ammo after fight
Difficulty: very easy
```

## Encounter 2: Supply Yard

```text
Location: crates and sandbags
Enemies: 3 riflemen
Purpose: teach cover and pickups
Pickups: medkit, ammo
Difficulty: easy
```

## Encounter 3: Terminal Alpha Counterattack

```text
Location: Terminal Alpha
Enemies: 2 riflemen, 1 rusher
Purpose: teach objective consequences
Pickups: none
Difficulty: easy-medium
```

## Encounter 4: Central Kill Lane

```text
Location: open lane
Enemies: 4 riflemen, 1 rusher
Purpose: teach radar route choice
Pickups: armor, grenade
Difficulty: medium
```

## Encounter 5: Motor Pool

```text
Location: vehicles and crates
Enemies: 2 riflemen, 2 rushers
Purpose: teach shotgun and close combat
Pickups: shotgun, ammo, armor
Difficulty: medium
```

## Encounter 6: Terminal Bravo Defense

```text
Location: comms truck
Enemies: 3 riflemen, 1 rusher, plus small reinforcement
Purpose: combine objective and pressure
Pickups: medkit nearby
Difficulty: medium
```

## Encounter 7: Relay Yard Heavy Fight

```text
Location: tower approach
Enemies: 4 riflemen, 1 rusher, 1 heavy
Purpose: teach grenades and flanking
Pickups: ammo, grenade, risky medkit
Difficulty: medium-hard
```

## Encounter 8: Terminal Charlie

```text
Location: tower base
Enemies: 2 riflemen, 1 rusher
Purpose: final sabotage interaction
Pickups: none
Difficulty: medium
```

## Encounter 9: Extraction Holdout

```text
Location: east road extraction zone
Enemies: 3 riflemen, 2 rushers, optional final riflemen
Purpose: climactic survival
Pickups: final ammo box before zone
Difficulty: medium-hard
```

---

# Enemy Count for Level 01

Recommended total:

```text
Riflemen: 24–28
Rushers: 7–9
Heavy: 1
Total enemies: 32–38
```

That is enough action for a first level without overwhelming the player.

---

# Pickup Placement

## Total Pickups

```text
Ammo boxes: 5
Medkits: 3
Armor plates: 3
Grenade pickups: 2
Shotgun pickup: 1
```

## Placement Philosophy

Pickups should not just be rewards. They should create tactical decisions.

Examples:

* Put armor slightly off the safest route.
* Put medkits in exposed positions.
* Put ammo before harder fights.
* Put shotgun before close-range enemies.
* Put grenades before the heavy.

---

# Level One Map Layout Detail

Here is a simple top-down planning layout.

```text
┌──────────────────────────────────────────────┐
│                    NORTH                     │
│                                              │
│        [G] RELAY YARD / HEAVY FIGHT          │
│                [H] TERMINAL CHARLIE          │
│                       │                      │
│                       │                      │
│          [F] TERMINAL BRAVO                  │
│        crates / comms truck / rushers        │
│                       │                      │
│     [E] MOTOR POOL────┘                      │
│     shotgun pickup                           │
│                       │                      │
│          [D] CENTRAL KILL LANE               │
│       open ground / radar route choice       │
│             /                    \           │
│            /                      \          │
│ [B] SUPPLY YARD             [SIDE FLANK]     │
│ ammo / medkit / cover                         │
│            \                      /          │
│             \                    /           │
│          [A] INSERTION ZONE                  │
│                                              │
│                              [I] EXTRACTION  │
└──────────────────────────────────────────────┘
```

The player generally moves from southwest to northeast, then extracts east.

---

# Radar Design for This Level

The radar should be doing constant design work.

## During Insertion

Radar shows only:

```text
Player arrow
Terminal Alpha marker
Nearby ammo after first fight
```

## During Supply Yard

Radar begins showing nearby enemies as red pings.

## After Terminal Alpha

Radar shows enemy reinforcements approaching from the north.

## Central Kill Lane

Radar shows two possible decisions:

```text
Red cluster: direct path danger
Green marker: armor on safer flank
Yellow marker: objective direction
```

## Motor Pool

Radar shows close red pings around vehicles, warning the player that enemies may flank.

## Relay Yard

Radar highlights the heavy unit with a larger red ping.

## After Comms Array Destroyed

Radar objective mode switches:

```text
Yellow objective marker disappears
Purple extraction marker appears
All enemy pings pulse faster
```

This creates a clean “now escape” moment.

---

# Cover and Prop Rules

## Concrete Barriers

```text
Blocks bullets fully
Cannot be destroyed
Low cover height
Good for player and enemies
```

## Sandbags

```text
Blocks bullets
Can be damaged
Medium durability
Enemies prefer this cover
```

## Wooden Crates

```text
Blocks some bullets
Low durability
Can be destroyed by gunfire or grenades
Often hides pickups
```

## Military Trucks

```text
High cover
Blocks line of sight
Can be used to flank around
Optional: can explode only if damaged heavily
```

For level one, I would avoid making trucks explode unless heavily telegraphed. Random vehicle explosions can feel unfair.

## Red Barrels

```text
Explosive
Clearly marked
Useful for killing groups
Explosion radius: 3.5 meters
```

Use only 4–6 explosive barrels in the whole level.

---

# Rules for Objectives

## Terminal Interaction

Each terminal requires the player to hold interact.

```text
Terminal Alpha: 3 seconds
Terminal Bravo: 3 seconds
Terminal Charlie: 5 seconds
```

Taking damage interrupts the interaction.

The terminal remains partially sabotaged so the player does not lose all progress.

Recommended:

```text
If interrupted above 50%, resume at 50%
If interrupted below 50%, keep current progress
```

## Objective Completion Feedback

Each destroyed terminal should provide:

* Loud electronic shutdown sound
* Screen shake
* Radar pulse
* Sparks from terminal
* Objective count update
* Enemy reinforcement cue

---

# Difficulty Tuning

## Normal Mode Target

The average player should complete the level after one or two deaths.

## Player Survivability

The player should survive:

```text
6–8 rifle hits with armor
4–5 rifle hits without armor
2–3 close rusher bursts
Heavy fire only briefly without cover
```

## Enemy Lethality

Enemies should be dangerous in groups, not individually.

A single rifleman should not be a major threat. Three riflemen behind cover should force the player to think.

## Ammo Pressure

Ammo should matter, but the player should not be starved in level one.

Recommended:

```text
Assault rifle ammo available often
Shotgun ammo limited
Pistol always available as fallback
```

---

# First Level Scripted Beats

These moments will make the level feel polished.

## Beat 1: Insertion

The player enters from behind a wrecked vehicle. The comms tower is visible in the distance.

HUD:

```text
OBJECTIVE: Destroy Comms Array
Reach Terminal Alpha
```

## Beat 2: First Contact

Two enemies appear near crates. The player shoots them easily.

Purpose: confidence.

## Beat 3: First Pickup

The player sees a glowing ammo box.

Purpose: teach pickups.

## Beat 4: First Terminal

The player disables Terminal Alpha.

Alarm sound. Radar pulses red.

Purpose: objectives trigger consequences.

## Beat 5: First Rusher

A fast enemy flanks from the side.

Purpose: teach movement under pressure.

## Beat 6: Open Yard Choice

The player reaches the central kill lane. Radar shows heavy enemy concentration ahead and safer pickups to the left.

Purpose: teach radar decision-making.

## Beat 7: Shotgun Discovery

The player finds a purple shotgun pickup in the motor pool.

Purpose: weapon excitement.

## Beat 8: Heavy Unit Warning

Near the relay yard:

```text
WARNING: HEAVY UNIT DETECTED
```

Purpose: mini-boss moment.

## Beat 9: Comms Array Destroyed

The tower sparks, alarms blare, radar shifts to extraction mode.

Purpose: mission climax.

## Beat 10: Extraction Holdout

The player reaches the flare zone and survives the final push.

Purpose: satisfying finish.

---

# Audio Direction

The audio should support clarity and pressure.

## Essential Sounds

* Assault rifle fire
* Shotgun blast
* Pistol fire
* Enemy gunfire
* Bullet impacts on metal, dirt, sandbags, concrete
* Armor hit sound
* Health damage sound
* Reload clicks
* Grenade pin and explosion
* Radar ping
* Objective terminal beeps
* Alarm after terminal sabotage
* Extraction flare / helicopter / vehicle arrival

## Enemy Audio

Use short radio-style barks:

```text
“Contact!”
“Flank left!”
“Reloading!”
“Push him!”
“Heavy moving!”
“Protect the relay!”
```

Keep them short and readable.

---

# Visual Direction

## Art Style

Use realistic 3D assets with stylized readability.

Not ultra-realistic. Not gritty FPS realism. More like:

```text
Readable tactical miniatures
Strong silhouettes
Clear cover shapes
Saturated HUD colors
Bright pickup glows
Visible bullet tracers
Cinematic lighting from high angle
```

## Lighting

The image’s warm desert lighting works well.

Use:

* Late afternoon sun
* Long shadows
* Dust in the air
* Orange muzzle flashes
* Neon HUD/pickup colors
* Blue armor glow
* Green ammo glow
* Purple weapon pickup glow
* Yellow objective glow

## Enemy Readability

Enemies should have:

* Red shoulder lights or red outline
* Red diamond overhead marker when detected
* Distinct heavy silhouette
* Clear muzzle flash direction

---

# Level Completion Metrics

At the end, show a score screen.

Recommended stats:

```text
Mission Time
Enemies Neutralized
Accuracy
Damage Taken
Pickups Collected
Deaths
Terminals Disabled
Difficulty
Rank
```

Example ranks:

```text
S: under 7 minutes, no deaths
A: under 10 minutes
B: mission complete
C: completed with multiple deaths
```

---

# First Level Implementation Checklist

## Required for Playable Level

```text
Player movement
Mouse aiming
Shooting
Reloading
Health and armor
Three weapons
Grenades
Enemy rifleman AI
Enemy rusher AI
Heavy enemy AI
Bullet collision
Cover objects
Pickups
Radar
Objective system
Terminal interaction
Extraction zone
Win/loss states
HUD
Basic sound effects
```

## Nice Polish for Demo

```text
Muzzle flashes
Tracer rounds
Hit markers
Blood/dust impacts
Cover chip effects
Enemy alert icons
Screen shake
Objective voice lines
Radar pulse animation
Pickup glow animation
Extraction flare
Mission complete screen
```

---

# Final First Level Spec

## Level Name

**Relay Yard**

## Mission

Destroy the enemy communications array.

## Objectives

```text
1. Enter the outpost
2. Disable Terminal Alpha
3. Cross the central yard
4. Acquire optional shotgun
5. Disable Terminal Bravo
6. Assault the relay yard
7. Defeat or bypass the heavy unit
8. Disable Terminal Charlie
9. Reach extraction
10. Hold extraction until evac arrives
```

## Weapons

```text
Pistol
Assault Rifle
Shotgun
Grenades
```

## Enemies

```text
Rifleman
Rusher
Heavy
```

## Pickups

```text
Ammo
Medkit
Armor
Grenade
Shotgun
```

## Core Mechanics Shown

```text
Mouse aim
Independent movement
Cover
Pickups
Weapon tradeoffs
Radar guidance
Objective interaction
Enemy reinforcements
Extraction holdout
```

## First Level Design Statement

**Relay Yard** is a compact open military outpost designed to teach the full game loop in one mission. The player advances through sandbag positions, vehicles, crates, and comms equipment while the radar guides them from objective to objective. Combat rewards smart cover use, controlled fire, weapon switching, and tactical movement. The level escalates from simple riflemen to rushers, then to a heavy unit guarding the final terminal, before ending with an extraction holdout.

That gives you a complete first-level foundation: readable, buildable, and demo-ready.
