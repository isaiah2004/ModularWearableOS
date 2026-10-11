# Build the NOS simulator -> build/nos_sim.exe   (docs/NOS_SIM.md)
#   .\sim\nos_build.ps1              build
#   .\sim\nos_build.ps1 -Run         build and open the window
#   .\sim\nos_build.ps1 -Verify      also prove the cost-model hooks are pixel-exact
#   .\sim\nos_build.ps1 -Out x.exe   build under another name (when nos_sim.exe is running)
#   .\sim\nos_build.ps1 -Clean       rebuild every object
#   .\sim\nos_build.ps1 -NoCostModel -Out nos_sim_nocost.exe   hooks compiled out (pixel A/B)
param([string]$Out = 'nos_sim.exe', [switch]$Run, [switch]$Verify, [switch]$Debug, [switch]$Clean,
      [switch]$NoCostModel, [switch]$Strict)
#   -Strict: fail on any compile error (default: a broken file links its last good object)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$gcc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter gcc.exe -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName
if (-not $gcc) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
New-Item -ItemType Directory -Force "$root\build" | Out-Null

# --- sources -------------------------------------------------------------------------------
# NOS: everything under nos/ (apps, faces, ui, fonts subfolders included).
$nos = @(Get-ChildItem "$root\nos" -Recurse -Filter *.c | ForEach-Object FullName)
# Core modules NOS may use. settings.c is left out: it needs the old UI's globals (wos, apps);
# the one wake_service.c reads (g_setting_tilt_wake) is defined in sim/nos_hal_sim.c.
$coreNames = 'gfx','font','sprite','anim','notif','battery','pedometer','walk','motion_wake','wake_service'
$core = @($coreNames | ForEach-Object { "$root\core\$_.c" } | Where-Object { Test-Path $_ })
# Baked core fonts: used by the self-test screen and by ui.c's NOS_FONT_SHIM.
$coreFonts = @(Get-ChildItem "$root\core\fonts\*.c" | ForEach-Object FullName)
# Simulator: sim/nos_*.c (nos_ext_sim.c is the modules agent's hal_ext.h implementation).
$sim = @(Get-ChildItem "$root\sim\nos_*.c" | ForEach-Object FullName)

$defs = [System.Collections.Generic.List[string]]::new()
if (-not $NoCostModel) { $defs.Add('-DGFX_COST_MODEL') }
if (-not (Test-Path "$root\nos\fonts\nos_fonts.h")) { $defs.Add('-DNOS_FONT_SHIM') }

[string[]]$opt = if ($Debug) { @('-O0', '-g') } else { @('-O2') }
[string[]]$flags = $opt + @('-std=c99', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-format-truncation',
                  "-I$root\core", "-I$root\nos", "-I$root\nos\fonts", "-I$root\sim") + $defs.ToArray()
$srcs = $nos + $core + $coreFonts + $sim

