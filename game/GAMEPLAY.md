# Signal Break — Gameplay Systems Specification

This version moves the design from “general idea” into a **procedural gameplay spec**. The mold is **Killzone: Liberation-style isometric tactical cover combat**, adapted for mouse/keyboard and gamepad.

The design goal is:

> **A top-down/isometric tactical shooter where the player advances from cover to cover, uses radar to read the battlefield, pops up to fire controlled bursts, manages ammo and armor, and completes objectives while enemies flank, rush, and suppress.**

This should not play like a free-roaming twin-stick bullet hose. It should play like a **cover-first tactical action game**.

---

# 1. Core Camera Style

## Camera Type

Use a **fixed isometric 3D camera**.

```text
Camera Mode: Fixed isometric tactical camera
Angle: 55–60 degrees downward
Rotation: Fixed per level
Zoom: Mostly fixed, slight dynamic adjustment allowed
Player position: Lower-center of screen
Aim direction: Camera subtly biases toward cursor
```

The camera should make the battlefield readable. The player should always understand:

```text
Where am I?
Where is cover?
Where are enemies?
Where is the objective?
Where can I move next?
```

---

# 2. Camera Rules

## Base Camera Position

The camera follows a target point, not directly the player.

```text
CameraTarget = PlayerPosition + AimLookAheadOffset + CombatOffset
```

## Aim Look-Ahead

When the player aims with the mouse, the camera shifts slightly toward the cursor direction.

```text
AimDirection = normalize(CursorWorldPosition - PlayerPosition)

AimLookAheadOffset = AimDirection * LookAheadDistance
```

Recommended value:

```text
LookAheadDistance = 3.5 to 5.0 meters
```

This lets the player see more in the direction they are aiming without losing sight of themselves.

## Camera Smoothing

Camera movement should be smooth but responsive.

```text
CameraPosition = SmoothDamp(CurrentCameraPosition, DesiredCameraPosition, 0.12 seconds)
```

Recommended values:

```text
Normal smoothing: 0.12s
Combat smoothing: 0.08s
Objective focus smoothing: 0.20s
```

The camera should never feel floaty during combat.

---

# 3. Camera Zoom

Use a mostly fixed zoom, with only small procedural changes.

## Default Zoom

```text
Default camera height: 18–24 meters above player
```

## Aim Zoom

When holding aim stance:

```text
Camera zooms out slightly by 5–8%
Camera shifts toward cursor
```

Purpose:

```text
Better target visibility
Better lane reading
More tactical feel
```

## Interior / Tight Area Zoom

If the player enters a tighter space:

```text
Camera zooms in slightly
Camera collision avoids blocking view
```

But Level 1 is mostly open, so this is optional.

---

# 4. Camera Obstruction Handling

Objects should not hide the player.

If a tall prop blocks the camera view between camera and player:

```text
Fade obstructing object to 35–50% opacity
Keep player fully visible
Keep enemy markers visible
```

Objects that may fade:

```text
Tents
Guard tower posts
Comms tower base
Cargo containers
Trucks
Large antenna structures
```

Never rotate the camera freely in Level 1. Free rotation complicates combat readability and asset layout.

---

# 5. Mouse and Keyboard Controls

## Final Control Layout

| Input                | Action                        |
| -------------------- | ----------------------------- |
| **WASD**             | Move                          |
| **Mouse**            | Aim cursor                    |
| **Left Click**       | Fire                          |
| **Right Click Hold** | Aim stance / strafe-lock      |
| **Ctrl Hold**        | Crouch / take cover           |
| **Space**            | Dodge / combat roll           |
| **R**                | Reload                        |
| **E Tap**            | Pickup / quick interact       |
| **E Hold**           | Objective interaction         |
| **G Tap**            | Quick grenade throw           |
| **G Hold**           | Grenade aim preview           |
| **Q**                | Radar pulse                   |
| **Tab Hold**         | Tactical map overlay          |
| **Mouse Wheel**      | Cycle weapons                 |
| **1**                | Assault rifle                 |
| **2**                | Shotgun                       |
| **3**                | Pistol                        |
| **F**                | Melee shove / close interrupt |

---

# 6. Gamepad Controls

| Input                | Action                   |
| -------------------- | ------------------------ |
| **Left Stick**       | Move                     |
| **Right Stick**      | Aim direction            |
| **RT / R2**          | Fire                     |
| **LT / L2 Hold**     | Aim stance / strafe-lock |
| **LB / L1 Hold**     | Crouch / cover           |
| **A / Cross**        | Dodge                    |
| **X / Square Tap**   | Reload / pickup          |
| **X / Square Hold**  | Objective interaction    |
| **RB / R1**          | Grenade                  |
| **Y / Triangle**     | Swap weapon              |
| **D-Pad Up**         | Radar pulse              |
| **D-Pad Left/Right** | Cycle weapons            |
| **View/Select**      | Tactical map             |

