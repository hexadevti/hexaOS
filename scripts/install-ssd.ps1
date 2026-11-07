# Installs hexaOS into the UNALLOCATED space of the secondary SSD (dual boot with Windows).
# Run in PowerShell AS ADMINISTRATOR, with the image already built in WSL:
#   powershell -ExecutionPolicy Bypass -File scripts\install-ssd.ps1
#
# What it does:
#   1. creates 2 partitions in the free space: EFI (256 MB) and hexaOS root (the rest)
#   2. attaches the disk to WSL and writes the system (scripts/install-ssd.sh)
#   3. adds "hexaOS" to the firmware boot menu (F10), at the end of the list:
#      Windows stays the default
# Existing partitions are not touched. D: is unavailable while writing.
param(
    [string]$DiskModel = 'MZVLB512HBJQ',   # secondary SSD (Samsung PM981)
    [int]$MinFreeGB = 20,
    [int]$EspMB = 256,
    [string]$Distro = 'Ubuntu-24.04'
)
$ErrorActionPreference = 'Stop'

$ESP_TYPE  = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$ROOT_TYPE = '{4f68bce3-e8cd-4db1-96e7-fbcaf984b709}'   # Linux root x86-64

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }

# --- Checks ----------------------------------------------------------------------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw 'Run this script as Administrator.' }

$disks = @(Get-Disk | Where-Object { $_.Model -match $DiskModel })
if ($disks.Count -ne 1) { throw "Expected 1 disk with model '$DiskModel', found $($disks.Count)." }
$disk = $disks[0]
if ($disk.IsBoot -or $disk.IsSystem) { throw "Disk $($disk.Number) is the Windows disk. Aborting." }
if ($disk.PartitionStyle -ne 'GPT') { throw 'The disk must be GPT.' }

$freeGB = [math]::Round($disk.LargestFreeExtent / 1GB, 1)
if ($freeGB -lt $MinFreeGB) {
    throw "Not enough free space on disk $($disk.Number): $freeGB GB (minimum $MinFreeGB GB). Already installed?"
}

$wslImages = "\\wsl.localhost\$Distro\home\luciano\hexaos-build\images"
foreach ($f in 'efi-part.vfat', 'rootfs.ext2') {
    if (-not (Test-Path "$wslImages\$f")) { throw "$wslImages\$f not found (run scripts/build.sh)." }
}

$repoWsl = (wsl -d $Distro -e wslpath -a ($PSScriptRoot -replace '\\', '/')).Trim()

Write-Host "Disk $($disk.Number): $($disk.FriendlyName), $([math]::Round($disk.Size/1GB)) GB"
Write-Host "Free space to be used: $freeGB GB"
Get-Partition -DiskNumber $disk.Number | Format-Table PartitionNumber, DriveLetter,
    @{n='GB';e={[math]::Round($_.Size/1GB,1)}}, Type -AutoSize | Out-Host
Write-Host 'The partitions above will NOT be changed. Close any open files on D: before continuing.'
if ((Read-Host 'Type YES to install hexaOS') -ne 'YES') { Write-Host 'Cancelled.'; exit 1 }

# --- 1. Partitions ----------------------------------------------------------------
Step 'Creating partitions'
$esp  = New-Partition -DiskNumber $disk.Number -Size ($EspMB * 1MB) -GptType $ESP_TYPE
$root = New-Partition -DiskNumber $disk.Number -UseMaximumSize -GptType $ROOT_TYPE
$espUuid  = $esp.Guid.Trim('{}').ToLower()
$rootUuid = $root.Guid.Trim('{}').ToLower()
Write-Host "EFI:  partition $($esp.PartitionNumber)  ($espUuid)"
Write-Host "Root: partition $($root.PartitionNumber)  ($rootUuid), $([math]::Round($root.Size/1GB,1)) GB"

# --- 2. Writing through WSL --------------------------------------------------------
Step 'Attaching the disk to WSL (D: goes offline while writing)'
$phys = "\\.\PHYSICALDRIVE$($disk.Number)"
wsl --mount $phys --bare
if ($LASTEXITCODE -ne 0) { throw 'wsl --mount failed.' }
try {
    Step 'Writing hexaOS'
    wsl -d $Distro -u root -e bash "$repoWsl/install-ssd.sh" $espUuid $rootUuid
    if ($LASTEXITCODE -ne 0) { throw 'Writing failed (see the messages above).' }
} finally {
    wsl --unmount $phys | Out-Null
    Set-Disk -Number $disk.Number -IsOffline $false -ErrorAction SilentlyContinue
}

# --- 3. Boot menu entry ------------------------------------------------------------
Step 'Adding hexaOS to the firmware boot menu'
$letter = [char[]](83..90 + 71..82) | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
@"
select disk $($disk.Number)
select partition $($esp.PartitionNumber)
assign letter=$letter
"@ | diskpart | Out-Null
try {
    $out = bcdedit /copy '{bootmgr}' /d 'hexaOS'
    $id = [regex]::Match(($out -join ' '), '\{[0-9a-fA-F-]{36}\}').Value
    if (-not $id) { throw "bcdedit /copy failed: $out" }
    bcdedit /set $id device "partition=${letter}:" | Out-Null
    bcdedit /set $id path '\EFI\BOOT\bootx64.efi' | Out-Null
    bcdedit /set '{fwbootmgr}' displayorder $id /addlast | Out-Null
    Write-Host "Entry created: $id"
} finally {
    @"
select disk $($disk.Number)
select partition $($esp.PartitionNumber)
remove letter=$letter
"@ | diskpart | Out-Null
}

Write-Host "`nDone! Reboot, press F10 at power-on and choose 'hexaOS'." -ForegroundColor Green
Write-Host 'Remember: Secure Boot must be disabled (F2 -> Boot).'
