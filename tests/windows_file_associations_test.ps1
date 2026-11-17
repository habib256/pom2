# POM2 — GPL-3.0-or-later
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../packaging/windows/FileAssociations.ps1"
$root = 'Software\POM2-Association-Test-' + [Guid]::NewGuid().ToString('N')
$classes = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($root)
function Check($Condition, $Message) { if (!$Condition) { throw $Message } }
try {
    $key = $classes.CreateSubKey('.po')
    $key.SetValue('', 'Existing.Editor')
    $key.Dispose()
    $key = $classes.CreateSubKey('.po\OpenWithProgids')
    $key.SetValue('Other.Emulator', '')
    $key.Dispose()
    $exe = 'C:\Games with spaces\POM2\POM2.exe'
    Set-Pom2FileAssociations $classes $exe $false
    Set-Pom2FileAssociations $classes $exe $false # idempotent
    $key = $classes.OpenSubKey('POM2.DiskImage\shell\open\command')
    Check ($key.GetValue('') -ceq ('"' + $exe + '" "%1"')) 'Command quoting failed'
    $key.Dispose()
    foreach ($ext in '.dsk', '.do', '.po', '.nib', '.nb2', '.woz', '.d13', '.2mg', '.hdv', '.moof') {
        $key = $classes.OpenSubKey("$ext\OpenWithProgids")
        Check ($key.GetValueNames() -contains 'POM2.DiskImage') "Missing $ext"
        $key.Dispose()
    }
    Set-Pom2FileAssociations $classes $exe $true
    Set-Pom2FileAssociations $classes $exe $true
    Check ($null -eq $classes.OpenSubKey('POM2.DiskImage')) 'ProgID not removed'
    $key = $classes.OpenSubKey('.po')
    Check ($key.GetValue('') -ceq 'Existing.Editor') 'Default association changed'
    $key.Dispose()
    $key = $classes.OpenSubKey('.po\OpenWithProgids')
    Check ($key.GetValueNames() -contains 'Other.Emulator') 'Other handler removed'
    Check (!($key.GetValueNames() -contains 'POM2.DiskImage')) 'Handler not removed'
    $key.Dispose()
    Write-Host 'Windows file associations: passed (isolated registry, defaults preserved).'
} finally {
    $classes.Dispose()
    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($root, $false)
}
