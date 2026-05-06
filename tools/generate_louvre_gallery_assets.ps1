Add-Type -AssemblyName System.Drawing

$root = "assets/models/louvre_gallery"
$texDir = Join-Path $root "textures"
New-Item -ItemType Directory -Force $texDir | Out-Null

function Save-Texture([string]$name, [int]$w, [int]$h, [scriptblock]$pixel) {
  $format = [System.Drawing.Imaging.PixelFormat]::Format24bppRgb
  $bmp = New-Object System.Drawing.Bitmap $w, $h, $format
  $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
  $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $format)
  try {
    $stride = [Math]::Abs($data.Stride)
    $bytes = New-Object byte[] ($stride * $h)
    for ($y = 0; $y -lt $h; $y++) {
      $row = $y * $stride
      for ($x = 0; $x -lt $w; $x++) {
        $c = & $pixel $x $y $w $h
        $i = $row + $x * 3
        $bytes[$i + 2] = [byte][Math]::Max(0, [Math]::Min(255, [int]$c[0]))
        $bytes[$i + 1] = [byte][Math]::Max(0, [Math]::Min(255, [int]$c[1]))
        $bytes[$i + 0] = [byte][Math]::Max(0, [Math]::Min(255, [int]$c[2]))
      }
    }
    [Runtime.InteropServices.Marshal]::Copy($bytes, 0, $data.Scan0, $bytes.Length)
  } finally {
    $bmp.UnlockBits($data)
  }
  $path = Join-Path $texDir $name
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
}

function Hash01([int]$x, [int]$y, [int]$seed) {
  $v = [Math]::Sin($x * 12.9898 + $y * 78.233 + $seed * 37.719) * 43758.5453
  return $v - [Math]::Floor($v)
}

Save-Texture "louvre_limestone.png" 160 160 {
  param($x, $y, $w, $h)
  $grain = [double](Hash01 $x $y 17) * 16
  $vein = [Math]::Sin(($x + $y * 1.7) / 37.0) * 9 + [Math]::Sin(($x - $y * 0.6) / 19.0) * 4
  $panel = if ((($x % 128) -lt 3) -or (($y % 128) -lt 3)) { -22 } else { 0 }
  $r = 188 + $grain + $vein + $panel
  $g = 178 + $grain * 0.8 + $vein + $panel
  $b = 158 + $grain * 0.5 + $vein + $panel
  @($r, $g, $b)
}
Save-Texture "parquet_oak.png" 160 160 {
  param($x, $y, $w, $h)
  $tile = 64
  $bx = [Math]::Floor($x / $tile)
  $by = [Math]::Floor($y / $tile)
  $local = if ((($bx + $by) % 2) -eq 0) { $x % $tile } else { $y % $tile }
  $grain = [Math]::Sin($local / 2.7) * 18 + [double](Hash01 $x $y 31) * 14
  $seam = if (($x % $tile) -lt 2 -or ($y % $tile) -lt 2) { -38 } else { 0 }
  $r = 142 + $grain + $seam
  $g = 96 + $grain * 0.55 + $seam
  $b = 48 + $grain * 0.22 + $seam
  @($r, $g, $b)
}
Save-Texture "gilded_frame.png" 160 160 {
  param($x, $y, $w, $h)
  $grain = [double](Hash01 $x $y 51) * 34
  $wave = [Math]::Sin($x / 7.0) * 12 + [Math]::Cos($y / 11.0) * 9
  $r = 207 + $grain + $wave
  $g = 151 + $grain * 0.5 + $wave * 0.4
  $b = 54 + $grain * 0.2
  @($r, $g, $b)
}
Save-Texture "dark_walnut.png" 160 160 {
  param($x, $y, $w, $h)
  $grain = [Math]::Sin($x / 5.0 + [Math]::Sin($y / 41.0) * 3.0) * 24 + [double](Hash01 $x $y 73) * 20
  $r = 78 + $grain
  $g = 43 + $grain * 0.45
  $b = 22 + $grain * 0.25
  @($r, $g, $b)
}
Save-Texture "brushed_brass.png" 160 160 {
  param($x, $y, $w, $h)
  $line = [Math]::Sin($y / 2.0) * 10 + [double](Hash01 $x $y 91) * 12
  $r = 198 + $line
  $g = 151 + $line * 0.5
  $b = 61 + $line * 0.2
  @($r, $g, $b)
}
Save-Texture "red_velvet.png" 160 160 {
  param($x, $y, $w, $h)
  $nap = [double](Hash01 $x $y 111) * 25 + [Math]::Sin(($x + $y) / 18.0) * 18
  $r = 116 + $nap
  $g = 15 + $nap * 0.14
  $b = 26 + $nap * 0.2
  @($r, $g, $b)
}
Save-Texture "linen_label.png" 160 80 {
  param($x, $y, $w, $h)
  $fiber = [double](Hash01 $x $y 131) * 18 + [Math]::Sin($x / 3.0) * 4
  $r = 213 + $fiber
  $g = 202 + $fiber * 0.7
  $b = 178 + $fiber * 0.35
  @($r, $g, $b)
}
Save-Texture "clear_glass_tint.png" 64 64 {
  param($x, $y, $w, $h)
  @(178, 214, 226)
}

