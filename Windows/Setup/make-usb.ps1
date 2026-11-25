# SPDX-License-Identifier: BSD-2-Clause-Patent
<#
Make a T70 Pro Windows install stick from the official Windows 11 ARM64 ISO,
on Windows. Start make-usb.cmd from an administrator terminal:

    make-usb.cmd -Iso C:\Users\me\Downloads\Win11_arm64.iso -Disk 3

Without -Iso or -Disk it asks. The whole stick is erased. The ISO is used
as it is; the only changes, all on the stick, are the ones make-usb.py
makes on macOS and Linux:

  - one FAT32 partition (at most 32 GB, the Windows FAT32 limit) with the
    ISO's files;
  - sources\install.wim split into install*.swm (FAT32 files stop at 4 GiB);
  - payload\ added to sources\boot.wim (Setup image) as \T70, with
    winpeshl.ini so that WinPE runs \T70\StartT70Setup.cmd;
  - test signing switched on for the Setup entry of efi\microsoft\boot\bcd.

Everything written is read back and compared at the end.
#>
param(
    [string]$Iso,
    [int]$Disk = -1,
    [switch]$Yes
)
$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Label = 'T70WIN'
$Fat32Max = 4GB - 1

function Die([string]$Message) {
    Write-Host "make-usb: $Message" -ForegroundColor Red
    exit 1
}

function Hash([string]$Path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash }

$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Die 'run make-usb.cmd from an administrator terminal'
}