Controller should use **soft aim assist**. Mouse should not.

---

# 7. Input Priority Rules

The game needs strict input priority so the player never gets unpredictable behavior.

## Priority Order

Highest priority actions override lower priority actions.

```text
1. Death / downed state
2. Dodge
3. Grenade throw release
4. Objective interaction
5. Reload
6. Cover / crouch
7. Aim stance
8. Firing
9. Movement
10. Pickup detection
```

## Examples

If the player is reloading and presses dodge:

```text
Reload is canceled
Dodge begins
Magazine is not refilled
```

If the player is interacting with a terminal and takes damage:

```text
Interaction is interrupted
Player returns to cover/movement state
Progress is partially retained
```

If the player is crouched in cover and fires:

```text
Player enters pop-up fire state
Weapon fires
Player returns to cover when fire stops
```

---

# 8. Player State Machine

The player should always be in one major state.

```text
FreeMove
AimMove
CrouchMove
CoverCrouch
CoverPeekFire
Reloading
Dodge
ThrowGrenade
Interacting
Staggered
Dead
```

## State Transitions

```text
FreeMove
    -> AimMove when Right Click held
    -> CrouchMove when Ctrl held
    -> Dodge when Space pressed
    -> Reloading when R pressed
    -> ThrowGrenade when G pressed
    -> Interacting when E held near objective

AimMove
    -> FreeMove when Right Click released
    -> CoverCrouch when Ctrl held near cover
    -> Dodge when Space pressed
    -> Reloading when R pressed

CrouchMove
    -> CoverCrouch when near valid cover
    -> FreeMove when Ctrl released
    -> CoverPeekFire when firing from low cover

CoverCrouch
    -> CoverPeekFire when Left Click held
    -> FreeMove when Ctrl released or moved away
    -> Dodge when Space pressed
    -> Reloading when R pressed

CoverPeekFire
    -> CoverCrouch when Left Click released
    -> Reloading if magazine empty
    -> Staggered if heavy damage taken

Reloading
    -> Previous state when reload complete
    -> Dodge if Space pressed
    -> ThrowGrenade if G pressed
    -> Canceled if weapon switched

Interacting
    -> Objective complete if timer finishes
    -> Interrupted if damaged
    -> Interrupted if player releases E

Dodge
    -> FreeMove or AimMove after dodge finishes
```

---

# 9. Movement System

## Movement Direction

Movement is camera-relative.

```text
W = move toward top of screen
S = move toward bottom of screen
A = move left across screen
D = move right across screen
```

This is important. The player should not move relative to the character’s facing direction.

## Movement Speeds

| State                |       Speed |
| -------------------- | ----------: |
| Free movement        |        100% |
| Aim stance movement  |         70% |
| Crouch movement      |         45% |
| Cover shuffle        |         35% |
| Reload movement      |         55% |
| Heavy damage stagger | 20% briefly |
| Dodge                | Fixed burst |

Recommended values:

```text
FreeMoveSpeed = 5.2 m/s
AimMoveSpeed = 3.6 m/s
CrouchMoveSpeed = 2.2 m/s
CoverShuffleSpeed = 1.8 m/s
ReloadMoveSpeed = 2.8 m/s
DodgeDistance = 3.2 m
DodgeDuration = 0.38 s
```

---

# 10. Rotation and Facing

## Normal Movement

When not aiming:

```text
Character body faces movement direction
Weapon lowered slightly
Accuracy reduced if firing from hip
```

## Aim Stance

When holding Right Click:

```text
Character body faces cursor
Movement becomes strafing
Accuracy improves
Camera shifts toward aim direction
```

## Firing Without Aim Stance

The player can still fire without holding right click.

```text
Character quickly turns toward cursor
Accuracy is worse
Movement remains faster
```

This allows emergency hip-fire, but tactical play favors aim stance.

---

# 11. Aim System

## Cursor Projection

The mouse cursor is projected onto the ground plane.

```text
CursorWorldPosition = RaycastFromCameraToGround(MousePosition)
AimDirection = normalize(CursorWorldPosition - PlayerPosition)
```

## Aim Reticle States

| Reticle            | Meaning                  |
| ------------------ | ------------------------ |
| White              | Valid aim point          |
| Red                | Enemy under aim          |
| Yellow             | Objective under aim      |
| Green              | Pickup under aim         |
| Gray               | Shot blocked by cover    |
| Expanding reticle  | Weapon spread increasing |
| Red broken reticle | Target behind cover      |

## Line of Fire Check

Before firing:

