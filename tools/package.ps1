param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    if (-not $SkipBuild) {
        & xmake f -a x64 -m release -p windows -y
        if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
        & xmake -y
        if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    }
    $manifest = Get-Content -LiteralPath 'dist/ModMenu/manifest.json' -Raw | ConvertFrom-Json
    $versionMatch = [regex]::Match((Get-Content -LiteralPath 'xmake.lua' -Raw), 'local menu_version = "([^"]+)"')
    if (-not $versionMatch.Success -or $manifest.version -ne $versionMatch.Groups[1].Value) { throw 'Build version mismatch' }
    if ($manifest.name -ne 'ModMenu' -or $manifest.entry -ne 'ModMenu.dll') { throw 'Unexpected manifest identity' }
    foreach ($name in @('ModMenu.dll', 'manifest.json', 'assets/icon.png', 'README.md', 'README.ja-JP.md', 'README.en-US.md', 'LICENSE', 'THIRD_PARTY_NOTICES.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path 'dist/ModMenu' $name))) { throw "Missing file: $name" }
    }
    New-Item -ItemType Directory -Path 'artifacts' -Force | Out-Null
    $zip = Join-Path 'artifacts' ("ModMenu-" + $manifest.version + '-windows-x64.zip')
    Compress-Archive -LiteralPath 'dist/ModMenu' -DestinationPath $zip -Force
    $checksum = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + (Split-Path -Leaf $zip)
    Set-Content -LiteralPath 'artifacts/SHA256SUMS.txt' -Value $checksum -Encoding ascii
    Write-Output $zip
}
finally { Pop-Location }