# The kit must be complete and unchanged.
$payload = Join-Path $Here 'payload'
$sums = Join-Path $Here 'SHA256SUMS.txt'
if (-not (Test-Path $payload) -or -not (Test-Path $sums)) { Die 'payload\ or SHA256SUMS.txt missing next to this script' }
$bad = @()
foreach ($line in Get-Content $sums) {
    if (-not $line.Trim()) { continue }
    $digest, $name = $line -split '\s+', 2
    $p = Join-Path $Here ($name.TrimStart('*') -replace '/', '\')
    if (-not (Test-Path -LiteralPath $p) -or (Hash $p) -ne $digest.ToUpper()) { $bad += $name }
}
if ($bad) { Die ("kit files differ from SHA256SUMS.txt: " + ($bad[0..4] -join ', ')) }
foreach ($need in 'StartT70Setup.cmd', 'winpeshl.ini', 'trust-pe.reg', 'trust-os.reg', 'Drivers\order.txt') {
    if (-not (Test-Path (Join-Path $payload $need))) { Die "payload\$need missing" }
}

if (-not $Iso) { $Iso = (Read-Host 'Path of the Windows 11 ARM64 ISO').Trim('"') }
$Iso = (Resolve-Path -LiteralPath $Iso).Path
if ($Disk -lt 0) {
    Get-Disk | Where-Object { $_.BusType -in 'USB', 'SD', 'MMC' } |
        Format-Table Number, FriendlyName, @{n = 'Size (GB)'; e = { [math]::Round($_.Size / 1GB, 1) } }, BusType -AutoSize | Out-Host
    $Disk = [int](Read-Host 'Number of the USB disk to erase')
}
$d = Get-Disk -Number $Disk
if ($d.IsBoot -or $d.IsSystem) { Die "disk $Disk holds the running system; refusing" }
if ($d.BusType -notin 'USB', 'SD', 'MMC') { Die "disk $Disk is on $($d.BusType), not USB; refusing to erase it" }

$work = Join-Path $env:TEMP ("t70-usb-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work | Out-Null
$image = Mount-DiskImage -ImagePath $Iso -PassThru
try {
    $src = (($image | Get-Volume).DriveLetter + ':\')
    foreach ($rel in 'sources\setup.exe', 'sources\boot.wim', 'efi\microsoft\boot\bcd', 'efi\boot\bootaa64.efi') {
        if (-not (Test-Path (Join-Path $src $rel))) { Die "the ISO has no $rel; is it the Windows 11 ARM64 ISO?" }
    }
    $install = @('sources\install.wim', 'sources\install.esd') | ForEach-Object { Join-Path $src $_ } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $install) { Die 'the ISO has no sources\install.wim or install.esd' }
    $setup = Get-WindowsImage -ImagePath (Join-Path $src 'sources\boot.wim') -Index 2
    if ("$($setup.Architecture)" -notmatch '^(12|arm64)$') { Die "boot.wim image 2 is $($setup.Architecture), not ARM64; use the Windows 11 ARM64 ISO" }
    if ($setup.ImageName -notmatch 'Setup') { Die 'boot.wim image 2 is not Windows Setup' }
    $installSize = (Get-Item $install).Length
    if ($install.EndsWith('.esd') -and $installSize -gt $Fat32Max) { Die 'install.esd is larger than 4 GiB; only install.wim is split' }
    $need = (Get-ChildItem -LiteralPath $src -Recurse -File | Measure-Object Length -Sum).Sum + 64MB
    $size = [math]::Min($d.Size - 8MB, 32GB - 8MB)
    if ($size -lt $need) { Die "disk $Disk is too small: $([math]::Round($need / 1GB, 1)) GB needed" }

    Write-Host "ISO: $(Split-Path -Leaf $Iso) (Setup $($setup.Version), $($setup.Languages -join ','))"
    Write-Host "Disk ${Disk}: $($d.FriendlyName), $([math]::Round($d.Size / 1GB, 1)) GB. EVERYTHING ON IT WILL BE ERASED." -ForegroundColor Yellow
    if (-not $Yes -and (Read-Host "Type $Disk to continue") -ne "$Disk") { Die 'cancelled; nothing written' }

    Write-Host 'Making the Setup image ...'
    $wim = Join-Path $work 'boot.wim'
    Copy-Item -LiteralPath (Join-Path $src 'sources\boot.wim') -Destination $wim
    Set-ItemProperty -LiteralPath $wim -Name IsReadOnly -Value $false
    $mount = Join-Path $work 'mount'
    New-Item -ItemType Directory -Path $mount | Out-Null
    Mount-WindowsImage -ImagePath $wim -Index 2 -Path $mount | Out-Null
    $saved = $false
    try {
        if (Test-Path (Join-Path $mount 'Windows\System32\winpeshl.ini')) { Die 'boot.wim already has a winpeshl.ini; use the unmodified official ISO' }
        if (Test-Path (Join-Path $mount 'T70')) { Die 'boot.wim already has \T70; use the unmodified official ISO' }
        Copy-Item -LiteralPath $payload -Destination (Join-Path $mount 'T70') -Recurse
        Copy-Item -LiteralPath (Join-Path $payload 'winpeshl.ini') -Destination (Join-Path $mount 'Windows\System32\winpeshl.ini')
        Dismount-WindowsImage -Path $mount -Save | Out-Null
        $saved = $true
    } finally {
        if (-not $saved) { Dismount-WindowsImage -Path $mount -Discard | Out-Null }
    }

    Write-Host 'Erasing and formatting the stick ...'
    # diskpart, because a blank removable stick shows as one whole-disk
    # pseudo partition that Clear-Disk/New-Partition cannot replace.
    $script = Join-Path $work 'diskpart.txt'
    @("select disk $Disk", 'clean', "create partition primary size=$([math]::Floor($size / 1MB))",
      "format fs=fat32 quick label=$Label", 'active', 'assign') | Set-Content -Encoding ASCII -LiteralPath $script
    diskpart.exe /s $script | Out-Host
    if ($LASTEXITCODE) { Die "diskpart could not prepare disk $Disk ($LASTEXITCODE)" }
    Update-Disk -Number $Disk
    $parts = @(Get-Partition -DiskNumber $Disk | Where-Object { $_.DriveLetter })
    if ($parts.Count -ne 1) { Die "expected one lettered partition on disk $Disk after formatting, found $($parts.Count)" }
    $vol = $parts[0] | Get-Volume
    if ($vol.FileSystem -ne 'FAT32' -or $vol.FileSystemLabel -ne $Label) { Die "disk $Disk was not formatted FAT32 $Label" }
    $dst = "$($parts[0].DriveLetter):\"

    Write-Host 'Copying the ISO files ...'
    robocopy $src $dst /E /A-:R /XF install.wim install.esd boot.wim /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { Die "robocopy failed ($LASTEXITCODE)" }
    Copy-Item -LiteralPath $wim -Destination (Join-Path $dst 'sources\boot.wim')
    if ($install.EndsWith('.wim') -and $installSize -gt $Fat32Max) {
        Write-Host "Splitting install.wim ($([math]::Round($installSize / 1MB)) MiB) ..."
        # DISM cannot split straight from the read-only mounted ISO (access denied).
        $free = (Get-Item -LiteralPath $work).PSDrive.Free
        if ($free -lt $installSize + 1GB) { Die "not enough free space next to $work to copy install.wim" }
        $local = Join-Path $work 'install.wim'
        Copy-Item -LiteralPath $install -Destination $local
        Set-ItemProperty -LiteralPath $local -Name IsReadOnly -Value $false
        Split-WindowsImage -ImagePath $local -SplitImagePath (Join-Path $dst 'sources\install.swm') -FileSize 3800 | Out-Null
        Remove-Item -LiteralPath $local -Force
    } else {
        Copy-Item -LiteralPath $install -Destination (Join-Path $dst ('sources\' + (Split-Path -Leaf $install)))
    }
    $bcd = Join-Path $dst 'efi\microsoft\boot\bcd'
    bcdedit.exe /store $bcd /set '{default}' testsigning on | Out-Null
    if ($LASTEXITCODE) { Die 'bcdedit could not switch on test signing' }
    # bcdedit leaves the hive's transaction logs beside it; the ISO has none.
    Get-ChildItem -LiteralPath (Split-Path -Parent $bcd) -Force |
        Where-Object { $_.Name -match '^bcd(\.log\d*|\{.*\}\.tm.*)$' } | Remove-Item -Force

    Write-Host 'Reading the stick back ...'
    $mismatch = @()
    foreach ($f in Get-ChildItem -LiteralPath $src -Recurse -File) {
        $rel = $f.FullName.Substring($src.Length)
        if ($rel -in 'sources\install.wim', 'sources\install.esd', 'sources\boot.wim', 'efi\microsoft\boot\bcd') { continue }
        if ((Hash $f.FullName) -ne (Hash (Join-Path $dst $rel))) { $mismatch += $rel }
    }
    if ($mismatch) { Die ('files on the stick differ from the ISO: ' + ($mismatch[0..4] -join ', ')) }
    if ((Hash $wim) -ne (Hash (Join-Path $dst 'sources\boot.wim'))) { Die 'boot.wim on the stick differs from the one made' }
    $parts = Get-ChildItem -LiteralPath (Join-Path $dst 'sources') -Filter 'install*' | ForEach-Object Name
    if ($install.EndsWith('.wim') -and $installSize -gt $Fat32Max) {
        if ($parts -contains 'install.wim') { Die 'install.wim was copied unsplit' }
        Get-WindowsImage -ImagePath (Join-Path $dst 'sources\install.swm') | Out-Null
    }
    $bcdCheck = Join-Path $work 'bcd-check'
    Copy-Item -LiteralPath $bcd -Destination $bcdCheck
    reg.exe load HKLM\T70BCD $bcdCheck | Out-Null
    if ($LASTEXITCODE) { Die 'could not open the stick BCD' }
    try {
        $default = (Get-ItemProperty 'HKLM:\T70BCD\Objects\{9dea862c-5cdd-4e70-acc1-f32b344d4795}\Elements\23000003').Element
        $ts = (Get-ItemProperty "HKLM:\T70BCD\Objects\$default\Elements\16000049" -ErrorAction SilentlyContinue).Element
    } finally {
        [gc]::Collect(); [gc]::WaitForPendingFinalizers()
        reg.exe unload HKLM\T70BCD | Out-Null
    }
    if (-not $ts -or $ts[0] -ne 1) { Die 'test signing is not on in the stick BCD' }
    # Nothing on the stick that the ISO does not have (Windows' own System Volume Information aside).
    $expected = @{}
    foreach ($f in Get-ChildItem -LiteralPath $src -Recurse -File) { $expected[$f.FullName.Substring($src.Length).ToLower()] = 1 }
    $extra = @(Get-ChildItem -LiteralPath $dst -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Substring($dst.Length) } |
        Where-Object { -not $expected.ContainsKey($_.ToLower()) -and $_ -notmatch '^sources\\install\d*\.swm$' -and $_ -notmatch '^System Volume Information\\' })
    if ($extra) { Die ('the stick has files the ISO does not: ' + ($extra[0..7] -join ', ')) }
    Write-Host ''
    Write-Host "Done. Install image as $($parts -join ', '), Setup image with the T70 drivers, test signing on." -ForegroundColor Green
    Write-Host 'Plug the stick into the T70 Pro and start it from the stick.'
} finally {
    Dismount-DiskImage -ImagePath $Iso | Out-Null
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