```text
Raycast from weapon muzzle to cursor direction
Check collision with:
    enemy
    cover
    vehicle
    destructible prop
    terrain blocker
```

If shot is blocked:

```text
Reticle turns gray
Bullet impacts cover
No damage to target
```

---

# 12. Aim Assist Rules

## Mouse

No aim assist by default.

Optional accessibility setting:

```text
Mild reticle magnetism
Only at close range
Never snaps fully
```

## Controller

Use soft aim assist.

```text
Assist cone: 8–12 degrees
Assist range: weapon-dependent
Priority: enemy closest to right-stick direction
```

Aim assist should not track through cover.

```text
If target is behind cover:
    Do not assist
```

---

# 13. Cover System

Cover is the central mechanic.

## Cover Object Data

Every cover object should have gameplay metadata.

```text
CoverObject:
    CoverType
    CoverHeight
    CoverNormal
    CoverSlots
    PeekPoints
    Durability
    BlocksLineOfSight
    BlocksBullets
```

## Cover Types

| Cover Type         | Behavior                               |
| ------------------ | -------------------------------------- |
| Low cover          | Player can crouch behind it and pop up |
| High cover         | Blocks line of sight fully             |
| Soft cover         | Reduces accuracy / partial protection  |
| Destructible cover | Can break under damage                 |
| Explosive cover    | Explodes when damaged enough           |

---

# 14. Entering Cover

The game does not use sticky automatic cover. The player controls it with **Ctrl**.

## Procedure

When Ctrl is held:

```text
Check for valid cover within 1.2 meters
Check if cover is between player and nearest threat or aim direction
If valid:
    Enter CoverCrouch
Else:
    Enter CrouchMove
```

## Valid Cover Conditions

```text
Distance to cover <= 1.2 m
Cover height is low or medium
Player is on usable side of cover
Cover has line blocking toward threat direction
```

## Invalid Cover Conditions

```text
Cover is behind the player relative to threat
Cover is too tall to pop over
Cover is destroyed
Cover side is blocked
Player is already dodging or interacting
```

---

# 15. Cover Behavior

## CoverCrouch State

While in CoverCrouch:

```text
Player is crouched
Incoming bullets from front are blocked by cover
Player can shuffle left/right along cover
Player cannot fire unless peeking
Player receives reduced suppression
```

## Cover Shuffle

If the player moves along the cover direction:

```text
Player slides along cover edge
Speed = CoverShuffleSpeed
```

If the player moves away from cover:

```text
Exit cover after 0.2 seconds
Return to crouch or free move
```

---

# 16. Pop-Up Shooting

This is the main cover-combat rhythm.

## Trigger

```text
Player is in CoverCrouch
Player holds Left Click
Weapon has ammo
```

## Procedure

```text
1. Player rises from cover
2. Weapon becomes exposed
3. Fire begins after 0.12s peek delay
4. Bullets fire toward cursor
5. Player remains partially exposed while firing
6. On fire release, player drops back into cover
```

## Timing

```text
Peek-up time: 0.12 s
Return-to-cover time: 0.10 s
Minimum exposure time: 0.25 s
```

## Risk Rule

The player is vulnerable while peeking.

```text
Incoming bullets can hit player during CoverPeekFire
Heavy enemies punish long peeks
```

## Anti-Abuse Rule

If the player fires continuously from cover:

```text
Weapon spread increases
Enemy suppression increases
Heavy enemy prioritizes player
```

The ideal player behavior is:

```text
Peek
Burst fire
Drop
Wait
Peek again
```

---

# 17. High Cover

High cover blocks line of sight completely.

Examples:

```text
Cargo containers
Large trucks
Comms tower base
Tall concrete walls
Military tents
```

Behind high cover:

```text
Player cannot shoot through it
Enemies cannot shoot through it
Radar still works
Camera may fade the object if it blocks view
```

High cover is for:

```text
Breaking enemy line of sight
Reloading safely
Flanking
Avoiding heavy suppression
```

---

# 18. Destructible Cover

Only some cover should break in Level 1.

Destructible:

```text
Wooden crates
Some sandbags
Light barricades
Control panels
```

Non-destructible:

```text
Concrete barriers
Cargo containers
Armored vehicles
Large trucks
Comms tower base
```

## Destruction Procedure

```text
Cover has HP
Bullets reduce HP
Grenades reduce HP heavily
At 0 HP:
    swap to destroyed mesh
    spawn debris VFX
    remove bullet blocking
```

Recommended values:

```text
Wooden crate HP: 60
Sandbag section HP: 180
Light metal crate HP: 220
Concrete barrier HP: infinite
```

---

# 19. Dodge System

## Input

```text
Space = dodge
```

## Direction

If movement input exists:

```text
DodgeDirection = MovementInputDirection
```

If no movement input exists:

