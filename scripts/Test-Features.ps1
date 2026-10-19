# Read-only acceptance check for AIMP Companion Control 0.3.1.
# Run on the AIMP computer, or pass -Address <AIMP-PC-IP> for a permitted LAN client.
param(
    [string]$Address = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$Port = 19122
)
$ErrorActionPreference = 'Stop'
$socket = [System.Net.WebSockets.ClientWebSocket]::new()
$failures = [System.Collections.Generic.List[string]]::new()
$endpoint = [System.UriBuilder]::new('ws', $Address, $Port, '/').Uri
$phase = 'connection'

function Invoke-ReadOnlyQuery {
    param([hashtable]$Command)
    # A fresh deadline covers the complete command and any unsolicited pushes.
    # On a timeout the outer handler closes this connection; never retry on it.
    $deadline = [System.Threading.CancellationTokenSource]::new(10000)
    try {
        $data = [System.Text.Encoding]::UTF8.GetBytes(($Command | ConvertTo-Json -Compress))
        $socket.SendAsync([ArraySegment[byte]]::new($data), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $deadline.Token).GetAwaiter().GetResult()
        do {
            $stream = [System.IO.MemoryStream]::new()
            try {
                do {
                    $buffer = [byte[]]::new(8192)
                    $result = $socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), $deadline.Token).GetAwaiter().GetResult()
                    if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
                        throw 'AIMP closed the WebSocket connection.'
                    }
                    if ($result.MessageType -ne [System.Net.WebSockets.WebSocketMessageType]::Text) {
                        throw 'AIMP returned a non-text response.'
                    }
                    $stream.Write($buffer, 0, $result.Count)
                    if ($stream.Length -gt 4194304) { throw 'Response exceeded the 4 MB diagnostic limit.' }
                } while (-not $result.EndOfMessage)
                $reply = [System.Text.Encoding]::UTF8.GetString($stream.ToArray()) | ConvertFrom-Json -Depth 64
            } finally { $stream.Dispose() }
        } while ($reply.action -ne $Command.action)
        return $reply
    } finally { $deadline.Dispose() }
}

function Show-QueryError {
    param([string]$Action, $Reply)
    $code = [string]$Reply.error.code
    if ($code -eq 'UNKNOWN_ACTION') {
        $message = "$Action is unsupported by the connected plug-in. Install AIMP Companion Control 0.3.1 and restart AIMP."
    } else {
        $detail = if ($Reply.error.message) { [string]$Reply.error.message } elseif ($Reply.error.detail) { [string]$Reply.error.detail } else { '' }
        $message = "$Action failed: $code"
        if ($detail) { $message += " ($detail)" }
        if ($null -ne $Reply.error.hresult) { $message += " [HRESULT $($Reply.error.hresult)]" }
    }
    $failures.Add($message)
    Write-Output "FAIL: $message"
}

