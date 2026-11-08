# Renders the hexaOS artwork (needs Ghostscript on Windows).
# Usage: powershell -File board\hexaos\art\render.ps1
$ErrorActionPreference = "Stop"
$gs = (Get-Command gswin64c -ErrorAction SilentlyContinue).Source
if (-not $gs) { $gs = "$env:LOCALAPPDATA\Programs\Ghostscript\bin\gswin64c.exe" }
$art = $PSScriptRoot
$overlay = Join-Path $art "..\rootfs_overlay\usr\share\hexaos"
New-Item -ItemType Directory -Force $overlay | Out-Null

Push-Location $art
try {
    $common = @("-q", "-dNOSAFER", "-dBATCH", "-dNOPAUSE", "-sDEVICE=png16m", "-r72",
                "-dGraphicsAlphaBits=4", "-dTextAlphaBits=4")
    & $gs @common "-g1920x1080" "-sOutputFile=$overlay\wallpaper.png" "wallpaper.ps"
    & $gs @common "-g480x420" "-sOutputFile=$art\splash-logo.png" "splash.ps"
    & $gs ($common -replace "png16m", "pngalpha") "-g24x24" "-sOutputFile=$overlay\wine.png" "wine-icon.ps"
    & $gs ($common -replace "png16m", "pngalpha") "-g24x24" "-sOutputFile=$overlay\settings.png" "settings-icon.ps"
    & $gs ($common -replace "png16m", "pngalpha") "-g24x24" "-sOutputFile=$overlay\setup.png" "setup-icon.ps"
    & $gs ($common -replace "png16m", "pngalpha") "-g24x24" "-sOutputFile=$overlay\power.png" "power-icon.ps"
} finally { Pop-Location }
Write-Output "ok: $overlay\wallpaper.png, $art\splash-logo.png"
