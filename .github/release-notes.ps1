# Prints the release notes for one version: its section of CHANGELOG.md, followed by
# a link to the full list of changes. The release workflow publishes this as the
# release's notes, and the same command previews them locally:
#
#   pwsh .github/release-notes.ps1 -Tag 1.04
#
# Fails when CHANGELOG.md has no section for the tag, or an empty one, so a release
# is never published without notes.
param(
    [Parameter(Mandatory)] [string] $Tag,
    [string] $Repo      = "altsimdev/winswapper",
    [string] $Changelog = (Join-Path $PSScriptRoot "..\CHANGELOG.md")
)

$ErrorActionPreference = "Stop"

# @() keeps a one-line file an array; Get-Content would otherwise return a bare
# string, and indexing that yields characters. Line endings are stripped here, so
# a CRLF checkout on the build machine makes no difference.
$lines = @(Get-Content -LiteralPath $Changelog -Encoding utf8)

# Version headings look like "## 1.04 - 2026-10-04"; "## [1.04]" is accepted too.
$headings = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^##\s+\[?([^\s\]]+)') {
        $headings += [pscustomobject]@{ Line = $i; Version = $Matches[1] }
    }
}

$index = -1
for ($h = 0; $h -lt $headings.Count; $h++) {
    if ($headings[$h].Version -eq $Tag) { $index = $h; break }
}
if ($index -lt 0) {
    throw "CHANGELOG.md has no '## $Tag' section. Add one, then tag the commit that has it."
}

$from = $headings[$index].Line + 1
$to   = if ($index + 1 -lt $headings.Count) { $headings[$index + 1].Line - 1 } else { $lines.Count - 1 }

# Guarded, because PowerShell ranges count down: 5..4 is 5,4, which would pull in
# the neighbouring headings instead of yielding nothing.
$body = if ($to -ge $from) { ($lines[$from..$to] -join "`n").Trim() } else { "" }
if (-not $body) {
    throw "The '## $Tag' section of CHANGELOG.md is empty."
}

# Sections run newest first, so the next heading down is the previous release.
$link = if ($index + 1 -lt $headings.Count) {
    "https://github.com/$Repo/compare/$($headings[$index + 1].Version)...$Tag"
} else {
    "https://github.com/$Repo/commits/$Tag"
}

"$body`n`n**Full Changelog**: $link`n"
