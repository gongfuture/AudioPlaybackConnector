<#
.SYNOPSIS
    Re-extract the translatable strings and merge them into every .po catalogue.

.DESCRIPTION
    Replaces the old gen_pot.sh. Two steps:

      1. xgettext scans the sources for _() and C_() and writes source/messages.pot,
         the template listing every translatable string with correct file:line
         references.
      2. msgmerge folds that template into each source/*.po, keeping existing
         translations, adding new strings as untranslated, and marking strings that
         disappeared as obsolete (#~). Reworded strings are carried over and flagged
         "fuzzy" so a translator confirms them rather than silently losing the text.

    Run this after adding, removing, or rewording any _() string. It does NOT build the
    .ymo resources -- build_translations.py does that, and the build runs it for you.

.NOTES
    REQUIRES GNU gettext (xgettext + msgmerge). It is not needed to build the project,
    only to update the catalogues. Install with either:

        choco install gettext
        winget install --id mlocati.GettextIconv

    A fresh install will not be on PATH in already-open terminals, so this script also
    looks in the usual install directories. Restart the shell if you would rather have
    the tools on PATH.

    The #: line references this regenerates are informational only -- they help a
    translator find the string in the source. Stale ones break nothing at runtime.

.EXAMPLE
    pwsh translate\update_catalogs.ps1
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$copyrightHolder = 'Richard Yu <yurichard3839@gmail.com>'
$packageName = 'AudioPlaybackConnector'

$translateDir = $PSScriptRoot
$sourceDir = Join-Path $translateDir 'source'
$repoRoot = Split-Path $translateDir -Parent
$potPath = Join-Path $sourceDir 'messages.pot'

# Where a Windows gettext install typically lands, checked after PATH.
$fallbackDirs = @(
    "$env:ProgramFiles\gettext-iconv\bin"
    "${env:ProgramFiles(x86)}\gettext-iconv\bin"
    "$env:ProgramData\chocolatey\bin"
    "$env:ProgramFiles\Git\usr\bin"
)

function Resolve-GettextTool {
    param([Parameter(Mandatory)][string]$Name)

    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    foreach ($dir in $fallbackDirs) {
        $candidate = Join-Path $dir "$Name.exe"
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }

    throw @"
$Name was not found.

This script needs GNU gettext. Install it with one of:

    choco install gettext
    winget install --id mlocati.GettextIconv

Then reopen the terminal, or let this script find it automatically in the default
install directory. gettext is only needed to update the catalogues -- building the
project does not require it.
"@
}

$xgettext = Resolve-GettextTool 'xgettext'
$msgmerge = Resolve-GettextTool 'msgmerge'
Write-Host "xgettext: $xgettext"
Write-Host "msgmerge: $msgmerge"

# Every source file that could contain a _() call. The old script only scanned
# AudioPlaybackConnector.cpp, so a string added in any header was silently missed.
$sources = Get-ChildItem -LiteralPath $repoRoot -File |
    Where-Object { $_.Extension -in '.cpp', '.hpp' } |
    ForEach-Object { $_.Name } |
    Sort-Object

if (-not $sources) { throw "No .cpp/.hpp files found in $repoRoot" }
Write-Host "`nScanning: $($sources -join ', ')"

Push-Location $repoRoot
try {
    # --from-code is required because the sources carry UTF-8 comments.
    # --keyword tells xgettext that _(x) and C_(context, x) mark translatable text.
    & $xgettext `
        --output "$potPath" `
        --c++ `
        --from-code=UTF-8 `
        --add-comments=/ `
        --keyword=_ `
        --keyword=C_:1c,2 `
        --copyright-holder="$copyrightHolder" `
        --package-name="$packageName" `
        $sources
    if ($LASTEXITCODE -ne 0) { throw "xgettext failed with exit code $LASTEXITCODE" }
}
finally {
    Pop-Location
}
Write-Host "Wrote $(Split-Path $potPath -Leaf)"

$catalogues = Get-ChildItem -LiteralPath $sourceDir -Filter '*.po' | Sort-Object Name
if (-not $catalogues) { throw "No .po files in $sourceDir" }

Write-Host ''
foreach ($po in $catalogues) {
    # --previous keeps the old msgid on fuzzy entries so a translator can see what
    # changed. --backup=none stops msgmerge leaving .po~ files behind.
    & $msgmerge --update --previous --backup=none "$($po.FullName)" "$potPath"
    if ($LASTEXITCODE -ne 0) { throw "msgmerge failed for $($po.Name) with exit code $LASTEXITCODE" }
}

Write-Host @"

Catalogues updated. Next:
  * Translate any new or fuzzy entries in translate\source\*.po (Poedit works well).
  * python translate\build_translations.py --check   lists anything still untranslated.
  * Building the project regenerates the .ymo resources automatically.
"@
