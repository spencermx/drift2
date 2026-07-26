[CmdletBinding()]
param(
    [string]$QualityRoot,
    [string]$RepoRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$failures = [System.Collections.Generic.List[string]]::new()

function Add-Failure {
    param([string]$Message)
    $script:failures.Add($Message)
}

function Get-MetadataValue {
    param(
        [string[]]$Lines,
        [string]$Name
    )

    $prefix = "**${Name}:**"
    $line = $Lines | Where-Object { $_.StartsWith($prefix) } |
        Select-Object -First 1
    if ($null -eq $line) { return $null }
    return $line.Substring($prefix.Length).Trim()
}

function Remove-CodeTicks {
    param([string]$Value)
    if ($null -eq $Value) { return $null }
    return $Value.Trim().Trim([char]0x60)
}

function Test-EmptyAttribution {
    param([string]$Value)
    return [string]::IsNullOrWhiteSpace($Value) -or
        $Value -eq '-' -or $Value -eq ([char]0x2014).ToString()
}

function Get-ImplementedIdentities {
    param([string]$Value)
    if (Test-EmptyAttribution $Value) { return @() }
    return @($Value -split '[;,]' | ForEach-Object { $_.Trim() } |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
}

function Get-ReviewerIdentities {
    param([string]$Value)
    if (Test-EmptyAttribution $Value) { return @() }

    $identities = [System.Collections.Generic.List[string]]::new()
    foreach ($entry in ($Value -split '[;,]')) {
        $trimmed = $entry.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed)) { continue }
        $verdict = [regex]::Match(
            $trimmed,
            '^(.*):\s*(approved with residual risk|approved|changes requested)$',
            [Text.RegularExpressions.RegexOptions]::IgnoreCase
        )
        $identity = if ($verdict.Success) {
            $verdict.Groups[1].Value.Trim()
        } else {
            $trimmed
        }
        if (-not [string]::IsNullOrWhiteSpace($identity)) {
            $identities.Add($identity)
        }
    }
    return @($identities)
}

function Test-CommitTrailer {
    param(
        [string]$Messages,
        [string]$Name,
        [string]$Value
    )

    $pattern = '(?im)^' + [regex]::Escape($Name) + ':\s*' +
        [regex]::Escape($Value) + '\s*$'
    return [regex]::IsMatch($Messages, $pattern)
}

function Get-AuditMessages {
    param([string]$Id)

    $safeDirectory = "safe.directory=$script:resolvedRepoRoot"
    $messageLines = & git -c $safeDirectory -C $script:resolvedRepoRoot log --all '--format=%B' "--grep=Audit-ID: $Id" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Add-Failure "$Id audit history could not be read with git log."
        return ''
    }
    return [string]::Join([Environment]::NewLine, $messageLines)
}

function Get-RepoRelativePath {
    param([string]$FullPath)

    $rootWithSlash = $script:resolvedRepoRoot.TrimEnd('\', '/') +
        [IO.Path]::DirectorySeparatorChar
    if (-not $FullPath.StartsWith($rootWithSlash,
            [StringComparison]::OrdinalIgnoreCase)) {
        return $null
    }
    return $FullPath.Substring($rootWithSlash.Length)
}

if ([string]::IsNullOrWhiteSpace($QualityRoot)) {
    $QualityRoot = $PSScriptRoot
}
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}

try {
    $resolvedQualityRoot = (Resolve-Path -LiteralPath $QualityRoot).Path
    $resolvedRepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
} catch {
    Write-Error "Quality or repository root could not be resolved: $($_.Exception.Message)"
    exit 1
}

$safeDirectory = "safe.directory=$resolvedRepoRoot"
$gitTop = & git -c $safeDirectory -C $resolvedRepoRoot rev-parse --show-toplevel 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Error "Repository root is not inside a Git worktree: $resolvedRepoRoot"
    exit 1
}
$gitTop = (Resolve-Path -LiteralPath $gitTop.Trim()).Path
if (-not $resolvedRepoRoot.Equals($gitTop,
        [StringComparison]::OrdinalIgnoreCase)) {
    Add-Failure "RepoRoot must be the Git worktree root ($gitTop)."
}

