# Installs hexaOS into the UNALLOCATED space of the secondary SSD (dual boot with Windows).
# Run in PowerShell AS ADMINISTRATOR, with the image already built in WSL:
#   powershell -ExecutionPolicy Bypass -File scripts\install-ssd.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\install-ssd.ps1 -DryRun   (checks only)
#
# What it does:
#   1. creates 2 partitions in the free space: EFI (256 MB) and hexaOS root (the rest)
#   2. prepares the boot menu for the new partition (in WSL, nothing written yet)
#   3. writes the boot files and the system into the 2 new partitions, then reads
#      them back to verify. Windows writes them itself: the SSD stays online, so
#      D: and WSL (which may live on D:) keep working
#   4. adds "hexaOS" to the firmware boot menu (F10), at the end of the list:
#      Windows stays the default
# Existing partitions are not touched. The filesystem grows to fill the
# partition on the first boot of hexaOS.
#
# UPDATE: when hexaOS is already installed on the SSD, running the script again
# rewrites those same 2 partitions with the new build (nothing else changes, the
# boot menu entry is kept). Everything stored in hexaOS is replaced: user files,
# saved Wi-Fi networks and settings.
param(
    [string]$DiskModel = 'MZVLB512HBJQ',   # secondary SSD (Samsung PM981)
    [int]$MinFreeGB = 20,
    [int]$EspMB = 256,
    [string]$Distro = 'Ubuntu-24.04',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'

$ESP_TYPE  = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$ROOT_TYPE = '{4f68bce3-e8cd-4db1-96e7-fbcaf984b709}'   # Linux root x86-64

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }

# Raw writing to a partition: at its offset on the physical disk, with the
# partition's volume (if Windows created one) locked and dismounted meanwhile.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class HexaRawWriter {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share,
        IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr inBuf, uint inSize,
        IntPtr outBuf, uint outSize, out uint returned, IntPtr overlapped);

    const uint GENERIC_RW = 0xC0000000, SHARE_RW = 3, OPEN_EXISTING = 3, WRITE_THROUGH = 0x80000000;
    const uint FSCTL_LOCK_VOLUME = 0x90018, FSCTL_DISMOUNT_VOLUME = 0x90020;
    const int CHUNK = 4 * 1024 * 1024;

    static SafeFileHandle Open(string target) {
        SafeFileHandle h = CreateFile(target, GENERIC_RW, SHARE_RW, IntPtr.Zero, OPEN_EXISTING, WRITE_THROUGH, IntPtr.Zero);
        if (h.IsInvalid)
            throw new IOException("Cannot open " + target + " (Windows error " + Marshal.GetLastWin32Error() + ")");
        return h;
    }

    // Locks and dismounts a volume so nothing else uses it. The lock lasts until
    // the returned handle is closed. No data goes through this handle: once a
    // mounted file system (e.g. FAT) is dismounted, I/O through its volume handle
    // fails with "The device is not ready".
    static SafeFileHandle LockVolume(string volume) {
        SafeFileHandle h = Open(volume);
        uint r;
        if (!DeviceIoControl(h, FSCTL_LOCK_VOLUME, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero)) {
            int e = Marshal.GetLastWin32Error(); h.Dispose();
            throw new IOException("Cannot lock " + volume + " (Windows error " + e + "). Close any window or program using it.");
        }
        if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero)) {
            int e = Marshal.GetLastWin32Error(); h.Dispose();
            throw new IOException("Cannot dismount " + volume + " (Windows error " + e + ")");
        }
        return h;
    }

    // Copies source onto the physical disk starting at offset (the partition's start),
    // then reads it back and compares. volume (or null) is the partition's volume,
    // kept locked meanwhile: Windows only allows raw writes to a volume's sectors
    // while that volume is locked.
    public static void WriteAndVerify(string source, int diskNumber, string volume, long offset, long maxLength) {
        using (FileStream src = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read, CHUNK)) {
            long len = src.Length;
            if (len > maxLength) throw new IOException(source + " (" + len + " bytes) does not fit in the partition (" + maxLength + " bytes)");
            if (len % 4096 != 0) throw new IOException(source + " size is not a multiple of 4096 bytes");
            byte[] a = new byte[CHUNK], b = new byte[CHUNK];
            // (PowerShell passes $null as "")
            using (SafeFileHandle lockHandle = String.IsNullOrEmpty(volume) ? null : LockVolume(volume))
            using (SafeFileHandle h = Open("\\\\.\\PHYSICALDRIVE" + diskNumber))
            using (FileStream dst = new FileStream(h, FileAccess.ReadWrite, 4096)) {
                for (int pass = 0; pass < 2; pass++) {
                    string stage = pass == 0 ? "writing" : "verifying";
                    src.Position = 0;
                    dst.Seek(offset, SeekOrigin.Begin);
                    long done = 0;
                    while (done < len) {
                        int n = ReadFull(src, a, (int)Math.Min(CHUNK, len - done));
                        try {
                            if (pass == 0) {
                                dst.Write(a, 0, n);
                            } else {
                                ReadFull(dst, b, n);
                            }
                        } catch (IOException e) {
                            throw new IOException(stage + " at byte " + done + ": " + e.Message, e);
                        }
                        if (pass == 1)
                            for (int i = 0; i < n; i++)
                                if (a[i] != b[i]) throw new IOException("Verification failed at byte " + (done + i));
                        done += n;
                        Console.Write("\r   " + (pass == 0 ? "writing  " : "verifying") + " " + (done * 100 / len) + "%   ");
                    }
                    if (pass == 0) {
                        try { dst.Flush(true); }
                        catch (IOException e) { throw new IOException("flushing to disk: " + e.Message, e); }
                    }
                }
                Console.WriteLine("\r   written and verified: " + (len / 1048576) + " MB      ");
            }
        }
    }

    static int ReadFull(Stream s, byte[] buf, int count) {
        int got = 0;
        while (got < count) {
            int n = s.Read(buf, got, count - got);
            if (n <= 0) throw new IOException("Unexpected end of data");
            got += n;
        }
        return got;
    }
}
'@

