# SPDX-License-Identifier: BSD-2-Clause-Patent
#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InputPath,
    [Parameter(Mandatory)][string]$OutputPath,
    [string]$Inf2Cat = 'Inf2Cat.exe',
    [string]$SignTool = 'SignTool.exe',
    [ValidateSet('10_GE_ARM64', '10_25H2_ARM64')][string]$WindowsVersion = '10_GE_ARM64'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-WdkTool([string]$Tool, [string[]]$Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "WDK tool failed with exit code ${LASTEXITCODE}: $Tool"
    }
}

function Test-EmbeddedSigner([string]$Path, [string]$Thumbprint) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if (!$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $Thumbprint -or
        $signature.Status -eq 'HashMismatch') {
        throw "The expected test signature is missing or invalid: $Path"
    }
}

$inputRoot = Get-Item -LiteralPath $InputPath
if (!$inputRoot.PSIsContainer) { throw 'InputPath must be a package directory.' }
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
if (Test-Path -LiteralPath $output) { throw 'Choose a new OutputPath.' }
$parent = Split-Path -Parent $output
if (!(Test-Path -LiteralPath $parent -PathType Container)) { throw 'OutputPath parent must exist.' }

$inf2CatExe = (Get-Command -Name $Inf2Cat -CommandType Application).Source
$signExe = (Get-Command -Name $SignTool -CommandType Application).Source
$packages = @(Get-ChildItem -LiteralPath $inputRoot.FullName -Directory |
    Where-Object { @(Get-ChildItem -LiteralPath $_.FullName -File -Filter '*.inf').Count -gt 0 })
if ($packages.Count -eq 0) { throw 'InputPath must contain a subdirectory per driver package.' }

$staged = Join-Path $parent ('.t70-test-sign-' + [Guid]::NewGuid().ToString('N'))
$cert = $null
New-Item -Path $staged -ItemType Directory | Out-Null
try {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=T70 Pro Local Test Signing' `
        -CertStoreLocation 'Cert:\CurrentUser\My' -KeyAlgorithm RSA -KeyLength 3072 `
        -KeyExportPolicy NonExportable -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(1)
    Export-Certificate -Cert $cert -FilePath (Join-Path $staged 'T70Pro-Test.cer') | Out-Null

    foreach ($package in $packages) {
        if ($package.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "A package directory is a reparse point: $($package.FullName)"
        }
        $target = Join-Path $staged $package.Name
        New-Item -Path $target -ItemType Directory | Out-Null
        $files = @(Get-ChildItem -LiteralPath $package.FullName -File |
            Where-Object { $_.Extension -ne '.cat' })
        foreach ($file in $files) {
            if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "A package file is a reparse point: $($file.FullName)"
            }
            Copy-Item -LiteralPath $file.FullName -Destination $target
        }
        $images = @(Get-ChildItem -LiteralPath $target -File |
            Where-Object { $_.Extension -in '.sys', '.dll' })
        if ($images.Count -eq 0) { throw "No driver images found in $($package.Name)." }
        foreach ($image in $images) {
            Invoke-WdkTool $signExe @('sign', '/fd', 'SHA256', '/sha1', $cert.Thumbprint, $image.FullName)
            Test-EmbeddedSigner $image.FullName $cert.Thumbprint
        }

        Invoke-WdkTool $inf2CatExe @("/driver:$target", "/os:$WindowsVersion", '/uselocaltime')
        $catalogs = @(Get-ChildItem -LiteralPath $target -File -Filter '*.cat')
        if ($catalogs.Count -eq 0) { throw "Inf2Cat produced no catalog for $($package.Name)." }
        foreach ($catalog in $catalogs) {
            Invoke-WdkTool $signExe @('sign', '/fd', 'SHA256', '/sha1', $cert.Thumbprint, $catalog.FullName)
            Test-EmbeddedSigner $catalog.FullName $cert.Thumbprint
        }
    }
    [IO.Directory]::Move($staged, $output)
    Write-Output $output
}
finally {
    if ($cert) { Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey }
    if (Test-Path -LiteralPath $staged) { Remove-Item -LiteralPath $staged -Recurse -Force }
}