try {
    Write-Output "Checking $endpoint (read-only)"
    $connectDeadline = [System.Threading.CancellationTokenSource]::new(10000)
    try { $socket.ConnectAsync($endpoint, $connectDeadline.Token).GetAwaiter().GetResult() }
    finally { $connectDeadline.Dispose() }

    $phase = 'capabilities/get'
    $capReply = Invoke-ReadOnlyQuery @{ action = $phase }
    if ($capReply.error) { Show-QueryError $phase $capReply }
    else {
        if ($capReply.protocol_version -ne 2 -or $null -eq $capReply.capabilities) {
            $failures.Add('The server did not return the expected protocol 2 capability catalogue.')
            Write-Output 'FAIL: Invalid or unexpected capability response.'
        }
        $cap = $capReply.capabilities
        $supported = @($cap.audio | Where-Object { $_.supported })
        $unavailable = @($cap.audio | Where-Object { -not $_.supported } | ForEach-Object { $_.id })
        Write-Output "Plug-in: $($capReply.plugin_version); protocol: $($capReply.protocol_version)."
        Write-Output "Audio parameters: $($supported.Count). Equalizer: $($cap.equalizer.available), $($cap.equalizer.band_count) bands."
        Write-Output "Queue: $($cap.library.queue). A-B loop: $($cap.loop.available). Radio recording: $($cap.recording.available). Output list: $($cap.output.list)."
        if ($unavailable.Count) { Write-Output "Unavailable audio parameters: $($unavailable -join ', ')." }
    }

    $phase = 'extensions/get'
    $ext = Invoke-ReadOnlyQuery @{ action = $phase }
    if ($ext.error) { Show-QueryError $phase $ext }
    else {
        foreach ($parameter in @('pitch', 'speed', 'tempo', 'balance')) {
            $value = $ext.audio.$parameter
            if ($null -eq $value) {
                $descriptor = @($capReply.capabilities.audio | Where-Object { $_.id -eq $parameter -and $_.supported })
                if ($descriptor.Count) {
                    $failures.Add("$parameter was advertised as supported but could not be read.")
                    Write-Output "FAIL: $parameter was advertised as supported but could not be read."
                } else { Write-Output "${parameter}: unavailable in this AIMP installation." }
            }
            else { Write-Output "${parameter}: $value" }
        }
        if ($ext.equalizer.read_errors) {
            $failures.Add('Equalizer returned one or more read errors.')
            Write-Output "FAIL: Equalizer read errors: $($ext.equalizer.read_errors | ConvertTo-Json -Compress)"
        }
        if ($null -ne $ext.queue.count) { Write-Output "Queue items: $($ext.queue.count); suspended: $($ext.queue.suspended)." }
        elseif ($capReply.capabilities.library.queue) {
            $failures.Add('Queue support was advertised but extensions/get could not read it.')
            Write-Output 'FAIL: Queue support was advertised but extensions/get could not read it.'
        }
    }

    $phase = 'player/status'
    $status = Invoke-ReadOnlyQuery @{ action = $phase }
    if ($status.error) { Show-QueryError $phase $status }
    elseif ($status.state -notin @('playing', 'paused', 'stopped')) {
        $failures.Add('player/status did not return a valid playback state.')
        Write-Output 'FAIL: Invalid playback status response.'
    } else { Write-Output "Playback: $($status.state); volume: $($status.volume)%; mute: $($status.muted)." }

    $phase = 'playlists/info'
    $playlist = Invoke-ReadOnlyQuery @{ action = $phase; target = 'playing' }
    if ($playlist.error.code -eq 'PLAYLIST_NOT_FOUND') { Write-Output 'Playing playlist: none (normal while no playlist is playing).' }
    elseif ($playlist.error) { Show-QueryError $phase $playlist }
    else { Write-Output "Playing playlist: $($playlist.playlist.name); $($playlist.playlist.track_count) tracks; $($playlist.playlist.duration) seconds." }

    $phase = 'player/fileinfo'
    $file = Invoke-ReadOnlyQuery @{ action = $phase }
    if ($file.error) { Show-QueryError $phase $file }
    elseif ($null -eq $file.file_info) { Write-Output 'Playing file metadata: none (normal while stopped).' }
    else { Write-Output "Playing file: $($file.file_info.artist) - $($file.file_info.title); codec: $($file.file_info.codec); sample rate: $($file.file_info.sample_rate)." }

    if ($failures.Count) {
        $phase = 'validation'
        throw "$($failures.Count) read-only checks failed; see the FAIL lines above."
    }
    Write-Output 'Read-only feature checks passed. No playback, DSP, queue, playlist, recording, or output setting was changed.'
    Write-Output 'This checks live reads only. Rotary actions and audio changes still need listening tests.'
} catch {
    if ($phase -eq 'connection') {
        throw "Could not connect to $endpoint. Check that AIMP is running, the plug-in listens at this address/port, and this client is allowed. $($_.Exception.Message)"
    }
    if ($phase -eq 'validation') { throw }
    throw "Read-only check stopped during $phase. The connection will be closed; no command is retried. A timeout can mean AIMP is busy or the bridge is not processing requests. $($_.Exception.Message)"
} finally {
    $socket.Abort()
    $socket.Dispose()
}
