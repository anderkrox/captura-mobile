function Assert-FfmpegArchiveChecksum {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)]
        [ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ExpectedSha256
    )

    $Actual = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256 -ErrorAction Stop).Hash
    if ($Actual -ne $ExpectedSha256) {
        throw "Checksum invalido. Esperado: $ExpectedSha256; obtido: $Actual"
    }
}

function Get-FfmpegPackageRoot {
    param([Parameter(Mandatory = $true)][string]$ExtractedPath)

    $Roots = @(Get-ChildItem -LiteralPath $ExtractedPath -Directory -ErrorAction Stop)
    if ($Roots.Count -ne 1) {
        throw 'Estrutura inesperada: o pacote deve conter exatamente uma pasta raiz.'
    }

    foreach ($RelativePath in @('bin\ffmpeg.exe', 'bin\ffprobe.exe', 'LICENSE')) {
        $FilePath = Join-Path $Roots[0].FullName $RelativePath
        if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf) -or
            (Get-Item -LiteralPath $FilePath).Length -eq 0) {
            throw "Arquivo obrigatorio ausente ou vazio: $RelativePath"
        }
    }

    return $Roots[0].FullName
}

function Install-FfmpegPackage {
    param(
        [Parameter(Mandatory = $true)][string]$ExtractedPath,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    # Valida todos os arquivos antes de criar ou modificar o destino.
    $Root = Get-FfmpegPackageRoot -ExtractedPath $ExtractedPath
    $BinDirectory = Join-Path $Destination 'bin'
    New-Item -ItemType Directory -Force -Path $BinDirectory -ErrorAction Stop | Out-Null
    foreach ($Name in @('ffmpeg.exe', 'ffprobe.exe')) {
        Copy-Item -LiteralPath (Join-Path $Root "bin\$Name") -Destination (Join-Path $BinDirectory $Name) -Force -ErrorAction Stop
    }
    Copy-Item -LiteralPath (Join-Path $Root 'LICENSE') -Destination (Join-Path $Destination 'LICENSE') -Force -ErrorAction Stop
}

function Invoke-FfmpegEncoderProbe {
    param(
        [Parameter(Mandatory = $true)][string]$FfmpegPath,
        [Parameter(Mandatory = $true)][ValidateSet('libx264', 'h264_nvenc')][string]$Encoder
    )

    $Process = New-Object System.Diagnostics.Process
    $Process.StartInfo.FileName = $FfmpegPath
    $Process.StartInfo.Arguments = '-hide_banner -loglevel error -f lavfi -i color=c=black:s=486x864:r=30:d=1 -vf scale=1080:1920:flags=lanczos,format=yuv420p -an -c:v ' + $Encoder + ' -f null -'
    $Process.StartInfo.UseShellExecute = $false
    $Process.StartInfo.CreateNoWindow = $true
    $Process.StartInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $Process.StartInfo.RedirectStandardError = $true
    $Process.StartInfo.RedirectStandardOutput = $true
    try {
        [void]$Process.Start()
        $Stdout = $Process.StandardOutput.ReadToEndAsync()
        $Stderr = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit(15000)) {
            $Process.Kill()
            $Process.WaitForExit()
            throw "O teste do encoder $Encoder excedeu 15 segundos."
        }
        return [pscustomobject]@{
            ExitCode = $Process.ExitCode
            Output = $Stdout.Result + $Stderr.Result
        }
    } finally {
        $Process.Dispose()
    }
}

function Get-FfmpegEncoderSelection {
    param([Parameter(Mandatory = $true)][string]$FfmpegPath)

    $Cpu = Invoke-FfmpegEncoderProbe -FfmpegPath $FfmpegPath -Encoder 'libx264'
    if ($Cpu.ExitCode -ne 0) {
        throw "O fallback libx264 nao inicializou: $($Cpu.Output)"
    }

    $Nvenc = Invoke-FfmpegEncoderProbe -FfmpegPath $FfmpegPath -Encoder 'h264_nvenc'
    return [pscustomobject]@{
        SelectedEncoder = $(if ($Nvenc.ExitCode -eq 0) { 'h264_nvenc' } else { 'libx264' })
        CpuAvailable = $true
        NvencAvailable = ($Nvenc.ExitCode -eq 0)
        NvencDiagnostic = $Nvenc.Output
    }
}
