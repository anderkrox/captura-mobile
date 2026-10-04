# Testes das Fases 0 a 5

Os testes usam CTest, Python 3.10 ou superior e Pester 3.4.0, disponível neste
computador pelo Windows PowerShell. Não é necessário baixar bibliotecas de
teste. FFmpeg e FFprobe devem estar instalados em `third_party/ffmpeg/bin`.

Na raiz do projeto:

```powershell
cmake --preset vs2022-x64 -DBUILD_TESTING=ON
cmake --build --preset release
ctest --preset release
```

Para salvar também o relatório consolidado do CTest, use um caminho absoluto
(o CTest executa a partir da pasta de build):

```powershell
ctest --preset release --output-junit "$PWD/build/vs2022-x64/test-results/Release/ctest.xml"
ctest --preset debug --output-junit "$PWD/build/vs2022-x64/test-results/Debug/ctest.xml"
```

Para executar apenas uma categoria:

```powershell
ctest --preset release -L unit
ctest --preset release -L e2e
```

Também existem os presets `debug` para build e testes. Para compilar o
aplicativo sem as dependências de teste, configure com
`cmake --preset vs2022-x64 -DBUILD_TESTING=OFF`.

Para compilar somente o aplicativo em Release x64, use
`..\compilar_windows_x64.cmd` a partir desta pasta, ou
`.\compilar_windows_x64.cmd` na raiz. Esse script usa `build/windows-x64`,
separado dos presets de testes, e não compila os executáveis de validação.

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

`calibration_tests.cpp` contém 87 casos sobre o módulo `Calibration`, usado
pela interface da Fase 2:

- Parsing estrito de inteiros, incluindo sinais, espaços, sufixos e overflow.
- Validação de proporção 9:16, limites físicos, tamanho do quadro e DPI.
- Correspondência da identidade da fonte, sem depender de `HWND`.
- Ajuste da imagem à prévia, letterbox, bordas exclusivas e coordenadas negativas.
- Conversão entre pixels da prévia e da textura, projeção do recorte e arraste
  nos quatro sentidos, incluindo limites e ponteiros extremos.
- Persistência Unicode, aspas, espaços, títulos longos, campos ausentes,
  valores corrompidos e dimensões inválidas.
- Substituição do INI, preservação do arquivo anterior em entradas inválidas
  e remoção do temporário quando a escrita falha.

O executável `CalibrationTests` usa uma pasta temporária própria e não usa
GPU, navegador nem FFmpeg. O relatório JUnit fica em `calibration.xml`.

`setup-ffmpeg.Tests.ps1` contém 14 casos sobre funções usadas pelo instalador:

- SHA-256 correto, arquivo adulterado, ausente e checksum malformado.
- Estrutura do pacote, pastas ambíguas, ausência de FFprobe/licença e binário vazio.
- Rejeição de pacote incompleto antes de criar o destino e cópia fiel dos arquivos.
- Preferência por NVENC, fallback para CPU com diagnóstico e erro quando CPU falha.

Esses casos usam arquivos pequenos no `TestDrive` do Pester e mocks dos probes.
Não baixam pacotes nem executam os encoders reais.

`recorder_tests.cpp` contém 84 casos sobre `RecorderCore`, usado pelo gravador:

- Preset de 30 FPS, capacidade de 1 a 32 posições e destino MP4.
- Recorte BGRA com verificação de canais, linhas, bordas, truncamento e overflow.
- Argumentos CPU/NVENC e remultiplexação, conferidos com o parser nativo do Windows,
  incluindo caminhos Unicode com espaços, vídeo sem áudio e `faststart`.
- Metadados obrigatórios do FFprobe, quantidade de quadros, taxa nominal/média,
  duração finita, parsing completo, campos duplicados e ausência de áudio.
- Limite da fila, descarte do quadro mais antigo e liberação da memória.
- Preenchimento de lacunas com o último quadro, cena estática, descarte inicial,
  pacotes fora de ordem e erro de escrita sem contar um quadro incompleto.

