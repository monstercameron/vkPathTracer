param(
  [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
  [string]$LodManifest = "game/models/lods/manifest.json",
  [string[]]$Outputs = @(
    "game/scenes/relay_yard_lowest_lod_demo.json",
    "assets/scenes/lowest_lod_asset_showcase.json"
  )
)

$ErrorActionPreference = "Stop"

function V3([double]$x, [double]$y, [double]$z) {
  return @([math]::Round($x, 6), [math]::Round($y, 6), [math]::Round($z, 6))
}

function Q([double]$x, [double]$y, [double]$z, [double]$w) {
  return @([math]::Round($x, 6), [math]::Round($y, 6), [math]::Round($z, 6), [math]::Round($w, 6))
}

function Transform([double]$x, [double]$y, [double]$z, [double]$scale, [double]$yawDegrees = 0.0) {
  $half = ($yawDegrees * [Math]::PI / 180.0) * 0.5
  return [ordered]@{
    translation = V3 $x $y $z
    rotation = Q 0.0 ([math]::Sin($half)) 0.0 ([math]::Cos($half))
    scale = V3 $scale $scale $scale
  }
}

function BoxTransform([double]$x, [double]$y, [double]$z, [double]$sx, [double]$sy, [double]$sz) {
  return [ordered]@{
    translation = V3 $x $y $z
    rotation = Q 0.0 0.0 0.0 1.0
    scale = V3 $sx $sy $sz
  }
}

function CylinderTransform([double]$x, [double]$y, [double]$z, [double]$radius, [double]$height) {
  return [ordered]@{
    translation = V3 $x $y $z
    rotation = Q 0.0 0.0 0.0 1.0
    scale = V3 $radius $height $radius
  }
}

function CameraTransform([double]$x, [double]$y, [double]$z, [double]$pitchDegrees) {
  $half = ($pitchDegrees * [Math]::PI / 180.0) * 0.5
  return [ordered]@{
    translation = V3 $x $y $z
    rotation = Q ([math]::Sin($half)) 0.0 0.0 ([math]::Cos($half))
    scale = V3 1.0 1.0 1.0
  }
}

function Add-BoxEntity($entities,
                       [int]$id,
                       [string]$name,
                       [double]$x,
                       [double]$y,
                       [double]$z,
                       [double]$sx,
                       [double]$sy,
                       [double]$sz,
                       [int]$materialId) {
  $entities.Add([ordered]@{
    id = $id
    name = $name
    transform = BoxTransform $x $y $z $sx $sy $sz
    mesh = [ordered]@{ mesh_id = 9101; material_id = $materialId }
  })
}

function Add-CylinderEntity($entities,
                            [int]$id,
                            [string]$name,
                            [double]$x,
                            [double]$surfaceY,
                            [double]$z,
                            [double]$radius,
                            [double]$height,
                            [int]$materialId) {
  $entities.Add([ordered]@{
    id = $id
    name = $name
    transform = CylinderTransform $x ($surfaceY + $height * 0.5) $z $radius $height
    mesh = [ordered]@{ mesh_id = 9102; material_id = $materialId }
  })
}

function Add-SpotLight($entities,
                       [int]$id,
                       [string]$name,
                       [double]$x,
                       [double]$y,
                       [double]$z,
                       [double[]]$color,
                       [double]$intensity,
                       [double[]]$direction,
                       [double]$beamDegrees = 52.0,
                       [double]$blend = 0.65,
                       [double]$radius = 0.35) {
  $entities.Add([ordered]@{
    id = $id
    name = $name
    transform = [ordered]@{
      translation = V3 $x $y $z
      rotation = Q 0.0 0.0 0.0 1.0
      scale = V3 1.0 1.0 1.0
    }
    light = [ordered]@{
      type = "spot"
      color = V3 $color[0] $color[1] $color[2]
      intensity = $intensity
      radius = $radius
      direction = V3 $direction[0] $direction[1] $direction[2]
      beam_angle_degrees = $beamDegrees
      blend = $blend
    }
  })
}

function Get-ModelBounds($lod) {
  $gltfPath = Join-Path $RepoRoot ($lod.gltf -replace "/", "\")
  $json = Get-Content $gltfPath -Raw | ConvertFrom-Json
  $mins = @(1.0e30, 1.0e30, 1.0e30)
  $maxs = @(-1.0e30, -1.0e30, -1.0e30)
  $found = $false

  foreach ($mesh in $json.meshes) {
    foreach ($primitive in $mesh.primitives) {
      $positionIndex = $primitive.attributes.POSITION
      if ($null -eq $positionIndex) {
        continue
      }
      $accessor = $json.accessors[[int]$positionIndex]
      if ($null -eq $accessor.min -or $null -eq $accessor.max) {
        continue
      }
      for ($i = 0; $i -lt 3; ++$i) {
        $mins[$i] = [math]::Min($mins[$i], [double]$accessor.min[$i])
        $maxs[$i] = [math]::Max($maxs[$i], [double]$accessor.max[$i])
      }
      $found = $true
    }
  }

  if (-not $found) {
    return [pscustomobject]@{
      min = @(-0.5, 0.0, -0.5)
      max = @(0.5, 1.0, 0.5)
      size = @(1.0, 1.0, 1.0)
      max_dimension = 1.0
    }
  }

  $size = @(
    [math]::Max(0.001, $maxs[0] - $mins[0]),
    [math]::Max(0.001, $maxs[1] - $mins[1]),
    [math]::Max(0.001, $maxs[2] - $mins[2])
  )
  return [pscustomobject]@{
    min = $mins
    max = $maxs
    size = $size
    max_dimension = ($size | Measure-Object -Maximum).Maximum
  }
}

function Display-Name([string]$source) {
  $stem = [IO.Path]::GetFileNameWithoutExtension($source)
  $stem = $stem -replace "^Meshy_AI_", ""
  $stem = $stem -replace "_texture$", ""
  $stem = $stem -replace "_\d{10}$", ""
  return (($stem -replace "_+", " ").Trim())
}

function Model-Placement([string]$source) {
  $name = [IO.Path]::GetFileNameWithoutExtension($source)
  $spec = @{
    target = 1.0
    x = 0.0
    z = 0.0
    surface = 0.0
    yaw = 0.0
    category = "misc"
  }

  switch -Regex ($name) {
    "Green_20_Foot_Cargo"     { $spec = @{ target = 6.1;  x = -12.6; z = -8.7; surface = 0.0;  yaw = 4.0;   category = "vehicle" }; break }
    "Mud_Caked_6x6_Armored"   { $spec = @{ target = 7.0;  x = -2.0;  z = -8.4; surface = 0.0;  yaw = -5.0;  category = "vehicle" }; break }
    "WWII_US_Army_2_5_Ton"    { $spec = @{ target = 6.4;  x = 9.8;   z = -8.2; surface = 0.0;  yaw = 8.0;   category = "vehicle" }; break }
    "Signal_Tower"            { $spec = @{ target = 6.8;  x = 17.2;  z = -8.4; surface = 0.0;  yaw = -16.0; category = "tower" }; break }
    "Black_Ops_Operator"      { $spec = @{ target = 1.78; x = -4.2;  z = -1.4; surface = 0.0;  yaw = 18.0;  category = "human" }; break }
    "Boho_Western_Muse"       { $spec = @{ target = 1.68; x = 0.0;   z = -1.2; surface = 0.0;  yaw = 0.0;   category = "human" }; break }
    "Modern_Tactical_Opera"   { $spec = @{ target = 1.78; x = 4.2;   z = -1.4; surface = 0.0;  yaw = -18.0; category = "human" }; break }
    "Comms_01"                { $spec = @{ target = 1.65; x = 7.2;   z = 2.2;  surface = 0.16; yaw = -30.0; category = "equipment" }; break }
    "Sabotage_Interface"      { $spec = @{ target = 1.35; x = 11.3;  z = 2.1;  surface = 0.16; yaw = 24.0;  category = "equipment" }; break }
    "Rusty_5000W_Portable"    { $spec = @{ target = 1.15; x = 15.0;  z = 2.2;  surface = 0.16; yaw = -10.0; category = "equipment" }; break }
    "Rusty_Vintage_Fuel"      { $spec = @{ target = 0.90; x = 17.4;  z = 2.6;  surface = 0.0;  yaw = 8.0;   category = "supply" }; break }
    "Weathered_Jersey_Barr"   { $spec = @{ target = 1.05; x = 13.3;  z = -2.3; surface = 0.0;  yaw = 90.0;  category = "barrier" }; break }
    "Stacked_U_S_Army_Amm"    { $spec = @{ target = 1.35; x = -17.0; z = 1.6;  surface = 0.16; yaw = 12.0;  category = "supply" }; break }
    "WWII_U_S_Army_Ammuni"    { $spec = @{ target = 0.65; x = -11.0; z = 4.9;  surface = 0.92; yaw = -10.0; category = "table" }; break }
    "5_56mm_Ammunition_Cra"   { $spec = @{ target = 0.55; x = -13.0; z = 4.9;  surface = 0.92; yaw = 8.0;   category = "table" }; break }
    "Colt_1911_Pistol"        { $spec = @{ target = 0.25; x = -16.2; z = 6.9;  surface = 0.92; yaw = 12.0;  category = "table" }; break }
    "Desert_Night_Assault"    { $spec = @{ target = 0.95; x = -14.2; z = 6.9;  surface = 0.92; yaw = -8.0;  category = "table" }; break }
    "Maelstrom_12"            { $spec = @{ target = 0.95; x = -12.0; z = 6.9;  surface = 0.92; yaw = 7.0;   category = "table" }; break }
    "Nightfall_AR_15"         { $spec = @{ target = 0.95; x = -9.7;  z = 6.9;  surface = 0.92; yaw = -5.0;  category = "table" }; break }
    "SP5K_Night_Raider"       { $spec = @{ target = 0.55; x = -7.6;  z = 6.9;  surface = 0.92; yaw = 9.0;   category = "table" }; break }
    "Pineapple_Grenade"       { $spec = @{ target = 0.13; x = -5.9;  z = 6.9;  surface = 0.92; yaw = 0.0;   category = "table" }; break }
    "Relic_of_Ironwood"       { $spec = @{ target = 0.65; x = -4.5;  z = 6.9;  surface = 0.92; yaw = -14.0; category = "table" }; break }
  }

  return [pscustomobject]$spec
}

function Make-RelativeUri([string]$scenePath, [string]$repoRelativeAssetPath) {
  $sceneDir = [IO.Path]::GetFullPath((Join-Path $RepoRoot ([IO.Path]::GetDirectoryName($scenePath))))
  $assetPath = [IO.Path]::GetFullPath((Join-Path $RepoRoot ($repoRelativeAssetPath -replace "/", "\")))
  if (-not $sceneDir.EndsWith([IO.Path]::DirectorySeparatorChar)) {
    $sceneDir += [IO.Path]::DirectorySeparatorChar
  }
  $baseUri = [Uri]::new($sceneDir)
  $assetUri = [Uri]::new($assetPath)
  return [Uri]::UnescapeDataString($baseUri.MakeRelativeUri($assetUri).ToString()).Replace("\", "/")
}

function Add-CylinderGeometry($geometry, [int]$id, [int]$materialId, [int]$segments = 48) {
  $vertices = New-Object System.Collections.Generic.List[object]
  $indices = New-Object System.Collections.Generic.List[object]
  $vertices.Add((V3 0.0 0.5 0.0))
  $vertices.Add((V3 0.0 -0.5 0.0))

  for ($i = 0; $i -lt $segments; ++$i) {
    $a = 2.0 * [Math]::PI * [double]$i / [double]$segments
    $x = [Math]::Cos($a)
    $z = [Math]::Sin($a)
    $vertices.Add((V3 $x 0.5 $z))
    $vertices.Add((V3 $x -0.5 $z))
  }

  for ($i = 0; $i -lt $segments; ++$i) {
    $next = ($i + 1) % $segments
    $top = 2 + $i * 2
    $bottom = $top + 1
    $nextTop = 2 + $next * 2
    $nextBottom = $nextTop + 1

    $indices.Add(0); $indices.Add($nextTop); $indices.Add($top)
    $indices.Add(1); $indices.Add($bottom); $indices.Add($nextBottom)
    $indices.Add($top); $indices.Add($nextTop); $indices.Add($nextBottom)
    $indices.Add($top); $indices.Add($nextBottom); $indices.Add($bottom)
  }

  $geometry.Add([ordered]@{
    id = $id
    primitive = "triangle"
    material_id = $materialId
    vertices = $vertices
    indices = $indices
  })
}

function Add-WarehouseShell($entities) {
  Add-BoxEntity $entities 9200 "large clean tile military warehouse floor" 0.0 -0.05 0.0 44.0 0.10 32.0 9001
  Add-BoxEntity $entities 9201 "dark insulated warehouse ceiling slab" 0.0 7.15 0.0 44.0 0.22 32.0 9003

  Add-BoxEntity $entities 9202 "rear wall lower concrete band below windows" 0.0 0.55 -16.0 44.0 1.10 0.22 9002
  Add-BoxEntity $entities 9203 "rear wall upper concrete band above windows" 0.0 5.55 -16.0 44.0 3.10 0.22 9002
  Add-BoxEntity $entities 9204 "rear wall left end pier" -21.3 2.55 -16.0 1.4 2.90 0.22 9002
  Add-BoxEntity $entities 9205 "rear wall right end pier" 21.3 2.55 -16.0 1.4 2.90 0.22 9002
  Add-BoxEntity $entities 9206 "rear wall window pier left" -10.8 2.55 -16.0 0.70 2.90 0.22 9002
  Add-BoxEntity $entities 9207 "rear wall window pier center" 0.0 2.55 -16.0 0.70 2.90 0.22 9002
  Add-BoxEntity $entities 9208 "rear wall window pier right" 10.8 2.55 -16.0 0.70 2.90 0.22 9002

  Add-BoxEntity $entities 9209 "left wall lower concrete band below windows" -22.0 0.55 0.0 0.22 1.10 32.0 9002
  Add-BoxEntity $entities 9210 "left wall upper concrete band above windows" -22.0 5.55 0.0 0.22 3.10 32.0 9002
  Add-BoxEntity $entities 9211 "right wall lower concrete band below windows" 22.0 0.55 0.0 0.22 1.10 32.0 9002
  Add-BoxEntity $entities 9212 "right wall upper concrete band above windows" 22.0 5.55 0.0 0.22 3.10 32.0 9002
  Add-BoxEntity $entities 9213 "left wall rear pier" -22.0 2.55 -13.8 0.22 2.90 1.0 9002
  Add-BoxEntity $entities 9214 "left wall middle pier" -22.0 2.55 -4.6 0.22 2.90 1.0 9002
  Add-BoxEntity $entities 9215 "left wall front pier" -22.0 2.55 4.6 0.22 2.90 1.0 9002
  Add-BoxEntity $entities 9216 "right wall rear pier" 22.0 2.55 -13.8 0.22 2.90 1.0 9002
  Add-BoxEntity $entities 9217 "right wall middle pier" 22.0 2.55 -4.6 0.22 2.90 1.0 9002
  Add-BoxEntity $entities 9218 "right wall front pier" 22.0 2.55 4.6 0.22 2.90 1.0 9002

  Add-BoxEntity $entities 9219 "open loading bay left column" -18.5 3.0 15.9 0.8 6.0 0.32 9002
  Add-BoxEntity $entities 9244 "open loading bay right column" 18.5 3.0 15.9 0.8 6.0 0.32 9002
  Add-BoxEntity $entities 9245 "open loading bay header" 0.0 6.45 15.9 37.8 0.7 0.32 9002

  Add-BoxEntity $entities 9246 "rear left HDRI window glass pane" -16.05 2.55 -15.86 9.0 2.45 0.035 9012
  Add-BoxEntity $entities 9247 "rear center HDRI window glass pane" -5.4 2.55 -15.86 5.8 2.45 0.035 9012
  Add-BoxEntity $entities 9248 "rear right HDRI window glass pane" 5.4 2.55 -15.86 5.8 2.45 0.035 9012
  Add-BoxEntity $entities 9249 "rear far right HDRI window glass pane" 16.05 2.55 -15.86 9.0 2.45 0.035 9012

  Add-BoxEntity $entities 9250 "left rear HDRI window glass pane" -21.86 2.55 -9.2 0.035 2.45 7.2 9012
  Add-BoxEntity $entities 9251 "left front HDRI window glass pane" -21.86 2.55 2.3 0.035 2.45 8.2 9012
  Add-BoxEntity $entities 9252 "right rear HDRI window glass pane" 21.86 2.55 -9.2 0.035 2.45 7.2 9012
  Add-BoxEntity $entities 9253 "right front HDRI window glass pane" 21.86 2.55 2.3 0.035 2.45 8.2 9012

  Add-BoxEntity $entities 9254 "center vehicle lane safety stripe" 0.0 0.02 -2.0 1.1 0.018 25.0 9006
  Add-BoxEntity $entities 9255 "left vehicle lane safety stripe" -7.4 0.02 -2.0 0.28 0.018 25.0 9006
  Add-BoxEntity $entities 9256 "right vehicle lane safety stripe" 7.4 0.02 -2.0 0.28 0.018 25.0 9006
}

function Add-TileFloorGrid($entities) {
  $id = 9300
  for ($x = -20.0; $x -le 20.01; $x += 2.0) {
    Add-BoxEntity $entities $id "clean floor tile grout line x $x" $x 0.013 0.0 0.035 0.012 31.5 9009
    ++$id
  }
  for ($z = -14.0; $z -le 14.01; $z += 2.0) {
    Add-BoxEntity $entities $id "clean floor tile grout line z $z" 0.0 0.014 $z 43.5 0.012 0.035 9009
    ++$id
  }
}

function Add-Table($entities, [int]$idBase, [string]$name, [double]$x, [double]$z, [double]$sx, [double]$sz) {
  Add-BoxEntity $entities ($idBase + 0) "$name steel tabletop" $x 0.86 $z $sx 0.10 $sz 9004
  Add-BoxEntity $entities ($idBase + 1) "$name left front leg" ($x - $sx * 0.42) 0.42 ($z - $sz * 0.36) 0.12 0.84 0.12 9004
  Add-BoxEntity $entities ($idBase + 2) "$name right front leg" ($x + $sx * 0.42) 0.42 ($z - $sz * 0.36) 0.12 0.84 0.12 9004
  Add-BoxEntity $entities ($idBase + 3) "$name left rear leg" ($x - $sx * 0.42) 0.42 ($z + $sz * 0.36) 0.12 0.84 0.12 9004
  Add-BoxEntity $entities ($idBase + 4) "$name right rear leg" ($x + $sx * 0.42) 0.42 ($z + $sz * 0.36) 0.12 0.84 0.12 9004
}

function Add-Pallet($entities, [int]$id, [string]$name, [double]$x, [double]$z, [double]$sx, [double]$sz) {
  Add-BoxEntity $entities $id $name $x 0.08 $z $sx 0.16 $sz 9005
}

function Turntable-Spec($placement) {
  $target = [double]$placement.target
  switch ($placement.category) {
    "vehicle" { return [pscustomobject]@{ radius = 3.85; height = 0.18; material = 9010 } }
    "tower" { return [pscustomobject]@{ radius = 2.05; height = 0.16; material = 9010 } }
    "human" { return [pscustomobject]@{ radius = 0.92; height = 0.11; material = 9010 } }
    "equipment" { return [pscustomobject]@{ radius = 1.05; height = 0.12; material = 9010 } }
    "supply" { return [pscustomobject]@{ radius = 0.82; height = 0.10; material = 9010 } }
    "barrier" { return [pscustomobject]@{ radius = 0.82; height = 0.10; material = 9010 } }
    "table" {
      $radius = [math]::Min(0.72, [math]::Max(0.18, $target * 0.72))
      return [pscustomobject]@{ radius = $radius; height = 0.075; material = 9011 }
    }
  }
  return [pscustomobject]@{ radius = [math]::Max(0.45, $target * 0.65); height = 0.10; material = 9010 }
}

function Add-CeilingLights($entities) {
  $lightId = 9400
  $panelId = 9260
  $rows = @(-10.5, -2.0, 6.5)
  $cols = @(-13.5, 0.0, 13.5)
  foreach ($z in $rows) {
    foreach ($x in $cols) {
      Add-BoxEntity $entities $panelId "high bay emissive ceiling panel $panelId" $x 6.96 $z 5.6 0.04 0.62 9007
      ++$panelId
      Add-SpotLight $entities $lightId "high bay warehouse spot $lightId" $x 6.75 $z @(0.92, 0.96, 1.0) 520.0 @(0.0, -1.0, 0.03) 68.0 0.78 0.7
      ++$lightId
    }
  }
}

function Build-Scene([string]$outputPath) {
  $manifestPath = Join-Path $RepoRoot ($LodManifest -replace "/", "\")
  $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
  $models = @($manifest.models | Sort-Object source)

  $assets = New-Object System.Collections.Generic.List[object]
  $entities = New-Object System.Collections.Generic.List[object]
  $materials = New-Object System.Collections.Generic.List[object]
  $geometry = New-Object System.Collections.Generic.List[object]

  $materials.Add([ordered]@{ id = 9001; name = "sealed warehouse concrete"; family = "diffuse"; albedo = V3 0.26 0.27 0.25; roughness = 0.92; metallic = 0.0; double_sided = $true })
  $materials.Add([ordered]@{ id = 9002; name = "painted reinforced concrete wall"; family = "diffuse"; albedo = V3 0.43 0.45 0.43; roughness = 0.86; metallic = 0.0; double_sided = $true })
  $materials.Add([ordered]@{ id = 9003; name = "dark corrugated warehouse ceiling"; family = "diffuse"; albedo = V3 0.23 0.24 0.25; roughness = 0.82; metallic = 0.05; double_sided = $true })
  $materials.Add([ordered]@{ id = 9004; name = "dark parkerized steel fixtures"; family = "metallic"; albedo = V3 0.22 0.23 0.23; roughness = 0.55; metallic = 0.65; double_sided = $true })
  $materials.Add([ordered]@{ id = 9005; name = "rough military wood pallet"; family = "rough_wood"; albedo = V3 0.42 0.30 0.18; roughness = 0.82; metallic = 0.0; double_sided = $true })
  $materials.Add([ordered]@{ id = 9006; name = "worn safety yellow floor paint"; family = "diffuse"; albedo = V3 0.95 0.68 0.12; roughness = 0.72; metallic = 0.0; double_sided = $true })
  $materials.Add([ordered]@{ id = 9007; name = "cool white high bay diffuser"; family = "emissive"; albedo = V3 0.88 0.95 1.0; emission = V3 0.75 0.86 1.0; emission_intensity = 28.0; roughness = 0.25; double_sided = $true })

  $geometry.Add([ordered]@{
    id = 9101
    primitive = "triangle"
    material_id = 9001
    vertices = @(
      (V3 -0.5 -0.5 -0.5),
      (V3 0.5 -0.5 -0.5),
      (V3 0.5 0.5 -0.5),
      (V3 -0.5 0.5 -0.5),
      (V3 -0.5 -0.5 0.5),
      (V3 0.5 -0.5 0.5),
      (V3 0.5 0.5 0.5),
      (V3 -0.5 0.5 0.5)
    )
    indices = @(0,1,2, 0,2,3, 4,6,5, 4,7,6, 0,4,5, 0,5,1, 3,2,6, 3,6,7, 1,5,6, 1,6,2, 0,3,7, 0,7,4)
  })

  Add-WarehouseShell $entities
  Add-Table $entities 9220 "front weapons inspection bench" -11.0 6.9 11.8 1.35
  Add-Table $entities 9230 "ammo inventory bench" -11.9 4.9 4.8 1.35
  Add-Pallet $entities 9240 "ammo stack pallet" -17.0 1.6 2.4 1.6
  Add-Pallet $entities 9241 "comms equipment pallet" 7.2 2.2 2.2 1.8
  Add-Pallet $entities 9242 "sabotage terminal pallet" 11.3 2.1 1.8 1.5
  Add-Pallet $entities 9243 "generator pallet" 15.0 2.2 2.0 1.5
  Add-CeilingLights $entities

  $modelBaseId = 10000
  $overheadLightBaseId = 9500
  for ($i = 0; $i -lt $models.Count; ++$i) {
    $model = $models[$i]
    $lod = @($model.lods | Where-Object { $_.index -eq 3 })[0]
    if ($null -eq $lod) {
      continue
    }

    $bounds = Get-ModelBounds $lod
    $placement = Model-Placement $model.source
    $scale = [double]$placement.target / [double]$bounds.max_dimension
    $y = [double]$placement.surface - ([double]$bounds.min[1] * $scale)
    $scaledHeight = [double]$bounds.size[1] * $scale
    $prettyName = Display-Name $model.source

    $assets.Add([ordered]@{
      id = $modelBaseId + $i
      type = "model/gltf"
      uri = Make-RelativeUri $outputPath $lod.gltf
      name = "$prettyName realistic lowest LOD"
      transform = Transform ([double]$placement.x) $y ([double]$placement.z) $scale ([double]$placement.yaw)
    })

    $lightY = [math]::Min(6.45, [math]::Max(2.4, [double]$placement.surface + $scaledHeight + 1.45))
    $lightZ = [double]$placement.z + 0.45
    $lightIntensity = if ($placement.category -eq "vehicle" -or $placement.category -eq "tower") { 620.0 } elseif ($placement.category -eq "table") { 210.0 } else { 360.0 }
    $beam = if ($placement.category -eq "vehicle" -or $placement.category -eq "tower") { 64.0 } elseif ($placement.category -eq "table") { 40.0 } else { 52.0 }
    Add-SpotLight $entities ($overheadLightBaseId + $i) "$prettyName overhead inspection spot" ([double]$placement.x) $lightY $lightZ @(1.0, 0.96, 0.88) $lightIntensity @(0.0, -1.0, -0.18) $beam 0.70 0.35
  }

  Add-SpotLight $entities 9700 "loading bay warm flood left" -12.0 5.7 12.8 @(1.0, 0.82, 0.58) 280.0 @(0.25, -1.0, -0.65) 68.0 0.74 0.6
  Add-SpotLight $entities 9701 "loading bay cool flood right" 12.0 5.7 12.8 @(0.72, 0.84, 1.0) 260.0 @(-0.25, -1.0, -0.65) 68.0 0.74 0.6
  Add-SpotLight $entities 9702 "rear wall vehicle rim left" -15.5 5.9 -14.4 @(0.58, 0.68, 1.0) 180.0 @(0.35, -0.9, 0.45) 60.0 0.66 0.5
  Add-SpotLight $entities 9703 "rear wall vehicle rim right" 15.5 5.9 -14.4 @(1.0, 0.62, 0.32) 180.0 @(-0.35, -0.9, 0.45) 60.0 0.66 0.5

  $entities.Add([ordered]@{
    id = 9800
    name = "Warehouse ambient fill"
    light = [ordered]@{
      type = "environment"
      color = V3 0.09 0.10 0.105
      intensity = 1.25
      radius = 0.0
    }
  })
  $entities.Add([ordered]@{
    id = 9900
    name = "Main warehouse camera"
    transform = CameraTransform 0.0 4.9 17.8 -11.0
    camera = [ordered]@{
      fov = 64.0
      near_plane = 0.05
      far_plane = 180.0
      focus_distance = 16.0
      f_stop = 7.0
      exposure_compensation = 2.35
    }
  })

  $scene = [ordered]@{
    metadata = [ordered]@{
      schema = "1.0"
      scene_name = "Lowest LOD Military Warehouse"
      author = "vkPathTracer"
      created = (Get-Date).ToString("yyyy-MM-dd")
      notes = "Generated military warehouse layout for all lod3_very_far_r0p1 game model assets. Assets are semantically scaled to human-realistic size, with vehicles on the floor, weapons and grenades on benches, concrete walls, ceiling, high-bay lights, and per-model inspection spots."
    }
    assets = $assets
    materials = $materials
    geometry = $geometry
    entities = $entities
    cameras = @(
      [ordered]@{
        id = 9900
        name = "Main warehouse camera"
        fov = 64.0
        near_plane = 0.05
        far_plane = 180.0
        focus_distance = 16.0
        f_stop = 7.0
        exposure_compensation = 2.35
      }
    )
    benchmark = [ordered]@{
      enabled = $true
      frame_target = 48
      warmup_frames = 4
    }
  }

  $fullOutput = Join-Path $RepoRoot ($outputPath -replace "/", "\")
  New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($fullOutput)) | Out-Null
  $scene | ConvertTo-Json -Depth 64 | Set-Content -Path $fullOutput -Encoding ASCII
}

foreach ($output in $Outputs) {
  Build-Scene $output
  Write-Host "Wrote $output"
}