```text
DodgeDirection = opposite of AimDirection
```

This creates a useful panic-backstep.

## Dodge Procedure

```text
1. Cancel reload/interact if active
2. Lock player direction briefly
3. Move player DodgeDistance over DodgeDuration
4. Disable firing during dodge
5. Reduce incoming accuracy briefly
6. Enter recovery for 0.18s
```

## Dodge Values

```text
DodgeDistance = 3.2 m
DodgeDuration = 0.38 s
RecoveryTime = 0.18 s
Cooldown = 1.0 s
```

## Dodge Restrictions

```text
Cannot dodge while stunned
Cannot dodge while dead
Cannot fire during dodge
Cannot interact during dodge
Reload is canceled
Grenade throw is canceled unless already released
```

## Dodge Damage Rule

Do not use huge invincibility frames.

Instead:

```text
During dodge, enemy tracking accuracy drops by 60%
Projectile hits still count if physically intersecting player
```

This keeps dodge tactical rather than magical.

---

# 20. Reload System

## Reload Input

```text
R = reload current weapon
```

## Reload Procedure

```text
1. Check magazine not full
2. Check reserve ammo available
3. Start reload animation
4. Apply movement penalty
5. At reload commit frame, transfer ammo
6. End reload state
```

## Reload Cancel

Reload is canceled by:

```text
Dodge
Weapon switch
Grenade throw
Taking heavy stagger damage
Objective interaction
```

If canceled before the commit frame:

```text
No ammo is loaded
```

If canceled after commit frame:

```text
Ammo remains loaded
```

## Reload Commit Frames

| Weapon        |    Reload Time |            Commit Point |
| ------------- | -------------: | ----------------------: |
| Pistol        |          1.1 s |                  0.75 s |
| Assault rifle |          1.7 s |                  1.15 s |
| Shotgun       | Shell-by-shell | Each shell after 0.45 s |
| LMG enemy     |          3.2 s |                   2.2 s |

---

# 21. Weapon System

Each weapon has a data table.

```text
Weapon:
    Damage
    MagazineSize
    ReserveAmmo
    FireRate
    ReloadTime
    Range
    SpreadBase
    SpreadMoving
    SpreadCrouched
    SpreadPerShot
    SpreadRecovery
    Recoil
    MobilityPenalty
    ProjectileSpeed
    Penetration
```

---

# 22. Assault Rifle

The assault rifle is the default tactical weapon.

```text
Magazine: 30
Reserve: 90
Damage: 23
Fire rate: 650 RPM
Reload: 1.7 s
Best range: medium
Movement penalty: small
```

## Procedural Behavior

```text
Standing hip-fire:
    medium spread

Aim stance:
    reduced spread

Crouched:
    further reduced spread

Sustained fire:
    spread increases after each shot

Burst fire:
    spread recovers quickly
```

Design intent:

```text
Best used in 3–6 round bursts from cover.
```

---

# 23. Pistol

The pistol is the fallback weapon.

```text
Magazine: 12
Reserve: 48 or infinite low reserve
Damage: 18
Fire rate: semi-auto
Reload: 1.1 s
Best range: short-medium
Movement penalty: none
```

## Procedural Behavior

```text
Accurate first shot
Low recoil
Fast draw
Weak against armor
Good while moving
```

Design intent:

```text
Emergency weapon when primary is dry.
```

---

# 24. Benelli M4 Shotgun

The shotgun is the close-range answer to rushers.

```text
Magazine: 7
Reserve: 28
Pellets: 8
Damage per pellet: 10
Reload: shell-by-shell
Best range: close
Spread: wide
Fire rate: moderate
```

## Procedural Behavior

```text
Each shot fires 8 pellet traces
Each pellet has separate collision
Damage falls off after short range
Reload loads one shell at a time
Player can interrupt reload after any shell
```

Design intent:

```text
Devastating near cover corners.
Weak in open lanes.
```

---

# 25. Grenade System

## Inputs

```text
Tap G = quick throw to cursor
Hold G = show throw arc
Release G = throw
Right Click while holding G = cancel
```

## Throw Procedure

```text
1. Player enters ThrowGrenade state
2. Movement slows to 50%
3. Arc preview appears if G is held
4. Throw distance clamps to max range
5. On release, grenade spawns from hand
6. Grenade follows arc to target
7. Fuse starts on release or on landing depending setting
8. Explosion applies radius damage
```

Recommended:

```text
Fuse starts on landing
Fuse time: 1.2 s
Max throw distance: 12 m
Blast radius: 4 m
Inner lethal radius: 2 m
```

## Damage

```text
0–2 m: 120 damage
2–4 m: falloff to 25 damage
Cover blocks or reduces damage
```

## Enemy Reaction

