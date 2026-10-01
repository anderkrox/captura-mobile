# Dependencias de desenvolvimento

Versoes fixadas para a primeira implementacao:

- Visual Studio Community 2022 17.14.40.
- MSVC 14.44.35207, alvo x64.
- CMake 4.3.2 (minimo aceito pelo projeto: 3.28).
- Windows SDK 10.0.26100.0.
- C++20.
- Windows.Graphics.Capture via C++/WinRT.
- Direct3D 11 e DXGI.
- FFmpeg/FFprobe 9.0.2, build Windows x64 Essentials da Gyan.

Para os testes: Python 3.10 ou superior, Windows PowerShell e Pester 3.4.0.
As instrucoes e a cobertura estao em [tests/README.md](../tests/README.md).
Essas dependencias nao sao necessarias com `BUILD_TESTING=OFF`.

O build Essentials foi escolhido porque ja inclui suporte a NVENC e contem
`ffmpeg.exe` e `ffprobe.exe`. O MVP chamara esses executaveis como processos
externos; nao fara link com as bibliotecas `libav*`.

No computador de desenvolvimento, a GPU e uma NVIDIA GeForce RTX 5060 com
driver 596.49. O FFmpeg 9.0.2 distribuido atualmente pela Gyan foi compilado
com NVENC API 13.1 e informa que essa API exige driver 610.00 ou superior.
Por isso, `h264_nvenc` aparece na lista de encoders, mas nao inicializa com o
driver atual. O fallback `libx264` por CPU foi validado e deve permanecer
disponivel. A inicializacao de NVENC sera testada em tempo de execucao e o
aplicativo recorrera ao fallback quando necessario.

Checksum SHA-256 do arquivo `ffmpeg-release-essentials.zip` consultado em
01/10/2026:

`60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba`

O pacote distribuido pela Gyan e GPLv3. Os avisos e licencas correspondentes
devem acompanhar a distribuicao final da ferramenta.
