# Testes das Fases 0 e 1

Os testes usam CTest, Python 3.10 ou superior e Pester 3.4.0, disponível neste
computador pelo Windows PowerShell. Não é necessário baixar bibliotecas de
teste. FFmpeg e FFprobe devem estar instalados em `third_party/ffmpeg/bin`.

Na raiz do projeto:

```powershell
cmake --preset vs2022-x64 -DBUILD_TESTING=ON
cmake --build --preset release
ctest --preset release
```

Para executar apenas uma categoria:

```powershell
ctest --preset release -L unit
ctest --preset release -L e2e
```

Também existem os presets `debug` para build e testes. Para compilar o
aplicativo sem as dependências de teste, configure com
`cmake --preset vs2022-x64 -DBUILD_TESTING=OFF`.

## Testes unitários

`capture_core_tests.cpp` contém 50 casos em C++ sobre o código usado pela POC:

- Parsing do recorte, campos ausentes, entradas negativas, sufixos e overflow.
- Limites do quadro, recortes nas extremidades e dimensões vazias.
- Cópia BGRA preservando canais e descartando padding entre linhas (`RowPitch`).
- Rejeição de buffers truncados e espaçamento insuficiente ou excessivo.
- Argumentos do processo com espaços, aspas, barras finais e Unicode,
  conferidos com o parser nativo `CommandLineToArgvW`.
- Duração positiva, limites e rejeição de entradas malformadas.

O executável `CaptureCoreTests` não usa GPU, navegador nem FFmpeg. O relatório
JUnit XML fica em `capture-core.xml`.

`setup-ffmpeg.Tests.ps1` contém 14 casos sobre funções usadas pelo instalador:

- SHA-256 correto, arquivo adulterado, ausente e checksum malformado.
- Estrutura do pacote, pastas ambíguas, ausência de FFprobe/licença e binário vazio.
- Rejeição de pacote incompleto antes de criar o destino e cópia fiel dos arquivos.
- Preferência por NVENC, fallback para CPU com diagnóstico e erro quando CPU falha.

Esses casos usam arquivos pequenos no `TestDrive` do Pester e mocks dos probes.
Não baixam pacotes nem executam os encoders reais.

## E2E da Fase 0

`e2e_phase0.py` executa um cenário com os binários reais:

1. Abre o aplicativo com a janela oculta, identifica sua janela pelo processo,
   confirma o título e a resposta da interface, fecha por `WM_CLOSE` e confere
   o código de saída. Repete o ciclo para verificar a próxima inicialização.
2. Confirma as versões de FFmpeg e FFprobe e usa o mesmo diagnóstico de
   encoders do instalador. Uma falha de NVENC pode selecionar `libx264`.
3. Envia 60 quadros BGRA de 486 × 864 pelo stdin do FFmpeg, com quatro cantos
   coloridos e um marcador em movimento. Gera um MKV de 2 segundos.
4. Remultiplexa para MP4 sem recodificar, com `faststart`.
5. Confere H.264, 1080 × 1920, 30 FPS, 9:16, pixels quadrados, `yuv420p`,
   BT.709, duração, quantidade de quadros e ausência de áudio pelo FFprobe.
6. Verifica que o atom `moov` antecede `mdat`, decodifica todo o vídeo e
   compara cores nos quatro cantos e posições do marcador no primeiro e
   último quadros, com tolerância para a compressão H.264.

O aplicativo `YourotsCapture` contém a inicialização da janela. Esse cenário
verifica essa inicialização e o fluxo externo de vídeo com imagens sintéticas.
A POC de captura nativa é validada pelo cenário da Fase 1 abaixo.

## E2E da Fase 1

`e2e_phase1.py` abre `CaptureFixture`, uma janela Win32 real com área de
486 × 864, quatro cantos coloridos e um marcador animado, cercada de magenta.
O teste chama `YourotsCapturePoc`; os pixels passam por
Windows.Graphics.Capture, Direct3D 11, recorte, PNG e FFmpeg reais.

São nove grupos de cenários:

1. Enumeração e seleção da janela por `HWND`.
2. Seis ciclos de início, captura de PNG e encerramento. Os quatro cantos são
   conferidos sem tolerância, e o ID da thread de callback difere do controle.
3. Janela magenta visível sobre a fonte, confirmada por pixel da área de
   trabalho, mas ausente da imagem capturada.
4. Gravação com movimento e destino com espaços e acentos. FFprobe confere
   H.264, 1080 × 1920, 9:16, pixels quadrados, BT.709, `yuv420p`, 30 FPS,
   90 quadros, três segundos e ausência de áudio. Também são conferidos
   `faststart`, decodificação completa, cantos e deslocamento do marcador.
5. Recorte fora do quadro, FFmpeg ausente e encerramento do encoder com erro.
6. Fonte estática e sobreposição, com repetição do último quadro e duração
   correta de dois segundos / 60 quadros.
7. Fechamento da fonte após o início da gravação, com erro informado.
8. Redimensionamento que invalida o recorte durante a gravação, com erro informado.
9. Argumentos inválidos, opção desconhecida, valor ausente e `HWND` inexistente.

O teste exige uma sessão interativa do Windows com desktop ativo e suporte a
Windows.Graphics.Capture. Não exige Edge nem conexão com o jogo para poder
repetir as verificações de pixels. A captura real do Edge e o vídeo de
30 segundos continuam documentados em [PHASE1_POC.md](../docs/PHASE1_POC.md).
Os E2E são serializados pelo CTest para não disputar a área de trabalho.

Os resultados ficam em `build/vs2022-x64/test-results/<configuração>/`:

- `unit.xml`: relatório NUnit dos testes unitários.
- `capture-core.xml`: 50 testes do núcleo de captura, em JUnit XML.
- `e2e/report.json`: resultado do E2E, encoder escolhido e diagnóstico NVENC.
- `e2e/ffprobe.json`: metadados do MP4.
- `e2e/recording.mkv` e `e2e/recording.mp4`: vídeo sintético preservado para inspeção.
- `e2e-phase1/report.json`: resultado dos nove cenários da captura nativa.
- `e2e-phase1/*.log`: códigos de saída e diagnósticos de cada operação.
- `e2e-phase1/*.png`, `*.mp4` e `*.ffprobe.json`: quadros, vídeos e metadados.

Cada processo externo tem prazo de execução. Falhas retornam código diferente
de zero e são reportadas pelo CTest; arquivos de vídeo úteis são preservados.

## Última execução

Em 01/10/2026, no Windows 10 do projeto, builds Debug e Release x64 aprovados.
`ctest --preset release` e `ctest --preset debug` passaram com 4/4 entradas cada:
50 casos de `unit.capture_core`, 14 de `unit.ffmpeg_setup` e os E2E das Fases 0
e 1. O E2E da Fase 1 aprovou os nove grupos de cenários e o da Fase 0 confirmou
o fallback real `libx264` quando NVENC não inicializou com o driver atual.

Reexecução concluída em 01/10/2026 às 09:05 (America/Sao_Paulo), sobre o commit
`abd074e`: 4/4 entradas CTest aprovadas novamente em cada configuração,
64 casos unitários e dois E2E, sem falhas. Tempos: 27,922 s em Release e
28,455 s em Debug. O resumo versionado está em
[VALIDACAO_FASE1.json](../docs/VALIDACAO_FASE1.json); os relatórios completos
incluem agora `ctest.xml` em cada pasta de configuração.
