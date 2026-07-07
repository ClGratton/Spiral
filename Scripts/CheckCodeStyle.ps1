param()

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Checker = Join-Path $Root "tools/check_code_style.py"

$Python = Get-Command python -ErrorAction SilentlyContinue
if (!$Python) {
    $Python = Get-Command python3 -ErrorAction SilentlyContinue
}

if (!$Python) {
    throw "Python 3 was not found on PATH. Install Python 3 to run the code style check."
}

& $Python.Source $Checker
if ($LASTEXITCODE -ne 0) {
    throw "Code style check failed with exit code $LASTEXITCODE."
}
