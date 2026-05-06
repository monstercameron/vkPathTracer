$root = "assets/models/louvre_gallery"

function Write-Portrait([string]$stem, [int]$seed, [double[]]$base, [double[]]$accent) {
  $objPath = Join-Path $root "$stem.obj"
  $mtlPath = Join-Path $root "$stem.mtl"
  $cols = 18
  $rows = 26
  $verts = New-Object System.Collections.Generic.List[string]
  $uvs = New-Object System.Collections.Generic.List[string]
  $faces = New-Object System.Collections.Generic.List[string]
  $mtl = New-Object System.Collections.Generic.List[string]

  function AddV([double]$x, [double]$y, [double]$z, [double]$u, [double]$v) {
    $verts.Add(("v {0:F5} {1:F5} {2:F5}" -f $x, $y, $z))
    $uvs.Add(("vt {0:F5} {1:F5}" -f $u, $v))
    return $verts.Count
  }
  function Clamp01([double]$v) {
    return [Math]::Max(0.0, [Math]::Min(1.0, $v))
  }
  function PortraitColor([double]$nx, [double]$ny, [double[]]$base, [double[]]$accent, [int]$seed) {
    $hair = [Math]::Exp(-(($nx * 2.2) * ($nx * 2.2) + (($ny + 0.03) * 2.0) * (($ny + 0.03) * 2.0)))
    $face = [Math]::Exp(-(($nx * 4.0) * ($nx * 4.0) + (($ny + 0.13) * 5.3) * (($ny + 0.13) * 5.3)))
    $robe = [Math]::Exp(-(($nx * 2.0) * ($nx * 2.0) + (($ny - 0.36) * 2.8) * (($ny - 0.36) * 2.8)))
    $halo = [Math]::Exp(-(($nx * 2.9) * ($nx * 2.9) + (($ny + 0.23) * 3.2) * (($ny + 0.23) * 3.2)))
    $brush = 0.06 * [Math]::Sin(($nx * 35.0) + $seed) + 0.05 * [Math]::Cos(($ny * 31.0) - $seed)
    $r = $base[0] * (0.55 + $brush) + $accent[0] * ($robe * 0.55 + $halo * 0.18) + 0.78 * $face - 0.22 * $hair
    $g = $base[1] * (0.55 + $brush) + $accent[1] * ($robe * 0.55 + $halo * 0.18) + 0.58 * $face - 0.18 * $hair
    $b = $base[2] * (0.55 + $brush) + $accent[2] * ($robe * 0.55 + $halo * 0.18) + 0.38 * $face - 0.12 * $hair
    return @((Clamp01 $r), (Clamp01 $g), (Clamp01 $b))
  }

  $mtl.Add("# CPU-visible procedural side portrait for the Louvre gallery.")
  for ($y = 0; $y -lt $rows; $y++) {
    for ($x = 0; $x -lt $cols; $x++) {
      $nx = ($x + 0.5) / $cols - 0.5
      $ny = ($y + 0.5) / $rows - 0.5
      $c = PortraitColor $nx $ny $base $accent $seed
      $name = "{0}_tile_{1:D2}_{2:D2}" -f $stem, $x, $y
      $mtl.Add("")
      $mtl.Add("newmtl $name")
      $mtl.Add("family diffuse")
      $mtl.Add(("Kd {0:F4} {1:F4} {2:F4}" -f ($c[0] * 0.55), ($c[1] * 0.55), ($c[2] * 0.55)))
      $mtl.Add("Pr 0.66")
      $mtl.Add("sheen 0.12")
    }
  }

  $faces.Add("$stem")
  for ($y = 0; $y -lt $rows; $y++) {
    for ($x = 0; $x -lt $cols; $x++) {
      $name = "{0}_tile_{1:D2}_{2:D2}" -f $stem, $x, $y
      $faces.Add("usemtl $name")
      $x0 = -0.5 + $x / $cols
      $x1 = -0.5 + ($x + 1) / $cols
      $y0 = 0.70 - ($y + 1) * 1.4 / $rows
      $y1 = 0.70 - $y * 1.4 / $rows
      $u0 = $x / $cols
      $u1 = ($x + 1) / $cols
      $v0 = ($y + 1) / $rows
      $v1 = $y / $rows
      $a = AddV $x0 $y0 0.0 $u0 $v0
      $b = AddV $x1 $y0 0.0 $u1 $v0
      $c = AddV $x1 $y1 0.0 $u1 $v1
      $d = AddV $x0 $y1 0.0 $u0 $v1
      $faces.Add("f $a/$a $b/$b $c/$c")
      $faces.Add("f $a/$a $c/$c $d/$d")
    }
  }

  $obj = New-Object System.Collections.Generic.List[string]
  $obj.Add("mtllib $stem.mtl")
  $obj.Add("o $stem")
  $obj.AddRange($verts)
  $obj.AddRange($uvs)
  $obj.AddRange($faces)
  Set-Content -Path $objPath -Value $obj -Encoding ASCII
  Set-Content -Path $mtlPath -Value $mtl -Encoding ASCII
}

Write-Portrait "left_wall_renaissance_portrait" 7 @(0.24, 0.20, 0.16) @(0.72, 0.22, 0.18)
Write-Portrait "back_wall_blue_portrait" 19 @(0.18, 0.21, 0.25) @(0.16, 0.42, 0.72)
