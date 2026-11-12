param(
    [Parameter(Mandatory=$true)][string]$QtDir,
    [string]$BuildDir = "build/release-windows",
    [switch]$Replace
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $root
function Check-Exit { if ($LASTEXITCODE -ne 0) { throw "Command failed with exit code $LASTEXITCODE" } }
$version = (& python scripts/release.py version).Trim(); Check-Exit
& python scripts/release.py verify-payload out/payload; Check-Exit
& cmake -S Utility-Source -B $BuildDir -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$QtDir" -DCMAKE_AUTOGEN_PARALLEL=1
Check-Exit
& cmake --build $BuildDir --config Release --parallel 2; Check-Exit
& ctest --test-dir $BuildDir -C Release --output-on-failure; Check-Exit
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
if ((Get-Item -LiteralPath $dist).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'dist must not be a link or junction' }
$package = [IO.Path]::GetFullPath((Join-Path $dist "Glassboot-$version-windows-x86_64"))
# Before recursive removal, resolve the exact destination and constrain it to dist.
if ([IO.Path]::GetDirectoryName($package) -ne [IO.Path]::GetFullPath($dist)) { throw 'Invalid package directory' }
if (Test-Path -LiteralPath $package) {
    if ((Get-Item -LiteralPath $package).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Package directory must not be a link or junction' }
    if (!$Replace) { throw 'Package directory already exists; use -Replace to rebuild it' }
    Remove-Item -LiteralPath $package -Recurse -Force
}
New-Item -ItemType Directory -Path $package | Out-Null
Copy-Item -LiteralPath (Join-Path $BuildDir 'Release/glassboot-installer.exe'),(Join-Path $BuildDir 'Release/glassboot-cli.exe') -Destination $package
& (Join-Path $QtDir 'bin/windeployqt.exe') --release --no-compiler-runtime --dir $package (Join-Path $package 'glassboot-installer.exe')
Check-Exit
# App-local VC runtime, so the portable program needs no VC redistributable installer.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if (!$vs) { throw 'MSVC installation not found' }
$redist = Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Redist/MSVC') -Directory |
    Where-Object Name -Match '^\d' | Sort-Object Name -Descending | Select-Object -First 1
$crt = Join-Path $redist.FullName 'x64/Microsoft.VC143.CRT'
Get-ChildItem -LiteralPath $crt -Filter '*.dll' | Copy-Item -Destination $package
Copy-Item -LiteralPath out/payload -Destination (Join-Path $package 'payload') -Recurse
Copy-Item -LiteralPath packaging/NOTICES.txt,versions.env -Destination $package
Copy-Item -LiteralPath packaging/PORTABLE-README.txt -Destination (Join-Path $package 'README.txt')
Copy-Item -LiteralPath packaging/licenses -Destination (Join-Path $package 'LICENSES') -Recurse
Copy-Item -LiteralPath packaging/versions.env -Destination (Join-Path $package 'packaging-versions.env')
& (Join-Path $QtDir 'bin/qmake.exe') -query QT_VERSION | Set-Content -LiteralPath (Join-Path $package 'qt-version.txt') -Encoding ascii
Check-Exit
@'
[Paths]
Prefix=.
Plugins=.
Translations=translations
'@ | Set-Content -LiteralPath (Join-Path $package 'qt.conf') -Encoding ascii
& python scripts/release.py inventory $package windows-x86_64; Check-Exit
& python scripts/release.py zip $package "$package.zip"; Check-Exit
Copy-Item -LiteralPath (Join-Path $package 'SBOM.cdx.json') -Destination "$package.zip.sbom.json"
Copy-Item -LiteralPath (Join-Path $package 'release.json') -Destination "$package.zip.provenance.json"
& python scripts/release.py hashes $dist; Check-Exit
Write-Host "Built $package.zip (also available as an unpacked portable directory)"
