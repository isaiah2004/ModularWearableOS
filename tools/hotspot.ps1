# Run with Windows PowerShell 5.1 for WinRT projection support.
param([switch]$Start)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null=[Windows.Networking.Connectivity.NetworkInformation,Windows.Networking.Connectivity,ContentType=WindowsRuntime]
$null=[Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager,Windows.Networking.NetworkOperators,ContentType=WindowsRuntime]
$null=[Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult,Windows.Networking.NetworkOperators,ContentType=WindowsRuntime]
function Await-Operation($Operation,$ResultType) {
    $method=[System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetGenericArguments().Count -eq 1 -and
        $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    } | Select-Object -First 1
    $task=$method.MakeGenericMethod($ResultType).Invoke($null,@($Operation))
    if(-not $task.Wait(30000)){throw 'Hotspot operation timed out'}
    $task.Result
}
$profile=[Windows.Networking.Connectivity.NetworkInformation]::GetInternetConnectionProfile()
if(-not $profile){throw 'No internet connection profile'}
$manager=[Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]::CreateFromConnectionProfile($profile)
$config=$manager.GetCurrentAccessPointConfiguration()
Write-Output "Hotspot SSID=$($config.Ssid) band=$($config.Band) state=$($manager.TetheringOperationalState) clients=$($manager.ClientCount)"
if($Start) {
    if($config.Ssid -ne 'AVY 3956'){throw 'Configured hotspot differs from the user-supplied network'}
    if($manager.TetheringOperationalState -ne 'On') {
        # ESP32-S3 supports 2.4 GHz. Preserve the existing SSID and password.
        $config.Band=1
        $operation=$manager.ConfigureAccessPointAsync($config)
        $actionMethod=[System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
            $_.Name -eq 'AsTask' -and -not $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 -and
            $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction'
        } | Select-Object -First 1
        $task=$actionMethod.Invoke($null,@($operation))
        if(-not $task.Wait(30000)){throw 'Hotspot configuration timed out'}
        $result=Await-Operation ($manager.StartTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
        Write-Output "Start status=$($result.Status) detail=$($result.AdditionalErrorMessage)"
    }
    Write-Output "Hotspot state=$($manager.TetheringOperationalState) clients=$($manager.ClientCount)"
}