`RecorderTests` não usa GPU nem processos externos. O relatório JUnit fica em
`recorder.xml`. A cadência usa o relógio real no E2E; os unitários conferem
as posições da linha do tempo sem esperas que dependam do escalonador.

`application_controls_tests.cpp` contém 87 casos sobre as regras de produção
extraídas para `ApplicationControls` e sobre as verificações de recuperação:

- Parsing e canonicalização de atalhos, F1–F24, aliases e duplicidades.
- Estados, habilitação dos controles, fonte indisponível e calibração inválida.
- Contador de duração, viradas de minuto/hora e rejeição de NaN/infinito.
- Preferências UTF-16/Unicode, substituição, defaults, corrupção e falha de escrita.
- Nomes determinísticos e colisões com MP4/MKV, preservando os arquivos anteriores.
- Quantidade de quadros recuperáveis, sinais, overflow e respostas ambíguas.
- Limiares de espaço de 64 MiB para iniciar e 32 MiB para continuar.

`ApplicationControlsTests` não usa GPU nem processos externos. O relatório
JUnit fica em `application-controls.xml`.

`ui_rendering_tests.cpp` contém 27 casos sobre a correção das piscadas da interface:

- Composição em memória sem apagar ou expor quadros parciais na tela.
- Apresentação conjunta do fundo, imagem e contorno, sem resíduos do quadro anterior.
- Coordenadas, regiões de atualização, reutilização e redimensionamento do bitmap.
- Restauração do estado do DC, pintura interrompida, limites e liberação de recursos GDI.
- Contagem real de mensagens `WM_SETTEXT` e `WM_ENABLE`: atualizações repetidas
  não repintam controles, mas mudanças de texto, duração e habilitação são aplicadas.
- Textos vazios, Unicode, textos longos, alterações externas e controles inválidos.

`UiRenderingTests` usa bitmaps em memória e controles Win32 ocultos, sem GPU,
captura ou FFmpeg. O relatório JUnit fica em `ui-rendering.xml`.

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

## E2E da Fase 2

`e2e_phase2.py` opera os controles reais de `YourotsCapture` por mensagens
Win32 e captura `CaptureFixture` por Windows.Graphics.Capture / Direct3D 11.
O teste salva screenshots BMP da interface e compara os quatro cantos
coloridos da prévia, antes e depois de mover a fonte e restaurar a calibração.
Antes de cada screenshot, verifica que `WM_ERASEBKGND` mantém o bitmap intacto,
para evitar a regressão que apagava o quadro antes de a pintura estar pronta.

São 19 grupos de cenários: inicialização e DPI por monitor; seleção e prévia;
recorte físico explícito; entradas inválidas; ajustes de posição/tamanho;
arraste e letterbox; movimento da fonte; persistência Unicode sem `HWND`;
restauração; redimensionamento com confirmação obrigatória; redução do quadro
e recorte menor; minimização/restauração; fechamento e seleção expirada;
fonte ausente; nova instância da fonte; identidade ambígua; divergência de DPI;
divergência de tamanho persistido; INI corrompido e falha real de escrita.

O `LOCALAPPDATA` dos E2E das Fases 0 e 2 aponta para pastas temporárias
isoladas. As preferências pessoais do usuário não são lidas nem alteradas.
Todos os processos criados pelo E2E são encerrados, inclusive em falhas.

O E2E verifica o contexto DPI `PER_MONITOR_AWARE_V2` real e compara uma
calibração com DPI salvo diferente do DPI real da fonte. Não modifica a escala
dos monitores. A passagem física entre escalas de 100%, 125% e 150% e as
mudanças internas de zoom/layout do DevTools continuam na Fase 5.

O recebimento de quadros ocorre na thread interna do pool, conforme a
[documentação de CreateFreeThreaded](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded).
A inicialização WinRT do aplicativo usa MTA para permitir a recriação dos
buffers nessa thread; o E2E verifica crescimento e redução reais da fonte.

## E2E da Fase 3

`e2e_phase3.py` executa o módulo `Recorder` de produção por
`YourotsCapturePhase3` e `RecorderLifecycleE2E`, com captura real de
`CaptureFixture` por Windows.Graphics.Capture / Direct3D 11.

