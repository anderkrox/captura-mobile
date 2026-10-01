# Testes da Fase 0

Os testes usam CTest, Python 3.10 ou superior e Pester 3.4.0, disponível neste
computador pelo Windows PowerShell. Não é necessário baixar bibliotecas de
teste. FFmpeg e FFprobe devem estar instalados em `third_party/ffmpeg/bin`.

Na raiz do projeto:

```powershell
cmake --preset vs2022-x64
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

`setup-ffmpeg.Tests.ps1` contém 14 casos sobre funções usadas pelo instalador:

- SHA-256 correto, arquivo adulterado, ausente e checksum malformado.
- Estrutura do pacote, pastas ambíguas, ausência de FFprobe/licença e binário vazio.
- Rejeição de pacote incompleto antes de criar o destino e cópia fiel dos arquivos.
- Preferência por NVENC, fallback para CPU com diagnóstico e erro quando CPU falha.

Esses casos usam arquivos pequenos no `TestDrive` do Pester e mocks dos probes.
Não baixam pacotes nem executam os encoders reais.

## E2E

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

O aplicativo ainda contém somente a inicialização da janela. O cenário E2E
verifica essa inicialização e o fluxo externo de vídeo com imagens sintéticas.
A captura do Edge, a calibração e os controles de gravação precisam de testes
quando suas respectivas fases forem implementadas.

Os resultados ficam em `build/vs2022-x64/test-results/<configuração>/`:

- `unit.xml`: relatório NUnit dos testes unitários.
- `e2e/report.json`: resultado do E2E, encoder escolhido e diagnóstico NVENC.
- `e2e/ffprobe.json`: metadados do MP4.
- `e2e/recording.mkv` e `e2e/recording.mp4`: vídeo sintético preservado para inspeção.

Cada processo externo tem prazo de execução. Falhas retornam código diferente
de zero e são reportadas pelo CTest; arquivos de vídeo úteis são preservados.