When grenade lands near enemy:

```text
Enemy checks distance
If within danger radius:
    break current behavior
    move away from grenade if route exists
    otherwise crouch/panic
```

Heavy enemies react slower.

---

# 26. Melee Shove

Melee is not a full combat system. It is a defensive interrupt.

```text
F = melee shove
Range: 1.4 m
Cooldown: 1.2 s
Damage: low
Effect: staggers rusher briefly
```

Use case:

```text
Rusher reaches player
Player shoves
Player dodges or shotgun fires
```

Do not make melee stronger than guns.

---

# 27. Health and Armor

## Player Stats

```text
Health: 100
Armor: 75
Max Armor: 100
```

## Damage Order

```text
If armor > 0:
    armor absorbs damage first
    leftover damage goes to health
Else:
    damage goes to health
```

## Armor Damage Modifier

Armor reduces some incoming damage.

```text
DamageToArmor = IncomingDamage * 0.85
DamageLeakToHealth = IncomingDamage * 0.15 if armor is hit hard
```

Simpler version:

```text
Armor absorbs all damage until depleted.
```

For Level 1, use the simpler version.

---

# 28. Damage Feedback

When the player takes damage:

```text
Directional damage indicator appears
Controller/keyboard rumble optional
Armor hit sound if armor absorbs
Health hit sound if health takes damage
Small camera impulse
HUD flashes briefly
```

## Low Health

At low health:

```text
Health below 30:
    heartbeat audio
    HUD pulse
    desaturated edge vignette
```

Do not overdo screen effects. The isometric view must remain readable.

---

# 29. Death and Checkpoints

## Death Procedure

```text
1. Health reaches 0
2. Player enters Dead state
3. Input disabled
4. Camera centers slightly on body
5. Fade out after 1.5 s
6. Reload latest checkpoint
```

## Checkpoints

Level 1 checkpoints:

```text
Checkpoint 1: After first patrol
Checkpoint 2: After Terminal Alpha
Checkpoint 3: After Terminal Bravo
Checkpoint 4: Before Heavy fight
Checkpoint 5: After Terminal Charlie
```

Checkpoint stores:

```text
Player health
Player armor
Current weapons
Ammo counts
Grenade count
Completed objectives
Dead enemies
Collected pickups
Destroyed props
```

---

# 30. Pickup System

## Pickup Types

```text
Ammo box
Medkit
Armor plate
Grenade pickup
Shotgun pickup
Weapon pickup
```

## Pickup Detection

Each pickup has an interaction radius.

```text
PickupRadius = 1.5 m
```

When player enters radius:

```text
Show world label
Show outline
Display input prompt
```

## Pickup Priority

If multiple interactables are nearby:

```text
1. Objective terminal
2. Weapon pickup
3. Health/armor pickup
4. Ammo pickup
5. Misc prop
```

## Auto vs Manual Pickup

Recommended:

```text
Ammo, medkit, armor, grenades = auto pickup on contact if useful
Weapons = press E to pick up/swap
Objectives = hold E
```

## Pickup Rules

Ammo pickup:

```text
If current weapon reserve is not full:
    add ammo
Else:
    leave pickup
```

Medkit:

```text
If health < max:
    restore health
Else:
    leave pickup
```

Armor plate:

```text
If armor < max:
    restore armor
Else:
    leave pickup
```

Grenade pickup:

```text
If grenade count < max:
    add grenade
Else:
    leave pickup
```

---

# 31. Objective Interaction

## Objective Input

```text
Hold E near terminal
```

## Terminal Procedure

```text
1. Player enters interaction zone
2. Prompt appears: HOLD E TO SABOTAGE
3. Player holds E
4. Interaction progress begins
5. Player cannot fire or dodge without canceling
6. Taking damage interrupts
7. On complete, terminal changes to disabled state
8. Objective count updates
9. Reinforcement event triggers
```

## Interaction Times

```text
Terminal Alpha: 3.0 s
Terminal Bravo: 3.0 s
Terminal Charlie: 5.0 s
```

## Interruption Rules

If player releases E:

```text
Interaction pauses
Progress remains for 2 seconds
Then slowly decays to nearest checkpoint threshold
```

If player takes damage:

```text
Interaction interrupts immediately
Progress remains at 50% if above 50%
```

This prevents the interaction from feeling unfair.

---

# 32. Terminal State Machine

Each terminal has states.

```text
Inactive
Active
Interacting
Interrupted
Sabotaged
Destroyed / Disabled
```

## Terminal Alpha

```text
Starts active
Sabotage time: 3 seconds
Triggers light reinforcement
```

## Terminal Bravo

```text
Starts locked until Alpha complete
Sabotage time: 3 seconds
Triggers flank reinforcement
```