São 21 grupos de cenários: gravação com movimento e cena estática; diretórios e caminhos
Unicode; calibração salva, DPI, tamanho e identidade ambígua; argumentos,
recortes e dependências inválidos; falha ao criar o MP4; fechamento e
redimensionamento da fonte, inclusive quando o recorte ainda cabe no quadro.

O executável de apoio verifica dois ciclos no mesmo gravador, início e parada
duplicados, nova tentativa após falha de inicialização, recuperação de erro do
encoder e destruição durante a gravação. `MediaToolFixture` força falhas dos
probes, saída extensa, atraso de leitura para transbordar a fila, falha do
encoder/remux/FFprobe e presença de áudio/duração NaN. A codificação dos
vídeos aprovados continua usando FFmpeg real; o fallback CPU é exercitado
mesmo em máquinas com NVENC disponível.

O FFprobe e a decodificação completa conferem resolução, H.264, `yuv420p`,
BT.709, SAR, ausência de áudio, quantidade de quadros e duração. Os timestamps
PTS/DTS e a duração dos pacotes são conferidos na cadência exata de 30 FPS.
Os pixels dos quatro cantos e do marcador conferem recorte e movimento; o
átomo `moov` deve preceder `mdat`. Os hashes dos pacotes H.264 antes/depois do
remux também são comparados para confirmar ausência de recodificação.

