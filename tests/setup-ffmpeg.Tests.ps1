. (Join-Path $PSScriptRoot '..\scripts\ffmpeg-functions.ps1')

Describe 'Integridade do arquivo FFmpeg' {
    BeforeEach {
        $ArchivePath = Join-Path $TestDrive 'package.zip'
        Set-Content -LiteralPath $ArchivePath -Value 'fixture de teste' -Encoding ASCII
        $Hash = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash
    }

    It 'aceita SHA-256 correto, independentemente de maiusculas' {
        { Assert-FfmpegArchiveChecksum $ArchivePath $Hash.ToLowerInvariant() } | Should Not Throw
    }

    It 'rejeita arquivo alterado apos o calculo do hash' {
        Add-Content -LiteralPath $ArchivePath -Value 'adulterado'
        { Assert-FfmpegArchiveChecksum $ArchivePath $Hash } | Should Throw 'Checksum invalido'
    }

    It 'rejeita arquivo inexistente' {
        { Assert-FfmpegArchiveChecksum (Join-Path $TestDrive 'missing.zip') $Hash } | Should Throw
    }

    It 'rejeita checksum malformado' {
        { Assert-FfmpegArchiveChecksum $ArchivePath 'invalid' } | Should Throw
    }
}

Describe 'Validacao e instalacao do pacote extraido' {
    BeforeEach {
        $ExtractedPath = Join-Path $TestDrive ([guid]::NewGuid().ToString('N'))
        $RootPath = Join-Path $ExtractedPath 'ffmpeg-fixture'
        New-Item -ItemType Directory -Path (Join-Path $RootPath 'bin') -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $RootPath 'bin\ffmpeg.exe') -Value 'ffmpeg fixture' -Encoding ASCII
        Set-Content -LiteralPath (Join-Path $RootPath 'bin\ffprobe.exe') -Value 'ffprobe fixture' -Encoding ASCII
        Set-Content -LiteralPath (Join-Path $RootPath 'LICENSE') -Value 'license fixture' -Encoding ASCII
        $DestinationPath = Join-Path $TestDrive ([guid]::NewGuid().ToString('N'))
    }

    It 'localiza a raiz quando os tres arquivos obrigatorios estao presentes' {
        Get-FfmpegPackageRoot $ExtractedPath | Should Be $RootPath
    }

    It 'rejeita multiplas pastas raiz' {
        New-Item -ItemType Directory -Path (Join-Path $ExtractedPath 'extra') | Out-Null
        { Get-FfmpegPackageRoot $ExtractedPath } | Should Throw 'exatamente uma pasta raiz'
    }

    It 'rejeita pasta sem pacote' {
        $EmptyPath = Join-Path $TestDrive ([guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $EmptyPath | Out-Null
        { Get-FfmpegPackageRoot $EmptyPath } | Should Throw 'exatamente uma pasta raiz'
    }

    It 'rejeita ffprobe ausente sem criar instalacao parcial' {
        Remove-Item -LiteralPath (Join-Path $RootPath 'bin\ffprobe.exe')
        { Install-FfmpegPackage $ExtractedPath $DestinationPath } | Should Throw 'ffprobe.exe'
        Test-Path -LiteralPath $DestinationPath | Should Be $false
    }

    It 'rejeita pacote sem licenca' {
        Remove-Item -LiteralPath (Join-Path $RootPath 'LICENSE')
        { Get-FfmpegPackageRoot $ExtractedPath } | Should Throw 'LICENSE'
    }

    It 'rejeita binario vazio' {
        [System.IO.File]::WriteAllBytes((Join-Path $RootPath 'bin\ffmpeg.exe'), [byte[]]@())
        { Get-FfmpegPackageRoot $ExtractedPath } | Should Throw 'ffmpeg.exe'
    }

    It 'instala ambos os binarios e a licenca preservando os bytes' {
        Install-FfmpegPackage $ExtractedPath $DestinationPath
        foreach ($RelativePath in @('bin\ffmpeg.exe', 'bin\ffprobe.exe', 'LICENSE')) {
            $Expected = (Get-FileHash -LiteralPath (Join-Path $RootPath $RelativePath)).Hash
            (Get-FileHash -LiteralPath (Join-Path $DestinationPath $RelativePath)).Hash | Should Be $Expected
        }
    }
}

Describe 'Escolha de encoder com probes isolados' {
    BeforeEach {
        Mock Invoke-FfmpegEncoderProbe { [pscustomobject]@{ ExitCode = 0; Output = '' } }
    }

    It 'prefere NVENC quando os dois encoders inicializam' {
        $Result = Get-FfmpegEncoderSelection 'fixture.exe'
        $Result.SelectedEncoder | Should Be 'h264_nvenc'
        $Result.NvencAvailable | Should Be $true
    }

    It 'usa CPU e preserva o diagnostico quando NVENC falha' {
        Mock Invoke-FfmpegEncoderProbe {
            [pscustomobject]@{ ExitCode = -40; Output = 'Driver does not support the required nvenc API version' }
        } -ParameterFilter { $Encoder -eq 'h264_nvenc' }
        $Result = Get-FfmpegEncoderSelection 'fixture.exe'
        $Result.SelectedEncoder | Should Be 'libx264'
        $Result.NvencAvailable | Should Be $false
        $Result.NvencDiagnostic | Should Match 'nvenc API'
    }

    It 'reporta falha quando o fallback CPU nao pode inicializar' {
        Mock Invoke-FfmpegEncoderProbe {
            [pscustomobject]@{ ExitCode = 1; Output = 'encoder indisponivel' }
        } -ParameterFilter { $Encoder -eq 'libx264' }
        { Get-FfmpegEncoderSelection 'fixture.exe' } | Should Throw 'fallback libx264'
        Assert-MockCalled Invoke-FfmpegEncoderProbe -Times 0 -Exactly -Scope It -ParameterFilter { $Encoder -eq 'h264_nvenc' }
    }
}
