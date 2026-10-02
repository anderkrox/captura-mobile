param(
    [Parameter(Mandatory = $true)][long] $WindowHandle,
    [ValidateSet('capture', 'baseline')][string] $Label = 'capture',
    [Parameter(Mandatory = $true)][string] $ReportPath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ValidationBrowserKeys {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
}
'@

$root = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr] $WindowHandle)
$condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'cm-content cm-lineWrapping')
$prompt = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
if (-not $prompt) { throw 'Open the existing Edge DevTools Console before measuring cadence.' }
$marker = "YOUROTS_PHASE5_$Label="
# Temporary requestAnimationFrame callbacks measure browser scheduling only.
# This does not read credentials or modify the game, DOM, storage, or account.
$javascript = @"
(() => { const t = [], start = performance.now(); requestAnimationFrame(function sample(now) {
  t.push(now); if (now - start < 10000) requestAnimationFrame(sample); else {
    const d = t.slice(1).map((n, i) => n - t[i]).sort((a, b) => a - b);
    console.log('$marker' + JSON.stringify({samples:t.length, elapsed_ms:t[t.length-1]-t[0],
      fps:(t.length-1)*1000/(t[t.length-1]-t[0]), median_ms:d[Math.floor(d.length*.5)],
      p95_ms:d[Math.floor(d.length*.95)], maximum_ms:Math.max(...d)}));
  } }); return 'Phase 5: measuring browser cadence for 10 seconds'; })()
"@
[void][ValidationBrowserKeys]::SetForegroundWindow([IntPtr] $WindowHandle)
$prompt.SetFocus()
$value = $prompt.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
if (-not [string]::IsNullOrWhiteSpace($value.Current.Value)) { throw 'Console contains an unfinished command; preserve it.' }
$value.SetValue($javascript)
[ValidationBrowserKeys]::keybd_event(13, 0, 0, [UIntPtr]::Zero)
[ValidationBrowserKeys]::keybd_event(13, 0, 2, [UIntPtr]::Zero)
$deadline = [DateTime]::UtcNow.AddSeconds(20)
$result = $null
do {
    Start-Sleep -Milliseconds 500
    $elements = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                             [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($element in $elements) {
        $name = $element.Current.Name
        if ($name.StartsWith($marker)) {
            $json = $name.Substring($marker.Length)
            try {
                $result = $json | ConvertFrom-Json
                break
            } catch { }
        }
    }
} while (-not $result -and [DateTime]::UtcNow -lt $deadline)
if (-not $result) { throw 'Browser cadence result was not exposed by Console accessibility.' }
$result | Add-Member -NotePropertyName scope -NotePropertyValue 'browser_requestAnimationFrame_scheduling_not_game_engine_fps'
$result | ConvertTo-Json | Set-Content -LiteralPath $ReportPath -Encoding UTF8
$result | ConvertTo-Json -Compress