$mona = Join-Path $texDir "mona_lisa_c2rmf_retouched.jpg"
$validMona = $false
if (Test-Path $mona) {
  try {
    $img = [System.Drawing.Image]::FromFile((Resolve-Path $mona))
    $img.Dispose()
    $validMona = $true
  } catch {
    $validMona = $false
  }
}
if (-not $validMona) {
  Save-Texture "mona_lisa_c2rmf_retouched.jpg" 512 768 {
    param($x, $y, $w, $h)
    $nx = ($x / $w - 0.5)
    $ny = ($y / $h - 0.5)
    $bg = 62 + [Math]::Sin($x / 29.0) * 18 + [Math]::Sin($y / 43.0) * 12
    $face = [Math]::Exp(-(($nx * 3.1) * ($nx * 3.1) + (($ny + 0.08) * 4.2) * (($ny + 0.08) * 4.2)))
    $hair = [Math]::Exp(-(($nx * 2.1) * ($nx * 2.1) + (($ny + 0.02) * 2.0) * (($ny + 0.02) * 2.0))) - $face * 0.7
    $hands = [Math]::Exp(-((($nx + 0.09) * 5.0) * (($nx + 0.09) * 5.0) + (($ny - 0.31) * 14.0) * (($ny - 0.31) * 14.0)))
    @(42 + $bg * 0.7 + $face * 105 + $hands * 72 - $hair * 25,
      48 + $bg * 0.55 + $face * 75 + $hands * 50 - $hair * 20,
      35 + $bg * 0.35 + $face * 45 + $hands * 29 - $hair * 12)
  }
}

$verts = New-Object System.Collections.Generic.List[string]
$uvs = New-Object System.Collections.Generic.List[string]
$faces = New-Object System.Collections.Generic.List[string]
$curMat = ""

function Use-Mat([string]$m) {
  if ($script:curMat -ne $m) {
    $script:faces.Add("usemtl $m")
    $script:curMat = $m
  }
}
function AddV([double]$x, [double]$y, [double]$z, [double]$u, [double]$v) {
  $script:verts.Add(("v {0:F5} {1:F5} {2:F5}" -f $x, $y, $z))
  $script:uvs.Add(("vt {0:F5} {1:F5}" -f $u, $v))
  return $script:verts.Count
}
function AddQuad([int]$a, [int]$b, [int]$c, [int]$d) {
  $script:faces.Add("f $a/$a $b/$b $c/$c")
  $script:faces.Add("f $a/$a $c/$c $d/$d")
}
function AddBox([double]$cx, [double]$cy, [double]$cz, [double]$sx, [double]$sy, [double]$sz, [string]$mat) {
  Use-Mat $mat
  $x0 = $cx - $sx / 2; $x1 = $cx + $sx / 2
  $y0 = $cy - $sy / 2; $y1 = $cy + $sy / 2
  $z0 = $cz - $sz / 2; $z1 = $cz + $sz / 2
  $p = @(
    @($x0,$y0,$z1),@($x1,$y0,$z1),@($x1,$y1,$z1),@($x0,$y1,$z1),
    @($x1,$y0,$z0),@($x0,$y0,$z0),@($x0,$y1,$z0),@($x1,$y1,$z0),
    @($x0,$y0,$z0),@($x0,$y0,$z1),@($x0,$y1,$z1),@($x0,$y1,$z0),
    @($x1,$y0,$z1),@($x1,$y0,$z0),@($x1,$y1,$z0),@($x1,$y1,$z1),
    @($x0,$y1,$z1),@($x1,$y1,$z1),@($x1,$y1,$z0),@($x0,$y1,$z0),
    @($x0,$y0,$z0),@($x1,$y0,$z0),@($x1,$y0,$z1),@($x0,$y0,$z1)
  )
  for ($i = 0; $i -lt 24; $i += 4) {
    $a = AddV $p[$i][0] $p[$i][1] $p[$i][2] 0 0
    $b = AddV $p[$i + 1][0] $p[$i + 1][1] $p[$i + 1][2] 1 0
    $c = AddV $p[$i + 2][0] $p[$i + 2][1] $p[$i + 2][2] 1 1
    $d = AddV $p[$i + 3][0] $p[$i + 3][1] $p[$i + 3][2] 0 1
    AddQuad $a $b $c $d
  }
}
function AddCylinder([double]$cx, [double]$cy, [double]$cz, [double]$r, [double]$h, [int]$seg, [string]$mat) {
  Use-Mat $mat
  $bottom = @()
  $top = @()
  for ($i = 0; $i -lt $seg; $i++) {
    $ang = 2 * [Math]::PI * $i / $seg
    $x = $cx + [Math]::Cos($ang) * $r
    $z = $cz + [Math]::Sin($ang) * $r
    $u = $i / $seg
    $bottom += AddV $x ($cy - $h / 2) $z $u 0
    $top += AddV $x ($cy + $h / 2) $z $u 1
  }
  $cb = AddV $cx ($cy - $h / 2) $cz .5 .5
  $ct = AddV $cx ($cy + $h / 2) $cz .5 .5
  for ($i = 0; $i -lt $seg; $i++) {
    $j = ($i + 1) % $seg
    AddQuad $bottom[$i] $bottom[$j] $top[$j] $top[$i]
    $script:faces.Add("f $cb/$cb $($bottom[$j])/$($bottom[$j]) $($bottom[$i])/$($bottom[$i])")
    $script:faces.Add("f $ct/$ct $($top[$i])/$($top[$i]) $($top[$j])/$($top[$j])")
  }
}
function AddRope([double]$x0, [double]$z0, [double]$x1, [double]$z1, [double]$y, [double]$sag, [double]$r, [int]$pathSeg, [int]$ringSeg, [string]$mat) {
  Use-Mat $mat
  $rings = @()
  for ($i = 0; $i -le $pathSeg; $i++) {
    $t = $i / $pathSeg
    $x = $x0 + ($x1 - $x0) * $t
    $z = $z0 + ($z1 - $z0) * $t
    $yy = $y - $sag * [Math]::Sin([Math]::PI * $t)
    $ring = @()
    for ($j = 0; $j -lt $ringSeg; $j++) {
      $a = 2 * [Math]::PI * $j / $ringSeg
      $ring += AddV $x ($yy + [Math]::Sin($a) * $r) ($z + [Math]::Cos($a) * $r) $t ($j / $ringSeg)
    }
    $rings += ,$ring
  }
  for ($i = 0; $i -lt $pathSeg; $i++) {
    for ($j = 0; $j -lt $ringSeg; $j++) {
      $k = ($j + 1) % $ringSeg
      AddQuad $rings[$i][$j] $rings[$i + 1][$j] $rings[$i + 1][$k] $rings[$i][$k]
    }
  }
}

