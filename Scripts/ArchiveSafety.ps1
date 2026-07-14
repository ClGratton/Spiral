$ErrorActionPreference = "Stop"

function Test-SafeArchivePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $normalized = $Path.Replace('\', '/')
    if ([string]::IsNullOrWhiteSpace($normalized) -or
        $normalized.StartsWith('/') -or
        $normalized -match '^[A-Za-z]:' -or
        $normalized.IndexOf([char]0) -ge 0) {
        return $false
    }

    $depth = 0
    foreach ($segment in $normalized.Split('/', [System.StringSplitOptions]::RemoveEmptyEntries)) {
        if ($segment -eq '.') { continue }
        if ($segment -eq '..') {
            if ($depth -eq 0) { return $false }
            --$depth
        }
        else {
            ++$depth
        }
    }
    return $true
}

function Test-SafeArchiveLink {
    param(
        [Parameter(Mandatory = $true)][string]$Member,
        [Parameter(Mandatory = $true)][string]$Target
    )

    $memberPath = $Member.Replace('\', '/')
    $targetPath = $Target.Replace('\', '/')
    if (!(Test-SafeArchivePath $memberPath) -or
        [string]::IsNullOrWhiteSpace($targetPath) -or
        $targetPath.StartsWith('/') -or
        $targetPath -match '^[A-Za-z]:' -or
        $targetPath.IndexOf([char]0) -ge 0) {
        return $false
    }

    $parent = [System.IO.Path]::GetDirectoryName($memberPath).Replace('\', '/')
    $combined = if ([string]::IsNullOrEmpty($parent)) { $targetPath } else { "$parent/$targetPath" }
    return Test-SafeArchivePath $combined
}

function Assert-SafeZipArchive {
    param([Parameter(Mandatory = $true)][string]$Archive)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        foreach ($entry in $zip.Entries) {
            if (!(Test-SafeArchivePath $entry.FullName)) {
                throw "Archive '$Archive' contains unsafe member '$($entry.FullName)'."
            }

            $unixMode = (($entry.ExternalAttributes -shr 16) -band 0xF000)
            if ($unixMode -eq 0xA000) {
                $reader = [System.IO.StreamReader]::new($entry.Open())
                try { $target = $reader.ReadToEnd() } finally { $reader.Dispose() }
                if (!(Test-SafeArchiveLink $entry.FullName $target)) {
                    throw "Archive '$Archive' contains escaping symbolic link '$($entry.FullName)' -> '$target'."
                }
            }
        }
    }
    finally {
        $zip.Dispose()
    }
}

function Assert-SafeTarArchive {
    param([Parameter(Mandatory = $true)][string]$Archive)

    $members = @(& tar -tzf $Archive)
    if ($LASTEXITCODE -ne 0) { throw "Could not list archive '$Archive'." }
    $verbose = @(& tar -tvzf $Archive)
    if ($LASTEXITCODE -ne 0) { throw "Could not inspect archive '$Archive'." }

    foreach ($member in $members) {
        if (!(Test-SafeArchivePath $member)) {
            throw "Archive '$Archive' contains unsafe member '$member'."
        }

        $marker = " $member -> "
        $linkLine = @($verbose | Where-Object { $_.StartsWith('l') -and $_.Contains($marker) })
        if ($linkLine.Count -gt 1) { throw "Archive '$Archive' has ambiguous symbolic-link metadata for '$member'." }
        if ($linkLine.Count -eq 1) {
            $target = $linkLine[0].Substring($linkLine[0].IndexOf($marker) + $marker.Length)
            if (!(Test-SafeArchiveLink $member $target)) {
                throw "Archive '$Archive' contains escaping symbolic link '$member' -> '$target'."
            }
        }
    }

    if (@($verbose | Where-Object { $_.StartsWith('h') }).Count -gt 0) {
        throw "Archive '$Archive' contains hard links, which are not admitted by the toolchain extractor."
    }
}
