param([string]$Address = '127.0.0.1', [int]$Port = 19122)
$ErrorActionPreference = 'Stop'
$socket = [System.Net.WebSockets.ClientWebSocket]::new()
$deadline = [System.Threading.CancellationTokenSource]::new(10000)
try {
    $socket.ConnectAsync([Uri]"ws://${Address}:$Port/", $deadline.Token).GetAwaiter().GetResult()
    foreach ($action in @('player/status', 'playlists/get')) {
        $data = [System.Text.Encoding]::UTF8.GetBytes((@{action=$action} | ConvertTo-Json -Compress))
        $socket.SendAsync([ArraySegment[byte]]::new($data), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $deadline.Token).GetAwaiter().GetResult()
        do {
            $stream = [System.IO.MemoryStream]::new()
            try {
                do {
                    $buffer = [byte[]]::new(8192)
                    $result = $socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), $deadline.Token).GetAwaiter().GetResult()
                    if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { throw 'Server closed the connection.' }
                    $stream.Write($buffer, 0, $result.Count)
                    if ($stream.Length -gt 4194304) { throw 'Response exceeded test limit.' }
                } while (-not $result.EndOfMessage)
                $reply = [System.Text.Encoding]::UTF8.GetString($stream.ToArray()) | ConvertFrom-Json
            } finally { $stream.Dispose() }
        } while ($reply.action -ne $action)
        if ($reply.error) { throw ($reply.error | ConvertTo-Json -Compress) }
        if ($action -eq 'player/status') { Write-Output "Status response received. State: $($reply.state). Volume: $($reply.volume)." }
        else { Write-Output "Playlist response received. Count: $(@($reply.playlists).Count)." }
    }
    Write-Output 'Read-only connection check passed. Playback controls and Defender status still require separate checks.'
} finally {
    $socket.Abort()
    $socket.Dispose()
    $deadline.Dispose()
}
