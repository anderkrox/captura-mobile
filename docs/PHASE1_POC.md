# Fase 1 - prova de conceito de captura

`YourotsCapturePoc` isola a validacao tecnica de `Windows.Graphics.Capture` da
interface definitiva, que pertence a Fase 2.

O executavel:

- verifica `GraphicsCaptureSession::IsSupported()` em tempo de execucao;
- enumera janelas visiveis e permite selecionar uma por `HWND`;
- cria o item de captura nativo diretamente a partir do `HWND`;
- usa `Direct3D11CaptureFramePool::CreateFreeThreaded`, portanto o recebimento
  de quadros nao depende da thread de interface;
- copia o recorte escolhido para uma textura `D3D11_USAGE_STAGING` e respeita
  o `RowPitch` ao transferir os pixels para a CPU;
- salva um quadro BGRA em PNG;
- cria uma janela magenta sobre a janela fonte para comprovar se uma janela
  sobreposta aparece ou nao dentro da captura por janela;
- envia o quadro mais recente a 30 FPS para o FFmpeg. Quando nao chega um novo
  quadro no intervalo, o ultimo quadro e repetido, preservando a duracao;
- gera MP4 H.264 `yuv420p`, 1080 x 1920, sem audio, usando `libx264` nesta prova
  de conceito porque o NVENC do pacote atual exige driver mais novo.

O recorte aceita quatro inteiros decimais sem sinal, com largura e altura
positivas. `--seconds` aceita valores entre 1 e 3600. A POC informa falhas da
captura durante a gravacao, em vez de continuar a repetir um quadro apos uma
falha. O fechamento da fonte e o redimensionamento que invalida o recorte
encerram a operacao com codigo de erro. Os callbacks usam uma referencia fraca
e mantem a sessao viva durante cada chamada; o encerramento e garantido tambem
quando ha excecoes.

## Uso

Compilar sem executar a suite de testes:

```powershell
cmake --preset vs2022-x64 -DBUILD_TESTING=OFF
cmake --build --preset release --target YourotsCapturePoc
```

Listar janelas que podem ser escolhidas:

```powershell
.\build\vs2022-x64\Release\YourotsCapturePoc.exe --list-windows
```

Capturar um quadro inteiro do Edge encontrado automaticamente:

```powershell
.\build\vs2022-x64\Release\YourotsCapturePoc.exe `
  --snapshot artifacts\phase1\edge-full.png
```

Depois de inspecionar esse quadro, informar manualmente o recorte em pixels
fisicos. O formato e `X,Y,LARGURA,ALTURA`. Para o Mobile configurado no projeto,
o tamanho esperado do recorte e `486 x 864`:

```powershell
.\build\vs2022-x64\Release\YourotsCapturePoc.exe `
  --crop X,Y,486,864 `
  --snapshot artifacts\phase1\mobile.png `
  --overlay-snapshot artifacts\phase1\mobile-overlay-probe.png `
  --record artifacts\phase1\phase1-30s.mp4 `
  --seconds 30
```

Tambem e possivel fixar a janela escolhida com `--hwnd <valor>`.

O teste de sobreposicao imprime `overlay_magenta_pixels`. O valor esperado para
captura por janela e zero ou proximo de zero, pois a janela magenta externa nao
deve fazer parte da textura da janela capturada.

`overlay_visible=true` confirma que a sobreposicao estava visivel no desktop.
Uma fonte estatica nao precisa emitir um quadro novo durante essa operacao:
a POC conserva o ultimo quadro recebido.

## Resultado validado em 01/10/2026

No ambiente descrito no plano, a POC encontrou suporte a
`Windows.Graphics.Capture` e capturou a janela do Edge em 1920 x 1040. A
inspecao do quadro completo determinou o recorte fisico atual como:

```text
X=410
Y=176
W=486
H=864
```

Artefatos gerados:

- `artifacts/phase1/edge-full.png`: quadro completo usado para calibracao;
- `artifacts/phase1/mobile.png`: recorte Mobile 486 x 864;
- `artifacts/phase1/mobile-overlay-probe.png`: recorte durante oclusao externa;
- `artifacts/phase1/desktop-during-capture.png`: area de trabalho durante a
  captura, mostrando a borda amarela de indicacao do Windows;
- `artifacts/phase1/phase1-30s.mp4`: gravacao experimental;
- `artifacts/phase1/phase1-30s-frame15.png`: quadro extraido aos 15 segundos.

A janela magenta de 240 x 240 foi colocada sobre o navegador. A POC reportou
`overlay_magenta_pixels=0`, portanto a oclusao externa nao apareceu no quadro da
janela capturada.

No Windows 10 testado, `GraphicsCaptureSession.IsBorderRequired` nao esta
disponivel. A indicacao amarela do sistema permanece visivel na area de trabalho
enquanto a captura esta ativa, mas nao entrou no recorte nem no video.

O FFprobe reportou para `phase1-30s.mp4`: H.264, 1080 x 1920, `yuv420p`,
30 FPS e duracao de 30,000 segundos. O arquivo possui somente stream de video,
sem audio.

## Testes automatizados

A suite agora inclui 50 testes unitarios do nucleo de captura, 14 testes
unitarios das dependencias e dois E2E: Fase 0 e captura nativa da Fase 1.
Instrucoes, requisitos do desktop e relatorios estao em
[tests/README.md](../tests/README.md).

Em 01/10/2026, ambos os builds x64 (Release e Debug) e todas as quatro entradas
CTest de cada configuracao passaram: 64 casos unitarios e dois E2E.

O E2E da Fase 1 confere pixels de uma janela Win32 controlada, pois isso permite
detectar recortes deslocados e perda de movimento sem depender do estado do
jogo. No Windows atual, uma janela de teste com `WS_EX_TOOLWINDOW` foi rejeitada
por `CreateForWindow` com `E_INVALIDARG`; a fonte de teste usa `WS_EX_APPWINDOW`.
A selecao de uma janela visivel nao garante que o sistema permita captura.

Os videos produzidos pela POC passam a declarar pixels quadrados e BT.709.
Essa verificacao foi acrescentada apos o E2E detectar metadados de proporcao
ausentes na primeira versao.

Referencias sobre callbacks e ciclo de vida:
[CreateFreeThreaded](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded)
e [referencias fortes e fracas em C++/WinRT](https://learn.microsoft.com/en-us/windows/uwp/cpp-and-winrt-apis/weak-references).