$allowedSeverities = @('High', 'Medium', 'Low')
$allowedStatuses = @(
    'Untriaged', 'Investigating', 'Fix planned', 'Fixing',
    'Awaiting review', 'Decision needed', 'Verified', 'Accepted risk',
    "Won't fix", 'Not a bug', 'Deferred'
)

$trackerPath = Join-Path $resolvedQualityRoot 'TRACKER.md'
$readmePath = Join-Path $resolvedQualityRoot 'README.md'
if (-not (Test-Path -LiteralPath $trackerPath -PathType Leaf)) {
    Add-Failure 'TRACKER.md is missing.'
}
if (-not (Test-Path -LiteralPath $readmePath -PathType Leaf)) {
    Add-Failure 'README.md is missing.'
}

$trackerRows = [System.Collections.Generic.List[object]]::new()
$rowsById = @{}
$previousNumber = 0

if (Test-Path -LiteralPath $trackerPath -PathType Leaf) {
    foreach ($line in (Get-Content -Encoding utf8 $trackerPath)) {
        if ($line -notmatch '^\|\s*\[?DRIFT-\d{3,}') { continue }

        $columns = [regex]::Split($line, '(?<!\\)\|')
        if ($columns.Count -lt 10) {
            Add-Failure "Malformed tracker row: $line"
            continue
        }

        $idCell = $columns[1].Trim()
        $idMatch = [regex]::Match($idCell, 'DRIFT-(\d{3,})')
        if (-not $idMatch.Success) {
            Add-Failure "Tracker row has no valid ID: $line"
            continue
        }

        $id = 'DRIFT-' + $idMatch.Groups[1].Value
        $number = [int]$idMatch.Groups[1].Value
        $linkMatch = [regex]::Match(
            $idCell,
            '^\[DRIFT-\d{3,}\]\(([^)]+)\)$'
        )
        $row = [pscustomobject]@{
            Id = $id
            Number = $number
            Linked = $linkMatch.Success
            LinkTarget = if ($linkMatch.Success) {
                $linkMatch.Groups[1].Value
            } else {
                $null
            }
            Severity = $columns[2].Trim()
            Status = $columns[3].Trim()
            Implemented = $columns[6].Trim()
            Reviewed = $columns[7].Trim()
        }

        if ($rowsById.ContainsKey($id)) {
            Add-Failure "Duplicate tracker ID: $id."
            continue
        }
        $rowsById[$id] = $row
        $trackerRows.Add($row)

        if ($number -ne $previousNumber + 1) {
            Add-Failure "Tracker IDs must be contiguous and ascending: expected " +
                ('DRIFT-{0:D3}' -f ($previousNumber + 1)) + ", found $id."
        }
        $previousNumber = $number

        if ($row.Severity -notin $allowedSeverities) {
            Add-Failure "$id has invalid severity '$($row.Severity)'."
        }
        if ($row.Status -notin $allowedStatuses) {
            Add-Failure "$id has invalid status '$($row.Status)'."
        }
    }
}

if ($trackerRows.Count -eq 0) {
    Add-Failure 'TRACKER.md contains no DRIFT issue rows.'
}

$activeRows = @($trackerRows | Where-Object {
    $_.Status -eq 'Investigating' -or $_.Status -eq 'Fixing'
})
if ($activeRows.Count -gt 1) {
    Add-Failure ('More than one issue is active: ' +
        (($activeRows | ForEach-Object { "$($_.Id) ($($_.Status))" }) -join ', ') + '.')
}

