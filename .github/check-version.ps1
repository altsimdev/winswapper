# Checks that a built winswapper.exe reports the same version as the tag being
# released, everywhere a version shows: the file and product version strings, the
# numeric version Explorer shows in the file's Properties, the About box, and the
# embedded manifest. Lists every mismatch, then fails. The release workflow runs it
# before publishing anything, and the same command checks a build locally:
#
#   pwsh .github/check-version.ps1 -Exe build\x64\Release\winswapper.exe -Tag 1.05
#
# Release tags are <major>.<minor>, such as 1.05, whose numeric form is 1.0.5.0.
param(
    [Parameter(Mandatory)] [string] $Exe,
    [Parameter(Mandatory)] [string] $Tag
)

$ErrorActionPreference = "Stop"

if ($Tag -notmatch '^(\d+)\.(\d+)$') {
    throw "Tag '$Tag' is not in the <major>.<minor> form releases use, such as 1.05."
}
$quad = "{0}.0.{1}.0" -f [int]$Matches[1], [int]$Matches[2]

$path  = (Resolve-Path -LiteralPath $Exe).Path
$info  = (Get-Item -LiteralPath $path).VersionInfo
$bytes = [IO.File]::ReadAllBytes($path)

# The About text is UTF-16 at whatever offset the linker placed it, so decode the
# file at both byte alignments rather than assume an even one.
$utf16 = [Text.Encoding]::Unicode.GetString($bytes) +
         [Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1)
$ascii = [Text.Encoding]::ASCII.GetString($bytes)

$problems = @()
if ($info.FileVersion -ne $Tag) {
    $problems += "the FileVersion string is '$($info.FileVersion)' (res/winswapper.rc)"
}
if ($info.ProductVersion -ne $Tag) {
    $problems += "the ProductVersion string is '$($info.ProductVersion)' (res/winswapper.rc)"
}
$numeric = "{0}.{1}.{2}.{3}" -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart
if ($numeric -ne $quad) {
    $problems += "the numeric FILEVERSION is $numeric, not $quad (res/winswapper.rc)"
}
if (-not $utf16.Contains("WinSwapper $Tag`n")) {
    $problems += "the About box does not say 'WinSwapper $Tag' (src/main.cpp)"
}
if (-not $ascii.Contains("name=""WinSwapper"" version=""$quad""")) {
    $problems += "the embedded manifest's version is not $quad (res/app.manifest)"
}

if ($problems) {
    throw ("winswapper.exe does not match tag $Tag`:`n  - " + ($problems -join "`n  - "))
}
Write-Host "winswapper.exe is version $Tag everywhere it shows one ($quad numerically)."
