Add-Type -AssemblyName System.Drawing

$root = "assets/models/louvre_gallery"
$texDir = Join-Path $root "textures"
$source = Join-Path $texDir "mona_lisa_c2rmf_retouched.jpg"
$objPath = Join-Path $root "mona_lisa_visible_mosaic.obj"
$mtlPath = Join-Path $root "mona_lisa_visible_mosaic.mtl"

$img = [System.Drawing.Bitmap]::FromFile((Resolve-Path $source))
try {
  $cols = 40
  $rows = 62
  $verts = New-Object System.Collections.Generic.List[string]
  $uvs = New-Object System.Collections.Generic.List[string]
  $faces = New-Object System.Collections.Generic.List[string]
  $mtl = New-Object System.Collections.Generic.List[string]

  function AddV([double]$x, [double]$y, [double]$z, [double]$u, [double]$v) {
    $script:verts.Add(("v {0:F5} {1:F5} {2:F5}" -f $x, $y, $z))
    $script:uvs.Add(("vt {0:F5} {1:F5}" -f $u, $v))
    return $script:verts.Count
  }

  $mtl.Add("# CPU-visible Mona Lisa color mosaic. Each material also binds the source image texture.")
  for ($y = 0; $y -lt $rows; $y++) {
    for ($x = 0; $x -lt $cols; $x++) {
      $px = [Math]::Min($img.Width - 1, [int](($x + 0.5) * $img.Width / $cols))
      $py = [Math]::Min($img.Height - 1, [int](($y + 0.5) * $img.Height / $rows))
      $c = $img.GetPixel($px, $py)
      $name = "mona_tile_{0:D2}_{1:D2}" -f $x, $y
      $mtl.Add("")
      $mtl.Add("newmtl $name")
      $mtl.Add("family diffuse")
      $gain = 0.46
      $mtl.Add(("Kd {0:F4} {1:F4} {2:F4}" -f (($c.R / 255.0) * $gain), (($c.G / 255.0) * $gain), (($c.B / 255.0) * $gain)))
      $mtl.Add("Pr 0.62")
      $mtl.Add("sheen 0.18")
      $mtl.Add("map_Kd textures/mona_lisa_c2rmf_retouched.jpg")
    }
  }

  $faces.Add("mtllib mona_lisa_visible_mosaic.mtl")
  $faces.Add("o MonaLisaVisibleMosaic")
  for ($y = 0; $y -lt $rows; $y++) {
    for ($x = 0; $x -lt $cols; $x++) {
      $name = "mona_tile_{0:D2}_{1:D2}" -f $x, $y
      $faces.Add("usemtl $name")
      $x0 = -0.5 + $x / $cols
      $x1 = -0.5 + ($x + 1) / $cols
      $y0 = 0.75 - ($y + 1) * 1.5 / $rows
      $y1 = 0.75 - $y * 1.5 / $rows
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
  $obj.AddRange([string[]]($faces | Select-Object -First 2))
  $obj.AddRange($verts)
  $obj.AddRange($uvs)
  $obj.AddRange([string[]]($faces | Select-Object -Skip 2))
  Set-Content -Path $objPath -Value $obj -Encoding ASCII
  Set-Content -Path $mtlPath -Value $mtl -Encoding ASCII
} finally {
  $img.Dispose()
}
