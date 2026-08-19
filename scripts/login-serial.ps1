param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [Parameter(Mandatory = $true)]
    [string]$Username,
    [Parameter(Mandatory = $true)]
    [string]$Password,
    [int]$Baud = 115200,
    [int]$TimeoutSeconds = 90
)

$serial = [System.IO.Ports.SerialPort]::new(
    $Port,
    $Baud,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
$serial.ReadTimeout = 200
$buffer = [Text.StringBuilder]::new()
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

function Read-Until([string]$Pattern) {
    while ([DateTime]::UtcNow -lt $script:deadline) {
        Start-Sleep -Milliseconds 100
        $chunk = $script:serial.ReadExisting()
        if ($chunk.Length -eq 0) {
            continue
        }

        [void]$script:buffer.Append($chunk)
        [Console]::Out.Write($chunk)
        [Console]::Out.Flush()
        if ($script:buffer.ToString().Contains($Pattern)) {
            return
        }
        if ($script:buffer.Length -gt 32768) {
            [void]$script:buffer.Remove(0, $script:buffer.Length - 16384)
        }
    }
    throw "Timed out waiting for serial text: $Pattern"
}

try {
    $serial.Open()
    $serial.DiscardInBuffer()
    $serial.WriteLine("")
    Read-Until "login:"

    $buffer.Clear() | Out-Null
    $serial.WriteLine($Username)
    Read-Until "Password:"

    $buffer.Clear() | Out-Null
    $serial.WriteLine($Password)
    Read-Until "$Username@"

    $buffer.Clear() | Out-Null
    $serial.WriteLine("sudo -S -p '__CODEX_SUDO_PASSWORD__' -v && echo __CODEX_AUTH_OK__")
    Read-Until "__CODEX_SUDO_PASSWORD__"

    $buffer.Clear() | Out-Null
    $serial.WriteLine($Password)
    Read-Until "__CODEX_AUTH_OK__"
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
}