function Write-Partition($source, $diskNumber, $partitionNumber) {
    $p = Get-Partition -DiskNumber $diskNumber -PartitionNumber $partitionNumber
    $vol = @($p.AccessPaths | Where-Object { $_ -like '\\?\Volume{*' }) | Select-Object -First 1
    if ($vol) { $vol = $vol.TrimEnd('\') } else { $vol = $null }
    [HexaRawWriter]::WriteAndVerify($source, $diskNumber, $vol, $p.Offset, $p.Size)
}

# Temporary drive letter for the hexaOS EFI partition (removed again afterwards).
function Mount-Esp($diskNumber, $partitionNumber) {
    $letter = [char[]](83..90 + 71..82) | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
    "select disk $diskNumber`nselect partition $partitionNumber`nassign letter=$letter" | diskpart | Out-Null
    if (-not (Test-Path "${letter}:\")) { throw "Could not mount EFI partition $partitionNumber." }
    return $letter
}
function Dismount-Esp($diskNumber, $partitionNumber, $letter) {
    "select disk $diskNumber`nselect partition $partitionNumber`nremove letter=$letter" | diskpart | Out-Null
}

# Existing hexaOS install: a Linux root partition right after an EFI partition
# (the way this script creates them). Returns $null when there is none.
function Find-Install($diskNumber) {
    $parts = @(Get-Partition -DiskNumber $diskNumber)
    $roots = @($parts | Where-Object { $_.GptType -eq $ROOT_TYPE })
    if ($roots.Count -eq 0) { return $null }
    if ($roots.Count -gt 1) { throw "Found $($roots.Count) Linux root partitions on disk $diskNumber, expected at most 1." }
    $r = $roots[0]
    $e = $parts | Where-Object {
        $gap = $r.Offset - ($_.Offset + $_.Size)
        $_.GptType -eq $ESP_TYPE -and $_.PartitionNumber -ne 1 -and $gap -ge 0 -and $gap -le 1MB
    } | Select-Object -First 1
    if (-not $e) { throw "Found a Linux root partition ($($r.PartitionNumber)) but no hexaOS EFI partition right before it." }
    return [pscustomobject]@{ Esp = $e; Root = $r; RootUuid = $r.Guid.Trim('{}').ToLower() }
}

# Identifier of the "hexaOS" firmware boot menu entry, or $null.
# Field names depend on the Windows language: entries are told apart by the
# "-----" lines, and the first {GUID} of an entry is its identifier.
function Get-HexaBootEntry {
    $text = (bcdedit /enum firmware) -join "`n"
    foreach ($block in ($text -split "`n-{5,}\s*`n")) {
        if ($block -match "(?m)^\S+\s+hexaOS\s*$") {
            $m = [regex]::Match($block, '\{[0-9a-fA-F]{8}-[0-9a-fA-F-]{27}\}')
            if ($m.Success) { return $m.Value }
        }
    }
    return $null
}

# --- Checks ----------------------------------------------------------------------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin -and -not $DryRun) { throw 'Run this script as Administrator.' }

$disks = @(Get-Disk | Where-Object { $_.Model -match $DiskModel })
if ($disks.Count -ne 1) { throw "Expected 1 disk with model '$DiskModel', found $($disks.Count)." }
$disk = $disks[0]
if ($disk.IsBoot -or $disk.IsSystem) { throw "Disk $($disk.Number) is the Windows disk. Aborting." }
if ($disk.PartitionStyle -ne 'GPT') { throw 'The disk must be GPT.' }

$wslImages = "\\wsl.localhost\$Distro\home\luciano\hexaos-build\images"
foreach ($f in 'efi-part.vfat', 'rootfs.ext2') {
    if (-not (Test-Path "$wslImages\$f")) { throw "$wslImages\$f not found (run scripts/build.sh)." }
}
$imageBytes = (Get-Item "$wslImages\rootfs.ext2").Length
$repoWsl = (wsl -d $Distro -e wslpath -a ($PSScriptRoot -replace '\\', '/')).Trim()

Write-Host "Disk $($disk.Number): $($disk.FriendlyName), $([math]::Round($disk.Size/1GB)) GB"
Get-Partition -DiskNumber $disk.Number | Format-Table PartitionNumber, DriveLetter,
    @{n='GB';e={[math]::Round($_.Size/1GB,1)}}, Type -AutoSize | Out-Host

$install = Find-Install $disk.Number
$update = [bool]$install
if ($update) {
    $esp = $install.Esp; $root = $install.Root; $rootUuid = $install.RootUuid
    Write-Host "hexaOS is already installed: EFI = partition $($esp.PartitionNumber), system = partition $($root.PartitionNumber) ($rootUuid, $([math]::Round($root.Size/1GB,1)) GB)"
    Write-Host 'UPDATE: these 2 partitions will be rewritten with the new build.' -ForegroundColor Yellow
    if ($imageBytes -gt $root.Size) { throw "The new system image does not fit in partition $($root.PartitionNumber)." }
} else {
    $freeGB = [math]::Round($disk.LargestFreeExtent / 1GB, 1)
    if ($freeGB -lt $MinFreeGB) {
        throw "Not enough free space on disk $($disk.Number): $freeGB GB (minimum $MinFreeGB GB)."
    }
    Write-Host "Free space to be used: $freeGB GB  ->  EFI $EspMB MB + hexaOS $([math]::Round($freeGB - $EspMB/1024, 1)) GB"
}
Write-Host "System image: $wslImages\rootfs.ext2 ($([math]::Round($imageBytes/1GB,1)) GB, built $((Get-Item "$wslImages\rootfs.ext2").LastWriteTime))"

if ($DryRun) {
    Step 'Dry run: testing the boot menu preparation (nothing is written to the SSD)'
    $testUuid = if ($update) { $rootUuid } else { '00000000-0000-4000-8000-000000000000' }
    wsl -d $Distro -e bash "$repoWsl/install-ssd.sh" $testUuid
    if ($LASTEXITCODE -ne 0) { throw 'Boot menu preparation failed.' }
    $what = if ($update) { 'update' } else { 'install' }
    Write-Host "`nAll checks passed. Run again without -DryRun, as Administrator, to $what." -ForegroundColor Green
    exit 0
}

if ($update) {
    Write-Host ''
    Write-Host 'WARNING: everything stored inside hexaOS will be erased: files in /home/hexa,' -ForegroundColor Yellow
    Write-Host 'saved Wi-Fi networks, the password and settings. Copy what you need first' -ForegroundColor Yellow
    Write-Host '(for example to a USB drive or a Windows partition).' -ForegroundColor Yellow
    Write-Host 'The other partitions will NOT be changed.'
    if ((Read-Host 'Type UPDATE to update hexaOS') -ne 'UPDATE') { Write-Host 'Cancelled.'; exit 1 }

    # Make sure the EFI partition really is hexaOS's before overwriting it
    Step 'Checking the hexaOS EFI partition'
    $espOk = $false
    $letter = $null
    try {
        $letter = Mount-Esp $disk.Number $esp.PartitionNumber
        $cfg = "${letter}:\EFI\BOOT\grub.cfg"
        $espOk = (Test-Path $cfg) -and (Select-String -Path $cfg -SimpleMatch "root=PARTUUID=$rootUuid" -Quiet)
    } catch {
        # Unreadable, e.g. after an interrupted update
    } finally {
        if ($letter) { Dismount-Esp $disk.Number $esp.PartitionNumber $letter }
    }
    if ($espOk) {
        Write-Host 'OK'
    } else {
        # It is still the EFI partition right before the hexaOS system partition
        # (Find-Install), but its contents do not confirm it: e.g. an earlier update
        # stopped while writing it.
        Write-Host "Partition $($esp.PartitionNumber) has no readable hexaOS boot menu for $rootUuid" -ForegroundColor Yellow
        Write-Host '(this is expected if an earlier update stopped halfway).'
        if ((Read-Host "Type REWRITE to overwrite partition $($esp.PartitionNumber) anyway") -ne 'REWRITE') { Write-Host 'Cancelled. Nothing was changed.'; exit 1 }
    }

    Step 'Preparing the boot menu'
    wsl -d $Distro -e bash "$repoWsl/install-ssd.sh" $rootUuid
    if ($LASTEXITCODE -ne 0) { throw 'Boot menu preparation failed (see the messages above). Nothing was changed.' }

    try {
        Step 'Writing the boot files (EFI partition)'
        Write-Partition "$wslImages\efi-part-install.vfat" $disk.Number $esp.PartitionNumber
        Step 'Writing the system (takes a few minutes)'
        Write-Partition "$wslImages\rootfs.ext2" $disk.Number $root.PartitionNumber
    } catch {
        Write-Host "`nUpdate failed: $_" -ForegroundColor Red
        Write-Host 'hexaOS may not boot now. Run the script again to retry the update.'
        exit 1
    }
} else {
    Write-Host 'The partitions above will NOT be changed.'
    if ((Read-Host 'Type YES to install hexaOS') -ne 'YES') { Write-Host 'Cancelled.'; exit 1 }

    # --- 1. Partitions --------------------------------------------------------------
    Step 'Creating partitions'
    $esp  = New-Partition -DiskNumber $disk.Number -Size ($EspMB * 1MB) -GptType $ESP_TYPE
    $root = $null
    try {
        $root = New-Partition -DiskNumber $disk.Number -UseMaximumSize -GptType $ROOT_TYPE
        $rootUuid = $root.Guid.Trim('{}').ToLower()
        Write-Host "EFI:  partition $($esp.PartitionNumber)"
        Write-Host "Root: partition $($root.PartitionNumber)  ($rootUuid), $([math]::Round($root.Size/1GB,1)) GB"

        # --- 2. Boot menu -------------------------------------------------------------
        Step 'Preparing the boot menu for the new partition'
        wsl -d $Distro -e bash "$repoWsl/install-ssd.sh" $rootUuid
        if ($LASTEXITCODE -ne 0) { throw 'Boot menu preparation failed (see the messages above).' }

        # --- 3. Writing ---------------------------------------------------------------
        Step 'Writing the boot files (EFI partition)'
        Write-Partition "$wslImages\efi-part-install.vfat" $disk.Number $esp.PartitionNumber
        Step 'Writing the system (takes a few minutes)'
        Write-Partition "$wslImages\rootfs.ext2" $disk.Number $root.PartitionNumber
    } catch {
        Write-Host "`nInstallation failed: $_" -ForegroundColor Red
        Write-Host 'Removing the partitions created by this script (nothing else was changed)...'
        if ($root) { Remove-Partition -DiskNumber $disk.Number -PartitionNumber $root.PartitionNumber -Confirm:$false }
        Remove-Partition -DiskNumber $disk.Number -PartitionNumber $esp.PartitionNumber -Confirm:$false
        exit 1
    }
}

# --- 4. Boot menu entry --------------------------------------------------------------
$entry = Get-HexaBootEntry
if ($entry) {
    Step "Firmware boot menu: keeping the existing 'hexaOS' entry ($entry)"
} else {
    Step 'Adding hexaOS to the firmware boot menu'
    $letter = Mount-Esp $disk.Number $esp.PartitionNumber
    try {
        if (-not (Test-Path "${letter}:\EFI\BOOT\bootx64.efi")) { throw 'The EFI partition was not written correctly.' }
        $out = bcdedit /copy '{bootmgr}' /d 'hexaOS'
        $id = [regex]::Match(($out -join ' '), '\{[0-9a-fA-F-]{36}\}').Value
        if (-not $id) { throw "bcdedit /copy failed: $out" }
        bcdedit /set $id device "partition=${letter}:" | Out-Null
        bcdedit /set $id path '\EFI\BOOT\bootx64.efi' | Out-Null
        bcdedit /set '{fwbootmgr}' displayorder $id /addlast | Out-Null
        Write-Host "Entry created: $id"
    } finally {
        Dismount-Esp $disk.Number $esp.PartitionNumber $letter
    }
}

$done = if ($update) { 'hexaOS updated' } else { 'Done' }
Write-Host "`n$done! Reboot, press F10 at power-on and choose 'hexaOS'." -ForegroundColor Green
Write-Host 'Remember: Secure Boot must be disabled (F2 -> Boot).'