# --- incremental, parallel compile -----------------------------------------------------------
# One object per source in build/nos_obj[_nocost]. An object is rebuilt when its source is
# newer, when any header under core/ nos/ sim/ is newer (headers are shared widely, so no
# per-file dependency scan), or when the flags change.
$objDir = if ($NoCostModel) { "$root\build\nos_obj_nocost" } else { "$root\build\nos_obj" }
if ($Clean -and (Test-Path $objDir)) { Remove-Item -Recurse -Force $objDir }
New-Item -ItemType Directory -Force $objDir | Out-Null
$flagSig = ($flags -join ' ')
$sigFile = "$objDir\flags.txt"
if (-not (Test-Path $sigFile) -or [IO.File]::ReadAllText($sigFile) -ne $flagSig) {
    Get-ChildItem "$objDir\*.o" -ErrorAction SilentlyContinue | Remove-Item -Force
    [IO.File]::WriteAllText($sigFile, $flagSig)
}
$newestHeader = (@(Get-ChildItem "$root\core", "$root\nos", "$root\sim" -Recurse -Filter *.h) |
                 Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum
$jobs = foreach ($s in $srcs) {
    $rel = $s.Substring($root.Length + 1) -replace '[\\/]', '_'
    $obj = "$objDir\$($rel -replace '\.c$', '.o')"
    $stale = -not (Test-Path $obj)
    if (-not $stale) {
        $ot = (Get-Item $obj).LastWriteTimeUtc
        $stale = (Get-Item $s).LastWriteTimeUtc -gt $ot -or $newestHeader -gt $ot
    }
    [pscustomobject]@{ Src = $s; Obj = $obj; Stale = $stale }
}
$todo = @($jobs | Where-Object Stale)
if ($todo.Count) {
    # compile to .tmp and replace on success, so a failed compile keeps the last good object
    if ($PSVersionTable.PSVersion.Major -ge 7) {
        $results = $todo | ForEach-Object -ThrottleLimit ([Environment]::ProcessorCount) -Parallel {
            $tmp = "$($_.Obj).$($using:PID).tmp"
            $cout = & $using:gcc @using:flags -c $_.Src -o $tmp 2>&1 | Out-String
            $ok = $LASTEXITCODE -eq 0
            if ($ok) { Move-Item -Force $tmp $_.Obj }
            [pscustomobject]@{ Src = $_.Src; Obj = $_.Obj; Ok = $ok; Out = $cout }
        }
    } else {                     # Windows PowerShell 5.1: no -Parallel, compile one by one
        $ErrorActionPreference = 'Continue'
        $results = foreach ($j in $todo) {
            $tmp = "$($j.Obj).$PID.tmp"
            $cout = & $gcc @flags -c $j.Src -o $tmp 2>&1 | Out-String
            $ok = $LASTEXITCODE -eq 0
            if ($ok) { Move-Item -Force $tmp $j.Obj }
            [pscustomobject]@{ Src = $j.Src; Obj = $j.Obj; Ok = $ok; Out = $cout }
        }
        $ErrorActionPreference = 'Stop'
    }
    $bad = @($results | Where-Object { -not $_.Ok })
    foreach ($r in $results) { if ($r.Out.Trim()) { Write-Host $r.Out.TrimEnd() } }
    if ($bad.Count) {
        $names = ($bad | ForEach-Object { $_.Src.Substring($root.Length + 1) }) -join ', '
        $missing = @($bad | Where-Object { -not (Test-Path $_.Obj) })
        # Seven teams share this build: by default a file that is broken mid-edit links its
        # last good object instead of blocking everyone (-Strict turns that off).
        if ($Strict -or $missing.Count) { throw "nos_sim build failed in: $names" }
        Write-Host "WARNING: $names failed to compile - linking the LAST GOOD object(s) instead" -ForegroundColor Yellow
    }
}
# Link to a temp file, then swap it in. Other teams may have nos_sim.exe running: Windows
# locks a running exe against overwrite but allows renaming it, so a locked one is moved
# aside (build\nos_sim.exe.<time>.old, cleaned up on later builds once it is free).
$target = "$root\build\$Out"
$newExe = "$target.new"
& $gcc @opt @($jobs | ForEach-Object Obj) -o $newExe -lgdi32 -luser32 -lwinmm -lm
if ($LASTEXITCODE -ne 0) { throw "nos_sim link failed" }
Get-ChildItem "$target.*.old" -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item -Force $_ -ErrorAction SilentlyContinue }
try { Move-Item -Force $newExe $target -ErrorAction Stop }
catch {
    Move-Item -Force $target "$target.$((Get-Date).ToString('HHmmssfff')).old"
    Move-Item -Force $newExe $target
}
Write-Host "built build\$Out  ($($srcs.Count) sources, $($todo.Count) compiled; $($defs -join ' '))"

# --- -Verify: the GFX_COST hooks must not change a single pixel ------------------------------
if ($Verify) {
    $tmp = Join-Path $env:TEMP 'nos_cost_verify'
    New-Item -ItemType Directory -Force $tmp | Out-Null
    foreach ($f in 'gfx', 'font', 'sprite') {
        git -C $root show "HEAD:core/$f.c" | Set-Content -Encoding utf8 "$tmp\$f.c"
    }
    $vf = @('-O2', '-std=c99', "-I$root\core", '-w')
    & $gcc @vf "$root\tools\gfx_cost_exact.c" "$root\core\gfx.c" "$root\core\font.c" "$root\core\sprite.c" @coreFonts -DGFX_COST_MODEL -o "$tmp\a.exe"
    & $gcc @vf "$root\tools\gfx_cost_exact.c" "$tmp\gfx.c" "$tmp\font.c" "$tmp\sprite.c" @coreFonts -o "$tmp\b.exe"
    & "$tmp\a.exe" "$tmp\a.bin" | Out-Null
    & "$tmp\b.exe" "$tmp\b.bin" | Out-Null
    $ha = (Get-FileHash "$tmp\a.bin").Hash; $hb = (Get-FileHash "$tmp\b.bin").Hash
    if ($ha -ne $hb) { throw "VERIFY FAIL: instrumented raster differs from HEAD" }
    Write-Host "verify: instrumented gfx/font/sprite render byte-identical to HEAD (400 random scenes)"
    & "$tmp\a.exe" --accuracy
}

if ($Run) { Start-Process "$root\build\$Out" -WorkingDirectory $root }
