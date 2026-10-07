# Checks that winswapper.exe needs nothing beyond Windows itself: it lists the DLLs
# the exe imports and refuses any that do not ship with Windows 10 and 11.
#
# Releases up to 1.06 needed VCRUNTIME140.dll, VCRUNTIME140_1.dll and MSVCP140.dll
# from the Visual C++ Redistributable and would not start on a clean install. CI
# never noticed, because its build machines have the redistributable installed -
# running the exe there proves nothing, so this reads its imports instead. The
# build workflow runs it on every build, and the same command checks one locally:
#
#   pwsh .github/check-dependencies.ps1 -Exe build\x64\Release\winswapper.exe
#
# dumpbin comes from the newest Visual Studio with the C++ tools, found by vswhere.
param(
    [Parameter(Mandatory)] [string] $Exe
)

$ErrorActionPreference = "Stop"

# DLLs that are part of Windows. Anything named api-ms-win-* is also fine: those are
# API sets the loader maps onto system DLLs - api-ms-win-crt-* onto the Universal C
# Runtime, which is part of Windows 10 and later. A Windows DLL newly linked into
# the project has to be added here.
$windows = @('kernel32.dll', 'user32.dll', 'gdi32.dll', 'shell32.dll', 'advapi32.dll',
             'dwmapi.dll', 'comctl32.dll', 'ole32.dll', 'oleaut32.dll', 'shlwapi.dll')

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe is not at $vswhere, so dumpbin cannot be found." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dumpbin = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe') -ErrorAction SilentlyContinue |
           Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $dumpbin) { throw "dumpbin.exe was not found under '$vs', so the dependencies cannot be checked." }

$out = & $dumpbin /nologo /dependents (Resolve-Path -LiteralPath $Exe).Path
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $Exe." }
$dlls = @($out | ForEach-Object { if ($_ -match '^\s+(\S+\.dll)\s*$') { $Matches[1] } })
if ($dlls.Count -eq 0) { throw "dumpbin listed no DLLs for $Exe, which cannot be right." }

$foreign = @($dlls | Where-Object { $_ -notlike 'api-ms-win-*' -and $windows -notcontains $_.ToLower() })
if ($foreign.Count -gt 0) {
    throw ("winswapper.exe needs DLLs that do not ship with Windows, so it would not start on a " +
           "clean install:`n  - " + ($foreign -join "`n  - ") +
           "`nVCRUNTIME* and MSVCP* mean the C++ runtime is linked as a DLL again; see RuntimeLibrary " +
           "in winswapper.vcxproj.")
}
Write-Host ("winswapper.exe needs only Windows' own DLLs: " + ($dlls -join ', '))
