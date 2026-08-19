param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [Parameter(Mandatory = $true)]
    [string]$Command,
    [int]$Baud = 115200,
    [int]$TimeoutSeconds = 60
)

$serial = [System.IO.Ports.SerialPort]::new(
    $Port,
    $Baud,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
$serial.NewLine = "`r"
$serial.ReadTimeout = 200
$token = [Guid]::NewGuid().ToString("N")
$marker = "__CODEX_RC_${token}_"
$output = [Text.StringBuilder]::new()
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

try {
    $serial.Open()
    $serial.DiscardInBuffer()
    $serial.WriteLine("$Command; __codex_rc=`$?; echo ${marker}`$__codex_rc")

    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $chunk = $serial.ReadExisting()
        if ($chunk.Length -eq 0) {
            continue
        }

        [void]$output.Append($chunk)
        [Console]::Out.Write($chunk)
        [Console]::Out.Flush()

        $match = [regex]::Match($output.ToString(), [regex]::Escape($marker) + "([0-9]+)")
        if ($match.Success) {
            exit [int]$match.Groups[1].Value
        }

        if ($output.Length -gt 65536) {
            [void]$output.Remove(0, $output.Length - 32768)
        }
    }

    throw "Timed out waiting for serial command completion after $TimeoutSeconds seconds"
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
}
