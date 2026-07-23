<#
.SYNOPSIS
    Connects to the EhZ device's raw debug TCP port and prints every byte
    received from the meter as hex, live. Useful for verifying wiring/baud
    rate and inspecting the exact SML byte stream independent of the parser.

.PARAMETER DeviceIp
    IP address of the EhZ device (see /debug page or your router's client list).

.PARAMETER Port
    Raw TCP debug port on the device. Defaults to 8266.

.PARAMETER LogFile
    Optional path to also append the hex output to a file.

.EXAMPLE
    .\raw_monitor.ps1 -DeviceIp 192.168.1.50
    .\raw_monitor.ps1 -DeviceIp 192.168.1.50 -LogFile capture.txt
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$DeviceIp,

    [int]$Port = 8266,

    [string]$LogFile = ""
)

$client = New-Object System.Net.Sockets.TcpClient
Write-Host "Connecting to $DeviceIp`:$Port ..."
$client.Connect($DeviceIp, $Port)
$stream = $client.GetStream()
$buffer = New-Object byte[] 1024

Write-Host "Connected. Streaming raw meter bytes as hex - press Ctrl+C to stop."

try {
    while ($client.Connected) {
        $read = $stream.Read($buffer, 0, $buffer.Length)
        if ($read -eq 0) {
            Write-Host "Connection closed by device."
            break
        }
        $hex = ($buffer[0..($read - 1)] | ForEach-Object { $_.ToString("X2") }) -join ' '
        Write-Host $hex
        if ($LogFile -ne "") {
            Add-Content -Path $LogFile -Value $hex
        }
    }
}
finally {
    $stream.Close()
    $client.Close()
}
