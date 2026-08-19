param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [int]$Baud = 115200,
    [int]$TimeoutSeconds = 300,
    [string]$Until = "login:"
)

$serial = [System.IO.Ports.SerialPort]::new(
    $Port,
    $Baud,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
$serial.ReadTimeout = 200
$recent = [Text.StringBuilder]::new()
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

try {
    $serial.Open()

    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $chunk = $serial.ReadExisting()
        if ($chunk.Length -eq 0) {
            continue
        }

        [void]$recent.Append($chunk)
        [Console]::Out.Write($chunk)
        [Console]::Out.Flush()

        if ($Until -and $recent.ToString().Contains($Until)) {
            exit 0
        }

        if ($recent.Length -gt 65536) {
            [void]$recent.Remove(0, $recent.Length - 32768)
        }
    }

    exit 2
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
}
