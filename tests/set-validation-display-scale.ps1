param(
    [ValidateSet(100, 125, 150)]
    [int] $Percent,
    [switch] $ReadOnly
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ValidationDisplayPointer {
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X; public int Y; }
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
}
'@

Start-Process 'ms-settings:display' -WindowStyle Hidden
$deadline = [DateTime]::UtcNow.AddSeconds(10)
$root = $null
do {
    $settingsProcess = Get-Process SystemSettings, ApplicationFrameHost -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle -match 'Configura|Settings' } |
        Select-Object -First 1
    if ($settingsProcess) {
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($settingsProcess.MainWindowHandle)
    }
    if (-not $root) { Start-Sleep -Milliseconds 100 }
} while (-not $root -and [DateTime]::UtcNow -lt $deadline)
if (-not $root) { throw 'Windows display Settings did not open.' }
[void][ValidationDisplayPointer]::ShowWindow($settingsProcess.MainWindowHandle, 9)
[void][ValidationDisplayPointer]::SetForegroundWindow($settingsProcess.MainWindowHandle)

function Find-DisplayControl([string] $Id) {
    $condition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    $element = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
    if (-not $element) { throw "Display control missing: $Id" }
    return $element
}

# Select the first physical display through Settings, using its accessible bounds.
# The caller verifies actual GetDpiForWindow values and restores the original scale.
$primaryControl = Find-DisplayControl 'SystemSettings_Display_MainMonitor_CheckBox'
if ($primaryControl.Current.IsEnabled) {
    $firstDisplay = Find-DisplayControl 'Display1'
    $scrollItem = $firstDisplay.GetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern)
    $scrollItem.ScrollIntoView()
    Start-Sleep -Milliseconds 100
    $point = $firstDisplay.GetClickablePoint()
    $oldPointer = New-Object ValidationDisplayPointer+Point
    [void][ValidationDisplayPointer]::GetCursorPos([ref] $oldPointer)
    try {
        [void][ValidationDisplayPointer]::SetCursorPos([int] $point.X, [int] $point.Y)
        [ValidationDisplayPointer]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
        [ValidationDisplayPointer]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    } finally {
        [void][ValidationDisplayPointer]::SetCursorPos($oldPointer.X, $oldPointer.Y)
    }
    Start-Sleep -Milliseconds 200
    $primaryControl = Find-DisplayControl 'SystemSettings_Display_MainMonitor_CheckBox'
    if ($primaryControl.Current.IsEnabled) { throw 'Display1 is not the primary display; refuse to alter another monitor.' }
}

$combo = Find-DisplayControl 'SystemSettings_Display_Scaling_ItemSizeOverride_ComboBox'
$selection = $combo.GetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern)
$original = $selection.Current.GetSelection()[0].Current.Name
if ($ReadOnly) {
    [PSCustomObject]@{ display = 1; selected = $original } | ConvertTo-Json -Compress
    exit 0
}
if (-not $Percent) { throw 'Supply -Percent or -ReadOnly.' }
$expansion = $combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
$expansion.Expand()
Start-Sleep -Milliseconds 150
$items = $combo.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                       [System.Windows.Automation.Condition]::TrueCondition)
$chosen = $false
foreach ($item in $items) {
    if ($item.Current.ControlType -eq [System.Windows.Automation.ControlType]::ListItem -and
        $item.Current.Name -match "^$Percent%") {
        $itemSelection = $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
        $itemSelection.Select()
        $chosen = $true
        break
    }
}
if (-not $chosen) {
    $expansion.Collapse()
    throw "Scale $Percent% is not available on this physical display."
}
Start-Sleep -Milliseconds 700
$combo = Find-DisplayControl 'SystemSettings_Display_Scaling_ItemSizeOverride_ComboBox'
$selection = $combo.GetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern)
$current = $selection.Current.GetSelection()[0].Current.Name
if ($current -notmatch "^$Percent%") { throw "Settings did not apply $Percent%: $current" }
[PSCustomObject]@{ display = 1; original = $original; selected = $current } | ConvertTo-Json -Compress