## Terminal Charlie

```text
Starts locked until Bravo complete
Sabotage time: 5 seconds
Triggers final comms shutdown
Unlocks extraction
```

---

# 33. Radar System

The radar is a gameplay tool, not decoration.

## Default Radar

Always shows:

```text
Player position
Player facing
Current objective direction
Radar radius ring
```

## Radar Icons

| Icon                | Meaning               |
| ------------------- | --------------------- |
| Blue/white triangle | Player                |
| Red diamond         | Enemy                 |
| Large red diamond   | Heavy enemy           |
| Yellow square       | Objective             |
| Purple square       | Extraction            |
| Green square        | Pickup                |
| White pulse         | Noise/gunfire         |
| Red wedge           | Enemy alert direction |

---

# 34. Radar Pulse

## Input

```text
Q = radar pulse
```

## Procedure

```text
1. Player presses Q
2. Radar emits expanding scan ring
3. Enemies within scan range are revealed temporarily
4. Pickups within scan range appear temporarily
5. Objective marker brightens
6. Cooldown begins
```

## Values

```text
Scan radius: 35 m
Reveal duration: 4 s
Cooldown: 8 s
```

## Enemy Reveal Rules

Enemies appear on radar if:

```text
They are firing
They are alerted
They are within active radar pulse
They are close to player
They are tagged by objective script
```

Enemies do not appear if:

```text
They are idle and outside radar pulse
They are behind radar jammer area
They are not yet spawned
```

---

# 35. Tactical Map

## Input

```text
Hold Tab = tactical overlay
```

## Behavior

Tab does not pause the game unless using accessibility mode.

It shows:

```text
Objective direction
Known enemy positions
Known pickups
Extraction route if unlocked
Current terminal progress
```

The player should not use it constantly. The radar is the main navigation tool.

---

# 36. Enemy AI Overview

Enemies should be readable, not genius.

## AI State Machine

```text
Patrol
Suspicious
Alert
MoveToCover
AttackFromCover
Suppress
Flank
Rush
Reload
Retreat
GrenadeEvade
Dead
```

## AI Update Procedure

Each AI updates in this order:

```text
1. Check if dead
2. Update perception
3. Update alert state
4. Evaluate threat
5. Choose behavior
6. Move or hold position
7. Aim
8. Fire
9. Reload if needed
10. Update radar visibility
```

---

# 37. Enemy Perception

## Sight

```text
Vision range: 22 m
Vision cone: 120 degrees
Line-of-sight required
```

## Hearing

```text
Gunfire hearing radius: 30 m
Grenade explosion hearing radius: 45 m
Footstep hearing: not needed in Level 1
```

## Suspicion

If enemy hears something but does not see player:

```text
Enemy enters Suspicious
Moves toward last known sound
Radar may show faint red ping
```

## Alert

If enemy sees player or is attacked:

```text
Enemy enters Alert
Shares alert with nearby enemies within 18 m
```

---

# 38. Enemy Cover Selection

When an enemy decides to take cover:

```text
1. Find cover objects within search radius
2. Reject cover already occupied
3. Reject cover exposed to player
4. Score cover by distance, protection, firing angle
5. Move to best cover
```

## Cover Score

```text
Score =
    ProtectionScore * 3
  + FiringAngleScore * 2
  - DistanceCost
  - OccupancyPenalty
```

Enemies should not always pick perfect cover. Add randomness.

```text
FinalScore += random(-15%, +15%)
```

---

# 39. Rifleman AI

The rifleman is the standard enemy.

## Behavior

```text
1. Patrol or guard
2. Detect player
3. Move to nearest good cover
4. Fire 3–5 round bursts
5. Duck/reload
6. Reposition if suppressed
7. Occasionally flank if player stays hidden
```

## Values

```text
Health: 45
Burst length: 3–5 shots
Time between bursts: 1.0–1.8 s
Reload time: 1.8 s
Flank chance: low
```

Player counter:

```text
Cover peeking
Burst fire
Grenades
Flanking
```

---

# 40. Rusher AI

The rusher is anti-camping pressure.

## Visual Identity

The rusher should be:

```text
Bulkier than a normal rifleman
Less armored than heavy
Aggressive posture
Red accents
Close-range weapon
```

He should look like a **breacher**, not a skinny scout.

## Behavior

```text
1. Detect player
2. Move quickly between cover
3. Close distance to 6–8 m
4. Fire short close-range bursts
5. Dodge sideways if aimed at
6. Force player out of static cover
```

## Values

```text
Health: 60
Speed: 115% of rifleman
Preferred range: 5–8 m
Weapon: SMG or shotgun
Cover use: brief, not static
Flank chance: high
```

Player counter:

```text
Shotgun
Dodge backward
Melee shove
Radar awareness
```

