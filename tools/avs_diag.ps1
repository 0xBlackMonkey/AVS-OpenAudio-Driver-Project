# SPDX-License-Identifier: GPL-2.0-only
# avs_diag.ps1 -- what Windows made of the avswin subdevices.
# Run as Administrator:
#   powershell -ExecutionPolicy Bypass -File avs_diag.ps1 [-OutFile path]
# Writes avs_diag.txt next to the script unless -OutFile is given.
param([string]$OutFile)

$out = if ($OutFile) { $OutFile } else { Join-Path $PSScriptRoot 'avs_diag.txt' }
$log = New-Object System.Collections.Generic.List[string]
function W($s) { $log.Add([string]$s) }

# 1. KS device interfaces registered by PcRegisterSubdevice (Wave0/Topo0/...)
$cats = [ordered]@{
    'AUDIO'    = '{6994ad04-93ef-11d0-a3cc-00a0c9223196}'
    'RENDER'   = '{65e8773e-8f56-11d0-a3b9-00a0c9223196}'
    'CAPTURE'  = '{65e8773d-8f56-11d0-a3b9-00a0c9223196}'
    'REALTIME' = '{eb115ffc-10c8-4964-831d-6dcb02e6f23f}'
    'TOPOLOGY' = '{dda54a40-1e4c-11d1-a050-405705c10000}'
}
foreach ($c in $cats.Keys) {
    W "== KSCATEGORY_$c $($cats[$c])"
    $base = "HKLM:\SYSTEM\CurrentControlSet\Control\DeviceClasses\$($cats[$c])"
    if (-not (Test-Path $base)) { W '  (class key missing)'; continue }
    $found = $false
    Get-ChildItem $base | Where-Object { $_.PSChildName -match 'ADSP' } | ForEach-Object {
        $found = $true
        W "  $($_.PSChildName)"
        Get-ChildItem -LiteralPath $_.PSPath | Where-Object { $_.PSChildName -like '#*' } | ForEach-Object {
            $linked = (Get-ItemProperty -LiteralPath (Join-Path $_.PSPath 'Control') -Name Linked -ErrorAction SilentlyContinue).Linked
            $dp = Get-ItemProperty -LiteralPath (Join-Path $_.PSPath 'Device Parameters') -ErrorAction SilentlyContinue
            W "    ref=$($_.PSChildName) Linked=$linked FriendlyName=$($dp.FriendlyName) CLSID=$($dp.CLSID)"
        }
    }
    if (-not $found) { W '  (no interface of the ADSP device in this category)' }
}

# 2. Audio endpoints the endpoint builder created (any state)
foreach ($flow in 'Render', 'Capture') {
    W "== MMDevices $flow"
    $k = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\$flow"
    if (-not (Test-Path $k)) { W '  (none)'; continue }
    Get-ChildItem $k -ErrorAction SilentlyContinue | ForEach-Object {
        $st = (Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).DeviceState
        $p = Get-ItemProperty (Join-Path $_.PSPath 'Properties') -ErrorAction SilentlyContinue
        $name = $p.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
        $dev  = $p.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
        W "  $($_.PSChildName) DeviceState=$st name='$name' device='$dev'"
    }
}

# 3. The PnP device itself
W '== PnP device'
Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'ADSP' } | ForEach-Object {
    W "  $($_.InstanceId) Status=$($_.Status) Class=$($_.Class) Name='$($_.FriendlyName)'"
    foreach ($key in 'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_Service', 'DEVPKEY_Device_ProblemCode') {
        $v = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName $key -ErrorAction SilentlyContinue).Data
        W "    $key = $v"
    }
}

# 4. Audio services
W '== Services'
Get-Service AudioEndpointBuilder, Audiosrv -ErrorAction SilentlyContinue | ForEach-Object {
    W "  $($_.Name): $($_.Status)"
}

# 5. Recent System-log events from the audio stack / PnP
W '== System events (last 3 h)'
Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddHours(-3) } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'Audio|Kernel-PnP|portcls|avswin' } |
    Select-Object -First 40 | ForEach-Object {
        W "  [$($_.TimeCreated)] $($_.ProviderName) id=$($_.Id): $($_.Message -replace '\s+', ' ')"
    }

$log | Out-File -FilePath $out -Encoding utf8
Write-Host "written: $out"