$issuesById = @{}
$issuesDir = Join-Path $resolvedQualityRoot 'issues'
if (Test-Path -LiteralPath $issuesDir -PathType Container) {
    foreach ($file in (Get-ChildItem -LiteralPath $issuesDir -Filter 'DRIFT-*.md' -File)) {
        if ($file.BaseName -notmatch '^DRIFT-\d{3,}$') {
            Add-Failure "Issue filename is invalid: $($file.Name)."
            continue
        }

        $id = $file.BaseName
        if ($issuesById.ContainsKey($id)) {
            Add-Failure "Duplicate issue file for $id."
            continue
        }
        $issuesById[$id] = $file

        if (-not $rowsById.ContainsKey($id)) {
            Add-Failure "$id has an issue file but no tracker row."
            continue
        }

        $lines = @(Get-Content -Encoding utf8 $file.FullName)
        $status = Remove-CodeTicks (Get-MetadataValue $lines 'Current status')
        $reported = Get-MetadataValue $lines 'Reported'
        $implemented = Get-MetadataValue $lines 'Implemented by'
        $reviewed = Get-MetadataValue $lines 'Reviewed by'
        $primaryLocations = Get-MetadataValue $lines 'Primary locations'
        $decisionOwner = Get-MetadataValue $lines 'Decision owner'
        $finalSeverity = Get-MetadataValue $lines 'Final severity'
        $initialSeverity = Get-MetadataValue $lines 'Initial severity'
        $severity = if ([string]::IsNullOrWhiteSpace($finalSeverity)) {
            $initialSeverity
        } else {
            $finalSeverity
        }
        $row = $rowsById[$id]

        if ($lines.Count -eq 0 -or $lines[0] -notmatch ('^#\s+' + [regex]::Escape($id) + '\b')) {
            Add-Failure "$id issue file does not begin with its canonical ID heading."
        }
        foreach ($required in @(
            @{ Name = 'Reported'; Value = $reported },
            @{ Name = 'Initial severity'; Value = $initialSeverity },
            @{ Name = 'Primary locations'; Value = $primaryLocations },
            @{ Name = 'Implemented by'; Value = $implemented },
            @{ Name = 'Reviewed by'; Value = $reviewed },
            @{ Name = 'Decision owner'; Value = $decisionOwner }
        )) {
            if ([string]::IsNullOrWhiteSpace($required.Value)) {
                Add-Failure "$id issue file has no $($required.Name) field."
            }
        }
        if ([string]::IsNullOrWhiteSpace($status)) {
            Add-Failure "$id issue file has no Current status field."
        } elseif ($status -ne $row.Status) {
            Add-Failure "$id status differs: tracker '$($row.Status)', issue '$status'."
        }
        if ($severity -ne $row.Severity) {
            Add-Failure "$id severity differs: tracker '$($row.Severity)', issue '$severity'."
        }
        if ($implemented -ne $row.Implemented) {
            Add-Failure "$id Implemented by differs between tracker and issue file."
        }
        if ($reviewed -ne $row.Reviewed) {
            Add-Failure "$id Reviewed by differs between tracker and issue file."
        }
        if (-not $row.Linked) {
            Add-Failure "$id has an issue file but its tracker ID is not linked."
        } else {
            $expectedTarget = "issues/$id.md"
            $actualTarget = $row.LinkTarget.Replace('\', '/')
            if ($actualTarget -ne $expectedTarget) {
                Add-Failure "$id tracker link points to '$($row.LinkTarget)', expected '$expectedTarget'."
            }
        }

        $body = [string]::Join([Environment]::NewLine, $lines)
        $implementedIdentities = @(Get-ImplementedIdentities $implemented)
        $reviewerIdentities = @(Get-ReviewerIdentities $reviewed)

        if ($status -eq 'Awaiting review' -or $status -eq 'Verified') {
            if ($implementedIdentities.Count -eq 0) {
                Add-Failure "$id is $status without an implementer."
            }
            if ($body -notmatch '(?m)^## (?:Independent review handoff|Reviewer instructions)\s*$') {
                Add-Failure "$id is $status without independent-review instructions."
            }

            $messages = Get-AuditMessages $id
            if (-not (Test-CommitTrailer $messages 'Audit-ID' $id)) {
                Add-Failure "$id has no discoverable exact Audit-ID trailer."
            }
            foreach ($identity in $implementedIdentities) {
                if (-not (Test-CommitTrailer $messages 'Implemented-by' $identity)) {
                    Add-Failure "$id lacks Implemented-by: $identity in its audit history."
                }
            }

            if ($status -eq 'Verified') {
                if ($reviewerIdentities.Count -eq 0) {
                    Add-Failure "$id is Verified without a reviewer."
                }
                if ($body -notmatch '(?m)^## Review history\s*$') {
                    Add-Failure "$id is Verified without a Review history section."
                }
                foreach ($reviewer in $reviewerIdentities) {
                    if ($implementedIdentities -contains $reviewer) {
                        Add-Failure "$id reviewer '$reviewer' is also an implementer."
                    }
                    if (-not (Test-CommitTrailer $messages 'Reviewed-by' $reviewer)) {
                        Add-Failure "$id lacks Reviewed-by: $reviewer in its audit history."
                    }
                }
            }
        }
    }
}

foreach ($row in $trackerRows) {
    if ($row.Status -ne 'Untriaged' -and -not $issuesById.ContainsKey($row.Id)) {
        Add-Failure "$($row.Id) is $($row.Status) but has no issue file."
    }
}

foreach ($markdown in (Get-ChildItem -LiteralPath $resolvedQualityRoot -Recurse -Filter '*.md' -File)) {
    $body = Get-Content -Raw -Encoding utf8 $markdown.FullName
    foreach ($match in [regex]::Matches($body, '\[[^\]]*\]\(([^)]+)\)')) {
        $target = $match.Groups[1].Value.Trim()
        if ($target -match '^(?:https?://|mailto:|#)') { continue }
        $pathOnly = ($target -split '#', 2)[0].Trim().Trim('<', '>')
        if ([string]::IsNullOrWhiteSpace($pathOnly)) { continue }
        $decoded = [Uri]::UnescapeDataString($pathOnly)
        $resolvedTarget = Join-Path $markdown.DirectoryName $decoded
        if (-not (Test-Path -LiteralPath $resolvedTarget)) {
            Add-Failure "Broken relative link in $($markdown.Name): $target."
        }
    }
}

$auditsDir = Join-Path $resolvedQualityRoot 'audits'
if (Test-Path -LiteralPath $auditsDir -PathType Container) {
    foreach ($audit in (Get-ChildItem -LiteralPath $auditsDir -Filter '*.md' -File)) {
        $relative = Get-RepoRelativePath $audit.FullName
        if ($null -eq $relative) {
            Add-Failure "Audit is outside RepoRoot and cannot be checked: $($audit.FullName)."
            continue
        }

        & git -c $safeDirectory -C $resolvedRepoRoot ls-files --error-unmatch -- $relative *> $null
        $tracked = $LASTEXITCODE -eq 0
        if (-not $tracked) { continue }

        $dirty = & git -c $safeDirectory -C $resolvedRepoRoot status --porcelain -- $relative
        if ($LASTEXITCODE -ne 0) {
            Add-Failure "Could not inspect audit status: $relative."
            continue
        }
        if ($dirty) {
            Add-Failure "Immutable audit has uncommitted changes: $relative."
        }

        $history = @(& git -c $safeDirectory -C $resolvedRepoRoot log --follow '--format=%H' -- $relative)
        if ($LASTEXITCODE -ne 0) {
            Add-Failure "Could not inspect audit history: $relative."
        } elseif ($history.Count -gt 1) {
            Add-Failure "Immutable audit was changed after creation: $relative."
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "Quality validation FAILED ($($failures.Count) problem(s)):" -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

$statusSummary = $trackerRows | Group-Object Status | Sort-Object Name |
    ForEach-Object { "$($_.Name)=$($_.Count)" }
Write-Host 'Quality validation passed.' -ForegroundColor Green
Write-Host "  Tracker rows: $($trackerRows.Count)"
Write-Host "  Detailed issue files: $($issuesById.Count)"
Write-Host "  Statuses: $($statusSummary -join ', ')"
