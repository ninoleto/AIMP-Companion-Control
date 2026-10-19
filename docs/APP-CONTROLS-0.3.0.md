# Additional application controls (0.3.0)

Official source: AIMP 5.40 SDK `apiMessages.h`, `apiActions.h`, matching Delphi declarations and CHM `IAIMPAction` / `IAIMPActionEvent`. Windows window commands use documented Win32 calls on handles returned by AIMP, and reject handles outside AIMP's own process.

- `app/control/get`: `app_control.loaded`, `buffering` (0–100 or null), `last_action`.
- `app/notification`: `text` (1–4096 UTF-8 bytes), optional Boolean `persistent`. Uses `AIMP_MSG_CMD_SHOW_NOTIFICATION`. The string remains owned by this plugin until replaced or finalized.
- `app/notification/clear`: clears the notification through the documented null-pointer operation.
- `app/window`: `window` main / playlist / equalizer, `operation` show / hide / minimize / restore / foreground. Availability depends on the active skin/window layout. Foreground activation can be denied by Windows; this is reported explicitly. Successful window requests are queued, not proof that the final visibility changed.
- `app/quit`: requires `confirm:true`. Posts WM_CLOSE to the SDK-provided application window (main-window fallback); only acknowledges the queued close request. AIMP may display a prompt or apply its own close behavior. No plug-in callback remains on the stack when the close message is handled.
- `app/action/get`: `id` (1–512 UTF-8 bytes). Resolves the exact supplied ID through `IAIMPServiceActionManager.GetByID`; returns name, group, enabled, executable. The SDK does not enumerate built-in IDs. Unrecognized IDs return `ACTION_NOT_FOUND`.
- `app/action/execute`: resolves the same ID, checks enabled and handler availability, and queues execution on the main thread. It re-resolves and rechecks before execution. The callback receives the invoking `IAIMPAction` as documented. Only one action can be queued/executing. `app_control.last_action` reports queued/executing/completed/failed; completed only means the handler returned.

Capabilities are under `capabilities.app_control`: notifications, windows (id + available), action_lookup, action_execute, action_enumeration:false, quit. No speculative built-in action-ID catalog, private configuration writes, keyboard injection, or arbitrary message-number execution is used.

Startup commands and broadcasts wait for AIMP_MSG_PROPERTY_LOADED. Commands return AIMP_LOADING until ready; Companion beta.4 retries startup discovery instead of silently treating it as an old plugin.

Build checks are recorded in VERIFICATION.txt. These features have not been runtime-tested inside Windows AIMP here.
