param([Parameter(Mandatory = $true)][string]$FfmpegPath)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ffmpeg-functions.ps1')

try {
    Get-FfmpegEncoderSelection -FfmpegPath $FfmpegPath | ConvertTo-Json
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