---

# 41. Heavy AI

The heavy controls space.

## Behavior

```text
1. Advances slowly
2. Suppresses player cover
3. Fires long LMG bursts
4. Pauses to reload
5. Turns slowly
6. Punishes exposed player
7. Vulnerable during reload or from side
```

## Values

```text
Health: 120
Armor: 80
Move speed: 60% of rifleman
Burst length: 2.5–4.0 s
Reload time: 3.2 s
Turn speed: slow
Suppression radius: wide
```

Player counter:

```text
Grenade
Flank
Wait for reload
Use high cover
Do not trade fire directly
```

---

# 42. Suppression System

Suppression gives heavy and rifle fire tactical meaning without instantly killing the player.

## Suppression Sources

```text
Enemy bullets passing near player
Heavy LMG sustained fire
Multiple riflemen firing at same cover
```

## Suppression Effects

If suppressed:

```text
Player aim spread increases
Screen edges pulse subtly
Radar audio becomes muffled
Cover exit feels risky
```

Do not take control away from the player.

## Suppression Values

```text
Near miss adds suppression
Being in cover reduces suppression gain
Suppression decays after 2 seconds without near misses
```

---

# 43. Enemy Accuracy

Enemy accuracy should depend on player state.

| Player State     |                      Enemy Accuracy |
| ---------------- | ----------------------------------: |
| Standing exposed |                                High |
| Moving           |                              Medium |
| Aim stance       |                         Medium-high |
| Crouched in open |                          Medium-low |
| Behind cover     |                  Very low / blocked |
| Dodging          |                    Reduced tracking |
| Suppressed       | Enemies fire more but not perfectly |

This makes cover meaningful.

---

# 44. Projectile and Hit Rules

Use visible tracers for readability.

## Player Bullets

```text
Fast projectile or hitscan with tracer
Line collision required
Impacts spawn effects
Damage applied on hit
```

## Enemy Bullets

Enemy bullets should be slightly readable.

```text
Use tracers
Use muzzle flashes
Use audio barks
Use cover impact effects
```

## Hit Priority

A bullet checks collision in this order:

```text
1. Cover
2. Enemy/player hitbox
3. Destructible props
4. Explosive props
5. Terrain
```

Cover should reliably block bullets.

---

# 45. Explosive Barrels

Explosive barrels create tactical opportunities.

## Procedure

```text
1. Barrel takes damage
2. If HP reaches 0, start short fuse
3. Flash/spark for 0.4 s
4. Explode
5. Damage enemies, player, cover
6. Spawn fire/smoke decal
```

Recommended values:

```text
HP: 40
Explosion radius: 3.5 m
Damage center: 100
Delay after destruction: 0.4 s
```

---

# 46. Level 1 Mission Flow

The mechanics should be introduced procedurally.

## Phase 1: Entry

Player learns:

```text
Move
Aim
Fire
Reload
```

Enemies:

```text
2 riflemen
```

The enemies are lightly exposed.

---

## Phase 2: Cover Tutorial

Player reaches sandbags.

Prompt:

```text
Hold Ctrl behind cover.
Fire to pop up.
```

Enemies:

```text
3 riflemen behind low cover
```

Player must use cover to survive comfortably.

---

## Phase 3: Radar Tutorial

Player enters a branching route.

Prompt:

```text
Press Q to pulse radar.
Yellow marks objective.
Red marks hostile movement.
Green marks pickups.
```

Radar reveals:

```text
Objective marker
Enemy group
Armor pickup on flank route
```

---

## Phase 4: First Objective

Player reaches Terminal Alpha.

Prompt:

```text
Hold E to sabotage.
```

When completed:

```text
Alarm triggers
2 riflemen arrive
1 rusher arrives
```

This teaches that objectives create counterattacks.

---

## Phase 5: Shotgun and Rusher Fight

Player finds the shotgun pickup before a close-quarters area.

Enemies:

```text
2 riflemen
2 rushers
```

This teaches the shotgun’s purpose.

---

## Phase 6: Terminal Bravo

Player sabotages second terminal.

Enemy pressure:

```text
Riflemen from front
Rusher from side
```

Player must check radar and reposition.

---

## Phase 7: Heavy Introduction

UI warning:

```text
HEAVY UNIT DETECTED
```

Enemy group:

```text
1 heavy
2 riflemen
```

Pickup before fight:

```text
Grenade pickup
Ammo box
```

This teaches heavy counterplay.

---

## Phase 8: Terminal Charlie

Longer sabotage interaction.

```text
Hold E for 5 seconds
Damage interrupts
```

After completion:

```text
Comms array destroyed
Extraction unlocked
```

---

## Phase 9: Extraction

Radar switches to purple extraction marker.

Objective:

