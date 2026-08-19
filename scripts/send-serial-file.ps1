param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [Parameter(Mandatory = $true)]
    [string]$Destination,
    [int]$Baud = 115200
)

$serial = [System.IO.Ports.SerialPort]::new(
    $Port,
    $Baud,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
$serial.NewLine = "`r"
$serial.WriteTimeout = 5000

try {
    $serial.Open()
    $serial.DiscardInBuffer()

    # Disable terminal echo so the encoded payload is not sent back over
    # the same serial link. base64 reads until the Ctrl-D below.
    $serial.WriteLine("stty -echo")
    Start-Sleep -Milliseconds 150
    $serial.WriteLine("base64 -d > '$Destination'")
    Start-Sleep -Milliseconds 150

    $encoded = [Convert]::ToBase64String([IO.File]::ReadAllBytes($Source))
    for ($offset = 0; $offset -lt $encoded.Length; $offset += 76) {
        $length = [Math]::Min(76, $encoded.Length - $offset)
        $serial.WriteLine($encoded.Substring($offset, $length))
        Start-Sleep -Milliseconds 7
    }

    $serial.Write([char]4)
    Start-Sleep -Milliseconds 750
    $serial.WriteLine("stty echo")
    $serial.WriteLine("sha256sum '$Destination'; echo __SERIAL_TRANSFER_DONE__")
    Start-Sleep -Milliseconds 1500
    $serial.ReadExisting()
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
}