A conversão de timestamps do MKV para a grade de 30 FPS usa o filtro
[`setts` do FFmpeg](https://ffmpeg.org/ffmpeg-bitstream-filters.html#setts),
com timebase 1/15360 e 512 ticks por quadro, preservando a ordem dos B-frames.
O MKV só é removido após validação; falhas preservam o temporário. Cada processo
do gravador pertence a um Job Object do Windows que encerra também seus filhos
ao sair do teste. O `LOCALAPPDATA` aponta para uma pasta temporária isolada.

Para executar somente esta fase:

```powershell
ctest --preset release -R 'unit.recorder|e2e.phase3'
ctest --preset debug -R 'unit.recorder|e2e.phase3'
```

É necessária uma sessão interativa do Windows com desktop ativo. Os E2E são
serializados pelo CTest; execute as configurações uma após a outra. Não é
necessário abrir o Edge ou o jogo. A inicialização NVENC real é tentada, mas
sua codificação só pode ser validada quando o driver permite o probe.

## E2E da Fase 4

`e2e_phase4.py` opera a interface de produção, os botões e atalhos globais
registrados pelo Windows. Usa uma cópia temporária do aplicativo com proxies
de teste de FFmpeg/FFprobe; captura WGC/D3D11 e codificação `libx264` são reais.
O `LOCALAPPDATA` e as preferências ficam isolados, e Job Objects encerram os
processos criados, inclusive os encoders. As gravações e screenshots ficam
na pasta de artefatos do CTest.

O teste confere início condicionado à calibração, estados e controles bloqueados;
pausas repetidas com contador congelado e intervalos ausentes do MP4; entrada
inválida, duplicidade, conflito externo e restauração dos atalhos anteriores;
eventos reais de teclado; persistência e liberação de registros ao reiniciar;
minimização/restauração e redimensionamento; recusa/confirmação do fechamento;
falhas de encoder/remux/probe/áudio/metadados; MKV preservado, tentativa de
recuperação recusada e recuperação validada; destino inválido, negação real de
permissão por ACL temporária e fechamento da fonte durante uma pausa.

FFprobe, timestamps dos pacotes, decodificação completa e pixels dos quatro
cantos conferem MP4 H.264, 1080 × 1920, 30 FPS, BT.709, `yuv420p`, `faststart`
e ausência de áudio. Hashes verificam que falhas e recuperação preservam os
arquivos anteriores. A ACL de teste é removida antes da limpeza.

Uma colisão criada após iniciar a gravação também é exercitada: o FFmpeg usa
`-n`, recusa sobrescrever o MP4 existente e mantém o MKV para recuperar com
outro nome. Se o encoder morrer antes de descarregar seus buffers, o temporário
pode estar vazio; nesse caso a recuperação informa o erro e mantém o arquivo.

O E2E de calibração também verifica a restauração automática com uma nova
instância da fonte. No Windows 10 observado, `CreateForWindow` recusou a primeira
ativação de uma janela válida com `E_INVALIDARG`; a captura agora faz até quatro
novas tentativas com intervalos de 100, 200, 400 e 800 ms enquanto a fonte permanece visível e disponível.
Erros definitivos incluem a operação e o HRESULT. Os testes esperam a
inicialização das janelas e a apresentação do DWM antes de automatizar a captura.
O encerramento da captura revoga eventos e fecha o pool sem manter o mutex dos
callbacks; uma barreira aguarda o processamento em andamento, evitando bloqueio
com callbacks pendentes. Os ciclos de captura e casos de calibração inválida
exercitam esse encerramento.
As screenshots usam `PrintWindow`/`WM_PRINTCLIENT` com a mesma rotina de pintura
da prévia, para obter um quadro completo e evitar ler a tela entre a limpeza do
fundo e o desenho da imagem.

Os limiares de espaço são exercitados por unitários; o E2E não enche o disco.
O seletor de pasta e a abertura do Explorer ainda exigem inspeção manual.
Mudanças físicas de DPI, DevTools e o jogo continuam na Fase 5.

```powershell
ctest --preset release -R 'unit.application_controls|e2e.phase4'
ctest --preset debug -R 'unit.application_controls|e2e.phase4'
```

Os resultados ficam em `build/vs2022-x64/test-results/<configuração>/`:

- `unit.xml`: relatório NUnit dos testes unitários.
- `capture-core.xml`: 50 testes do núcleo de captura, em JUnit XML.
- `calibration.xml`: 87 testes de calibração, em JUnit XML.
- `recorder.xml`: 84 testes do núcleo de gravação, em JUnit XML.
- `application-controls.xml`: 87 testes dos controles e regras de falha, em JUnit XML.
- `e2e/report.json`: resultado do E2E, encoder escolhido e diagnóstico NVENC.
- `e2e/ffprobe.json`: metadados do MP4.
- `e2e/recording.mkv` e `e2e/recording.mp4`: vídeo sintético preservado para inspeção.
- `e2e-phase1/report.json`: resultado dos nove cenários da captura nativa.
- `e2e-phase1/*.log`: códigos de saída e diagnósticos de cada operação.
- `e2e-phase1/*.png`, `*.mp4` e `*.ffprobe.json`: quadros, vídeos e metadados.
- `e2e-phase2/report.json`: resultado dos 19 grupos da interface/calibração.
- `e2e-phase2/preview-*.bmp`: screenshots da prévia real conferidos por pixels.
- `e2e-phase3/report.json`: resultado dos grupos da gravação/exportação.
- `e2e-phase3/*.log`, `*.mp4`, `*.ffprobe.json`: diagnósticos, vídeos e metadados.
- `e2e-phase3/*.recording.mkv`: temporários preservados nos testes de falha/destruição.
- `e2e-phase4/report.json`: cenários dos controles/atalhos/falhas e duração sem pausas.
- `e2e-phase4/**/*.mp4`, `*.ffprobe.json` e `preview-phase4.bmp`: vídeos, metadados e prévia.
- `e2e-phase4/**/*.recording.mkv`: temporários preservados ao interromper o encoder.
- `ctest.xml`: resultado consolidado, quando usado `--output-junit`.

Cada processo externo tem prazo de execução. Falhas retornam código diferente
de zero e são reportadas pelo CTest; arquivos de vídeo úteis são preservados.

## Fase 5: ambiente real e estabilidade

`unit.phase5_validation` acrescenta 37 casos de aceitação: rejeição de áudio,
resolução/proporção/FPS incorretos, duração acelerada ou truncada, valores não
finitos, borda inferior perdida, memória que cresce e pacotes que param de
avançar. O relatório JUnit é `phase5-validation.xml`.

`e2e.phase5`, incluído no CTest, faz 12 gravações pela interface de produção:
quatro estáticas, quatro com movimento e quatro com uma fonte Win32 com muitas
animações a aproximadamente 60 Hz. Confere cantos, movimento, decodificação,
MP4/H.264/1080 × 1920/30 FPS/BT.709, duração sem pausas, nomes distintos e
recursos após ciclos no mesmo aplicativo. Com dois monitores na mesma escala,
move a fonte entre eles e verifica a prévia e o recorte durante a gravação.
As configurações permanecem isoladas; a fonte animada não representa combate
real no jogo.

```powershell
ctest --preset release -R 'unit.phase5_validation|e2e.phase5'
ctest --preset debug -R 'unit.phase5_validation|e2e.phase5'
```

Os testes abaixo são explícitos, porque usam o navegador aberto ou alteram
temporariamente a escala física do primeiro monitor. Não são executados por
`ctest`. A validação de estabilidade usa ciclos curtos; não há requisito de
gravação contínua de 30 minutos. Para identificar o `HWND` atual e salvar a
referência, use a POC; confirme visualmente o recorte antes da gravação:

```powershell
.\build\vs2022-x64\Release\YourotsCapturePoc.exe --list-windows
# Exemplo: substitua o HWND e o recorte pelos valores conferidos na sua janela.
$sourceHandle = 263732
.\build\vs2022-x64\Release\YourotsCapturePoc.exe --hwnd $sourceHandle `
  --crop '410,147,486,864' --snapshot artifacts/phase5/mobile-reference.png
```

Para observar um encoder já iniciado, obtenha seu PID com `Get-Process
ffmpeg` e execute `observe_phase5_resources.py --pid <PID> --artifacts <pasta>`.
O observador encerra quando esse processo termina e registra seu intervalo de
observação; não inicia gravações. A análise de tendência exige dois minutos
de aquecimento e pelo menos um minuto de amostras adicionais; intervalos
menores são insuficientes para essa análise. Nos ciclos curtos, o E2E compara
os recursos liberados ao final de cada sessão. A idade de pacotes legíveis em
um MKV inclui buffers do [FFmpeg](https://ffmpeg.org/ffmpeg-formats.html#matroska)
e não mede a latência do encoder. As medições de agendamento de
`requestAnimationFrame` do navegador são separadas do FPS do motor do jogo:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/measure-browser-cadence.ps1 `
  -WindowHandle $sourceHandle -Label capture -ReportPath artifacts/phase5/browser-cadence-capture.json
```

`e2e_phase5_dpi.py` usa a tela Configurações do Windows para testar 100%, 125%
e 150% no primeiro monitor. Confere os DPI reais 96/120/144, captura e vídeos,
mudanças entre monitores com DPI diferentes e a recalibração obrigatória,
restaurando a escala original em `finally`. Usa uma fonte Win32 controlada.
Exige o arranjo observado de dois monitores e o primeiro como principal.

```powershell
python tests/e2e_phase5_dpi.py --app build/vs2022-x64/Release/YourotsCapture.exe `
  --fixture build/vs2022-x64/Release/CaptureFixture.exe `
  --ffmpeg third_party/ffmpeg/bin/ffmpeg.exe --ffprobe third_party/ffmpeg/bin/ffprobe.exe `
  --scale-helper tests/set-validation-display-scale.ps1 --artifacts artifacts/phase5/physical-dpi
```

`e2e_phase5_browser.py` testa pausas, movimentação, minimização/restauração e
redimensionamento na janela do jogo. Restaura sua posição original. Para
fechamento e recuperação, cria e fecha somente uma nova janela `about:blank`
do Edge e recupera seu MKV com os binários reais:

```powershell
python tests/e2e_phase5_browser.py --app build/vs2022-x64/Release/YourotsCapture.exe `
  --edge 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe' `
  --ffmpeg third_party/ffmpeg/bin/ffmpeg.exe --ffprobe third_party/ffmpeg/bin/ffprobe.exe `
  --hwnd $sourceHandle --crop '410,147,486,864' `
  --reference artifacts/phase5/mobile-reference.png --artifacts artifacts/phase5/browser
```

Os artefatos grandes permanecem locais em `artifacts/phase5/` e
`build/vs2022-x64/test-results/<configuração>/e2e-phase5/`. O resumo versionado
e os limites reais de cobertura ficam em `docs/VALIDACAO_FASE5.json`.

## Última execução da Fase 5

Em 02/10/2026, os builds Release e Debug passaram nas 12 entradas CTest, com
366 casos unitários e os E2E das Fases 0 a 5. O novo E2E aprovou 12 gravações
curtas por configuração. No ambiente real, os seis grupos do navegador e as
três escalas físicas de DPI passaram; a escala original foi restaurada.
Foram corrigidos o timeout da verificação completa do FFprobe e a captura
WGC após restaurar o Edge. Os testes não mantêm processos ativos ao terminar.

A exigência de gravação contínua de 30 minutos foi removida a pedido do
usuário. Os artefatos do ensaio inicial e de sua recuperação foram preservados
como histórico; a repetição cancelada não conta como aprovação. Combate
intenso no jogo e fidelidade do jogo em 125%/150% permanecem fora da cobertura
confirmada. A matriz de DPI usa uma fonte controlada, e o encoder validado
foi `libx264`, pois o driver disponível não passou no teste NVENC.

## Última execução da Fase 4

Em 01/10/2026 às 18:35 (America/Sao_Paulo), builds Release e Debug x64
aprovados. A suíte completa passou com 10/10 entradas CTest em cada
configuração: 50 casos de captura, 87 de calibração, 84 de gravação, 87 dos
controles e 14 de dependências (322 unitários), mais os E2E das Fases 0 a 4.
Os 20 grupos da Fase 4 passaram nas duas configurações. Soma dos tempos:
169,492 s em Release e 171,630 s em Debug. Nenhum processo de validação
permaneceu em execução.

O resumo versionado, incluindo hashes dos fontes e executáveis, está em
[VALIDACAO_FASE4.json](../docs/VALIDACAO_FASE4.json). Relatórios completos,
screenshots, vídeos e temporários ficam nas pastas de artefatos acima. Os E2E
da Fase 4 usam `libx264`; a validação do jogo e demais limites descritos acima
permanecem na Fase 5.

## Histórico da Fase 3

Em 01/10/2026 às 15:33 (America/Sao_Paulo), builds Release e Debug
x64 aprovados. A suíte completa passou com 8/8 entradas CTest em cada
configuração: 50 casos de captura, 87 de calibração, 84 de gravação e 14 de
dependências (235 unitários), mais os E2E das Fases 0, 1, 2 e 3. Os 21 grupos
da Fase 3 passaram nas duas configurações, incluindo pressão real da fila,
reinício/recuperação do gravador, cadência exata e preservação do MKV em falhas.
Soma dos tempos dos testes: 86,832 s em Release e 86,752 s em Debug.
Nenhum processo de validação permaneceu em execução.

O resumo versionado e os hashes dos fontes validados estão em
[VALIDACAO_FASE3.json](../docs/VALIDACAO_FASE3.json). Os relatórios completos e
vídeos ficam nas pastas de artefatos indicadas acima. O encoder real usado foi
`libx264`; a codificação NVENC depende de um driver que passe no probe.

## Histórico da Fase 2

Em 01/10/2026 às 14:32 (America/Sao_Paulo), builds Release e Debug x64
aprovados. A suíte completa passou com 6/6 entradas CTest em cada
configuração: 50 casos de captura, 87 de calibração, 14 de dependências
(151 unitários) e os E2E das Fases 0, 1 e 2. Os 19 grupos da Fase 2 passaram
nas duas configurações. Soma dos tempos dos testes: 39,933 s em Release e
36,588 s em Debug. Nenhum processo de teste permaneceu em execução.

O resumo versionado, incluindo os hashes dos fontes validados, está em
[VALIDACAO_FASE2.json](../docs/VALIDACAO_FASE2.json). Os relatórios completos
e screenshots estão nas pastas de artefatos indicadas acima. NVENC continua
indisponível no driver atual; o fallback real `libx264` foi aprovado nos E2E
de vídeo.

## Histórico da Fase 1

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
