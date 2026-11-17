# POM2 — GPL-3.0-or-later
# Run from the extracted package. No administrator rights are needed.
# https://learn.microsoft.com/en-us/windows/win32/shell/how-to-include-an-application-on-the-open-with-dialog-box
param([switch]$Remove)
$ErrorActionPreference = 'Stop'

function Set-Pom2FileAssociations($Classes, [string]$Executable, [bool]$Unregister) {
    $progId = 'POM2.DiskImage'
    $extensions = '.dsk', '.do', '.po', '.nib', '.nb2', '.woz', '.d13', '.2mg', '.hdv', '.moof'
    if ($Unregister) {
        foreach ($extension in $extensions) {
            $key = $Classes.OpenSubKey("$extension\OpenWithProgids", $true)
            if ($null -ne $key) {
                try { $key.DeleteValue($progId, $false) } finally { $key.Dispose() }
            }
        }
        $Classes.DeleteSubKeyTree($progId, $false)
        return
    }
    $key = $Classes.CreateSubKey($progId)
    try {
        $key.SetValue('', 'Apple II disk image')
        # Never take over a public extension when Windows chooses a default.
        $key.SetValue('AllowSilentDefaultTakeOver', '')
        $icon = $key.CreateSubKey('DefaultIcon')
        try { $icon.SetValue('', '"' + $Executable + '",0') } finally { $icon.Dispose() }
        $command = $key.CreateSubKey('shell\open\command')
        try { $command.SetValue('', '"' + $Executable + '" "%1"') } finally { $command.Dispose() }
    } finally { $key.Dispose() }
    foreach ($extension in $extensions) {
        $key = $Classes.CreateSubKey("$extension\OpenWithProgids")
        try { $key.SetValue($progId, '') } finally { $key.Dispose() }
    }
}

# Dot-sourcing exposes the operation for an isolated-registry test.
if ($MyInvocation.InvocationName -eq '.') { return }
$executable = Join-Path $PSScriptRoot 'POM2.exe'
if (!$Remove -and !(Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw 'Extract the complete package before registering POM2.'
}
$classes = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Classes')
try { Set-Pom2FileAssociations $classes $executable $Remove.IsPresent }
finally { $classes.Dispose() }
Add-Type -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shell32.dll")]
public static extern void SHChangeNotify(uint eventId, uint flags, System.IntPtr item1, System.IntPtr item2);
'@ -Name Shell -Namespace Pom2
[Pom2.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
if ($Remove) { Write-Host 'POM2 removed from Open with.' }
else { Write-Host 'POM2 added to Open with. Choose the default app in Windows Settings if desired.' }
