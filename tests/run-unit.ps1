param([Parameter(Mandatory = $true)][string]$ReportPath)

$ErrorActionPreference = 'Stop'
Import-Module Pester -RequiredVersion 3.4.0 -ErrorAction Stop
New-Item -ItemType Directory -Path (Split-Path -Parent $ReportPath) -Force | Out-Null
$Result = Invoke-Pester -Script (Join-Path $PSScriptRoot 'setup-ffmpeg.Tests.ps1') -PassThru -OutputFormat NUnitXml -OutputFile $ReportPath
if ($Result.TotalCount -eq 0 -or $Result.FailedCount -gt 0) {
    exit 1
}