```text
Reach extraction
Hold zone for 20 seconds
```

Enemy waves:

```text
Wave 1: 3 riflemen
Wave 2: 2 rushers
Final pressure: riflemen or damaged heavy depending difficulty
```

Player wins by surviving, not necessarily killing everyone.

---

# 47. Procedural Encounter Template

Every combat arena should be built with this structure:

```text
1. Entry cover
2. Enemy cover
3. Flank route
4. Risk/reward pickup
5. Objective or exit
6. Reinforcement trigger
```

## Arena Start Procedure

```text
Player crosses trigger
Enemies enter alert or patrol
Radar updates objective direction
Music/combat layer begins
```

## Arena Clear Procedure

```text
All required enemies dead or objective completed
Combat music fades
Radar highlights next objective
Checkpoint may save
```

---

# 48. Level Script Triggers

Use triggers, not random spawning.

## Trigger Types

```text
AreaEnterTrigger
ObjectiveStartTrigger
ObjectiveCompleteTrigger
EnemyDeathTrigger
PickupCollectedTrigger
ExtractionStartTrigger
```

## Example

```text
On TerminalAlphaComplete:
    Set objective = Reach Terminal Bravo
    Spawn reinforcement squad north road
    Reveal red radar pings
    Play alarm
    Save checkpoint
```

---

# 49. UI Procedures

## Objective Update

When objective changes:

```text
1. Play objective beep
2. Animate top-left objective panel
3. Highlight marker on radar
4. Show brief world-space objective marker
```

## Low Ammo

When magazine is low:

```text
If ammo <= 25%:
    show low ammo warning
```

When empty:

```text
Play dry click
Flash reload prompt
Auto-switch to pistol only if setting enabled
```

## Pickup Display

When near pickup:

```text
Show item label
Show whether useful
Show E prompt for weapons
```

Example:

```text
SHOTGUN
Press E to equip
```

---

# 50. Difficulty Scaling

## Easy

```text
More armor pickups
Enemy accuracy reduced
Radar cooldown shorter
Terminal progress does not decay
```

## Normal

Default values.

## Hard

```text
Enemy accuracy increased
Armor pickups reduced
Radar reveal shorter
Rushers flank more often
Heavy reloads faster
```

Do not change controls by difficulty.

---

# 51. Recommended Update Order Per Frame

This is the procedural gameplay loop.

```text
1. Read player input
2. Resolve input priority
3. Update player state machine
4. Update camera target
5. Update aim cursor and reticle
6. Process movement
7. Process cover detection
8. Process firing/reload/grenade/interact
9. Update enemy perception
10. Update enemy AI states
11. Resolve projectiles and damage
12. Update pickups/objectives
13. Update radar visibility
14. Update UI/HUD
15. Update audio/VFX
16. Check win/loss conditions
```

---

# 52. Core Feel Rules

These are the non-negotiable design rules.

## Rule 1

The player should survive by using cover, not by circle-strafing forever.

## Rule 2

Right click is a deliberate combat stance, not just a tiny accuracy modifier.

## Rule 3

Ctrl cover must be reliable, simple, and readable.

## Rule 4

Pop-up firing is the main shooting rhythm.

## Rule 5

Rushers punish camping.

## Rule 6

Heavies punish long exposure.

## Rule 7

Grenades solve entrenched enemies.

## Rule 8

Radar guides the player but does not solve the whole battlefield.

## Rule 9

Objectives create pressure.

## Rule 10

Every arena needs a safe entry, enemy cover, flank route, and pickup temptation.

---

# Final Locked Control and Gameplay Identity

The game should now be defined as:

> **An isometric tactical cover shooter where the player uses WASD movement, mouse aim, right-click combat stance, Ctrl cover crouch, pop-up shooting, dodge repositioning, radar pulses, grenades, and objective interactions to push through structured combat arenas.**

The official Level 1 control set is:

```text
WASD              Move
Mouse             Aim cursor
Left Click        Fire
Right Click Hold  Aim stance / strafe-lock
Ctrl Hold         Crouch / cover
Space             Dodge
R                 Reload
E Tap             Pickup / quick interact
E Hold            Sabotage objective
G Tap             Quick grenade
G Hold            Grenade arc preview
Q                 Radar pulse
Tab Hold          Tactical map
Mouse Wheel       Cycle weapons
1 / 2 / 3          Direct weapon select
F                 Melee shove
```

The official gameplay loop is:

```text
Radar pulse
Move to cover
Crouch
Aim stance
Pop up and burst fire
Drop back
Reload or reposition
Use grenade against entrenched enemies
Push to objective
Sabotage terminal
Survive counterattack
Advance to next arena
Extract
```

This gives the game a defined procedural backbone instead of just a concept.