AddBox 0 0.42 1.85 2.2 0.16 0.62 "walnut"
for ($x = -0.95; $x -le 0.96; $x += 0.38) {
  AddBox $x 0.55 1.85 0.28 0.08 0.70 "walnut"
}
AddBox 0 0.25 1.53 2.0 0.16 0.13 "walnut"
AddBox 0 0.25 2.17 2.0 0.16 0.13 "walnut"
foreach ($x in @(-0.9, 0.9)) {
  foreach ($z in @(1.57, 2.13)) {
    AddCylinder $x 0.12 $z 0.045 0.24 48 "walnut"
    AddCylinder $x 0.02 $z 0.075 0.04 48 "brass"
  }
}
foreach ($x in @(-1.55, 1.55)) {
  foreach ($z in @(0.9, 2.8)) {
    AddCylinder $x 0.45 $z 0.045 0.9 64 "brass"
    AddCylinder $x 0.91 $z 0.11 0.08 64 "brass"
    AddCylinder $x 0.04 $z 0.18 0.08 64 "brass"
  }
}
AddRope -1.55 0.9 1.55 0.9 0.83 0.11 0.035 48 18 "velvet"
AddRope -1.55 2.8 1.55 2.8 0.83 0.11 0.035 48 18 "velvet"
AddRope -1.55 0.9 -1.55 2.8 0.80 0.08 0.035 32 18 "velvet"
AddRope 1.55 0.9 1.55 2.8 0.80 0.08 0.035 32 18 "velvet"
AddBox -1.2 0.9 1.15 0.42 0.035 0.26 "label"
AddBox 1.2 0.9 1.15 0.42 0.035 0.26 "label"

$mtl = @"
newmtl walnut
family wood
Kd 0.42 0.24 0.12
Pr 0.38
map_Kd textures/dark_walnut.png

newmtl brass
family metallic_pbr
Kd 0.95 0.74 0.28
Pr 0.18
Pm 1.0
map_Kd textures/brushed_brass.png

newmtl velvet
family fabric
Kd 0.48 0.02 0.05
Pr 0.78
sheen 0.65
map_Kd textures/red_velvet.png

newmtl label
family paper
Kd 0.86 0.82 0.72
Pr 0.68
map_Kd textures/linen_label.png
"@
Set-Content -Path (Join-Path $root "louvre_furniture_highres.mtl") -Value $mtl -Encoding ASCII

$obj = New-Object System.Collections.Generic.List[string]
$obj.Add("mtllib louvre_furniture_highres.mtl")
$obj.Add("o LouvreFurnitureHighRes")
$obj.AddRange($verts)
$obj.AddRange($uvs)
$obj.AddRange($faces)
Set-Content -Path (Join-Path $root "louvre_furniture_highres.obj") -Value $obj -Encoding ASCII
