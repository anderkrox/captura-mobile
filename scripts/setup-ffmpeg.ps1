param(
    [string]$Destination = (Join-Path $PSScriptRoot "..\third_party\ffmpeg")
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
. (Join-Path $PSScriptRoot 'ffmpeg-functions.ps1')

$Version = "9.0.2"
$ExpectedSha256 = "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba"
$Url = "https://github.com/GyanD/codexffmpeg/releases/download/$Version/ffmpeg-$Version-essentials_build.zip"

$TemporaryBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$WorkDirectory = Join-Path $TemporaryBase ("yourots-ffmpeg-" + [guid]::NewGuid().ToString('N'))
$TempArchive = Join-Path $WorkDirectory "ffmpeg-$Version.zip"
$TempExtract = Join-Path $WorkDirectory 'extracted'
New-Item -ItemType Directory -Path $WorkDirectory | Out-Null

try {
    Invoke-WebRequest $Url -OutFile $TempArchive -UseBasicParsing
    Assert-FfmpegArchiveChecksum -ArchivePath $TempArchive -ExpectedSha256 $ExpectedSha256
    Expand-Archive -LiteralPath $TempArchive -DestinationPath $TempExtract
    Install-FfmpegPackage -ExtractedPath $TempExtract -Destination $Destination
} finally {
    $ResolvedWorkDirectory = [System.IO.Path]::GetFullPath($WorkDirectory)
    $AllowedPrefix = $TemporaryBase.TrimEnd('\') + '\yourots-ffmpeg-'
    if (-not $ResolvedWorkDirectory.StartsWith($AllowedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'Diretorio temporario fora do limite esperado.'
    }
    Remove-Item -LiteralPath $ResolvedWorkDirectory -Recurse -Force
}

Set-Content -Path (Join-Path $Destination "VERSION.txt") -Value @(
    "FFmpeg $Version"
    "Source: $Url"
    "SHA256: $ExpectedSha256"
)

& (Join-Path $Destination "bin\ffmpeg.exe") -hide_banner -version | Select-Object -First 1
& (Join-Path $Destination "bin\ffmpeg.exe") -hide_banner -encoders 2>$null | Select-String "h264_nvenc"
& (Join-Path $Destination "bin\ffprobe.exe") -hide_banner -version | Select-Object -First 1

$Selection = Get-FfmpegEncoderSelection -FfmpegPath (Join-Path $Destination 'bin\ffmpeg.exe')
Write-Output 'libx264 smoke test: OK'
if ($Selection.NvencAvailable) {
    Write-Output "NVENC smoke test: OK"
} else {
    Write-Warning "NVENC esta presente no FFmpeg, mas nao inicializou com o driver NVIDIA atual. O aplicativo deve usar libx264 como fallback."
}
