# Instala o hexaOS no espaco NAO ALOCADO do SSD secundario (dual boot com o Windows).
# Rodar no PowerShell COMO ADMINISTRADOR, com a imagem ja compilada no WSL:
#   powershell -ExecutionPolicy Bypass -File scripts\install-ssd.ps1
#
# O que faz:
#   1. cria 2 particoes no espaco livre: EFI (256 MB) e raiz do hexaOS (o resto)
#   2. anexa o disco ao WSL e grava o sistema (scripts/install-ssd.sh)
#   3. adiciona "hexaOS" ao menu de boot do firmware (F10), no fim da lista:
#      o Windows continua sendo o padrao
# Nao mexe nas particoes existentes. Durante a gravacao o D: fica indisponivel.
param(
    [string]$DiskModel = 'MZVLB512HBJQ',   # SSD secundario (Samsung PM981)
    [int]$MinFreeGB = 20,
    [int]$EspMB = 256,
    [string]$Distro = 'Ubuntu-24.04'
)
$ErrorActionPreference = 'Stop'

$ESP_TYPE  = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$ROOT_TYPE = '{4f68bce3-e8cd-4db1-96e7-fbcaf984b709}'   # Linux root x86-64

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }

# --- Verificacoes ---------------------------------------------------------------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw 'Rode este script como Administrador.' }

$disks = @(Get-Disk | Where-Object { $_.Model -match $DiskModel })
if ($disks.Count -ne 1) { throw "Esperava 1 disco com modelo '$DiskModel', achei $($disks.Count)." }
$disk = $disks[0]
if ($disk.IsBoot -or $disk.IsSystem) { throw "O disco $($disk.Number) e o disco do Windows. Abortando." }
if ($disk.PartitionStyle -ne 'GPT') { throw 'O disco precisa ser GPT.' }

$freeGB = [math]::Round($disk.LargestFreeExtent / 1GB, 1)
if ($freeGB -lt $MinFreeGB) {
    throw "Pouco espaco livre no disco $($disk.Number): $freeGB GB (minimo $MinFreeGB GB). Ja instalado?"
}

$wslImages = "\\wsl.localhost\$Distro\home\luciano\hexaos-build\images"
foreach ($f in 'efi-part.vfat', 'rootfs.ext2') {
    if (-not (Test-Path "$wslImages\$f")) { throw "Nao achei $wslImages\$f (rode scripts/build.sh)." }
}

$repoWsl = (wsl -d $Distro -e wslpath -a ($PSScriptRoot -replace '\\', '/')).Trim()

Write-Host "Disco $($disk.Number): $($disk.FriendlyName), $([math]::Round($disk.Size/1GB)) GB"
Write-Host "Espaco livre que sera usado: $freeGB GB"
Get-Partition -DiskNumber $disk.Number | Format-Table PartitionNumber, DriveLetter,
    @{n='GB';e={[math]::Round($_.Size/1GB,1)}}, Type -AutoSize | Out-Host
Write-Host 'As particoes acima NAO serao alteradas. Feche arquivos abertos no D: antes de continuar.'
if ((Read-Host 'Digite SIM para instalar o hexaOS') -ne 'SIM') { Write-Host 'Cancelado.'; exit 1 }

# --- 1. Particoes -----------------------------------------------------------------
Step 'Criando particoes'
$esp  = New-Partition -DiskNumber $disk.Number -Size ($EspMB * 1MB) -GptType $ESP_TYPE
$root = New-Partition -DiskNumber $disk.Number -UseMaximumSize -GptType $ROOT_TYPE
$espUuid  = $esp.Guid.Trim('{}').ToLower()
$rootUuid = $root.Guid.Trim('{}').ToLower()
Write-Host "EFI:  particao $($esp.PartitionNumber)  ($espUuid)"
Write-Host "Raiz: particao $($root.PartitionNumber)  ($rootUuid), $([math]::Round($root.Size/1GB,1)) GB"

# --- 2. Gravacao pelo WSL ---------------------------------------------------------
Step 'Anexando o disco ao WSL (o D: fica offline durante a gravacao)'
$phys = "\\.\PHYSICALDRIVE$($disk.Number)"
wsl --mount $phys --bare
if ($LASTEXITCODE -ne 0) { throw 'wsl --mount falhou.' }
try {
    Step 'Gravando o hexaOS'
    wsl -d $Distro -u root -e bash "$repoWsl/install-ssd.sh" $espUuid $rootUuid
    if ($LASTEXITCODE -ne 0) { throw 'A gravacao falhou (veja as mensagens acima).' }
} finally {
    wsl --unmount $phys | Out-Null
    Set-Disk -Number $disk.Number -IsOffline $false -ErrorAction SilentlyContinue
}

# --- 3. Entrada no menu de boot ---------------------------------------------------
Step 'Adicionando hexaOS ao menu de boot do firmware'
$letter = [char[]](83..90 + 71..82) | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
@"
select disk $($disk.Number)
select partition $($esp.PartitionNumber)
assign letter=$letter
"@ | diskpart | Out-Null
try {
    $out = bcdedit /copy '{bootmgr}' /d 'hexaOS'
    $id = [regex]::Match(($out -join ' '), '\{[0-9a-fA-F-]{36}\}').Value
    if (-not $id) { throw "bcdedit /copy falhou: $out" }
    bcdedit /set $id device "partition=${letter}:" | Out-Null
    bcdedit /set $id path '\EFI\BOOT\bootx64.efi' | Out-Null
    bcdedit /set '{fwbootmgr}' displayorder $id /addlast | Out-Null
    Write-Host "Entrada criada: $id"
} finally {
    @"
select disk $($disk.Number)
select partition $($esp.PartitionNumber)
remove letter=$letter
"@ | diskpart | Out-Null
}

Write-Host "`nPronto! Reinicie, aperte F10 ao ligar e escolha 'hexaOS'." -ForegroundColor Green
Write-Host 'Lembre: o Secure Boot precisa estar desativado (F2 -> Boot).'
