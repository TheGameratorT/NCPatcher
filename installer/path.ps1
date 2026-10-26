# Adds a directory to the machine PATH, or removes it. Run by the installer and
# the uninstaller, which carry their own copy.
#
# This is not done in NSIS itself because a stock NSIS build holds strings of at
# most 1024 characters, and a PATH longer than that would be read truncated and
# written back that way, losing every entry past the cut.

param(
    [Parameter(Mandatory)][ValidateSet('Add', 'Remove')][string]$Action,
    [Parameter(Mandatory)][string]$Dir
)

$ErrorActionPreference = 'Stop'

$key = Get-Item 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment'

# Read unexpanded, so that entries such as %SystemRoot%\system32 are written
# back as references rather than as whatever they expand to today.
$path = $key.GetValue('Path', '', 'DoNotExpandEnvironmentNames')
$entries = @($path -split ';' | Where-Object { $_ -ne '' })

# -ne compares without regard to case, which is how Windows compares paths. A
# trailing backslash does not make it a different directory either.
$target = $Dir.TrimEnd('\')
$others = @($entries | Where-Object { $_.TrimEnd('\') -ne $target })

if ($Action -eq 'Add') {
    if ($others.Count -ne $entries.Count) { exit 0 }
    $entries += $Dir
} else {
    if ($others.Count -eq $entries.Count) { exit 0 }
    $entries = $others
}

Set-ItemProperty -LiteralPath $key.PSPath -Name 'Path' -Value ($entries -join ';') -Type ExpandString
