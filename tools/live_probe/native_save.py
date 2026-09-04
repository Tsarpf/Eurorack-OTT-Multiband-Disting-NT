#!/usr/bin/env python3
"""Save an already-open, named measurement set without changing Windows focus.

The default is read-only inspection. --execute discovers Live's native File >
Save Live Set menu command and posts it directly to that Live window. It never
sends keyboard input. The caller must supply the owned set's path and can pin
the process ID. A successful save requires the expected window title and a
changed file on disk. --close --execute posts WM_CLOSE only when the named set
has no unsaved-change marker; it never discards changes or force-kills Live.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
import time

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.live_probe.bridge import local_path
    from tools.live_probe.setup_windows import powershell, ps_literal
else:
    from .bridge import local_path
    from .setup_windows import powershell, ps_literal


def inspect_or_post(expected_set: Path, process_id: int | None = None,
                    execute: bool = False) -> dict:
    process_query = (
        f"Get-Process -Id {int(process_id)} -ErrorAction Stop"
        if process_id is not None
        else "Get-Process -Name 'Ableton Live 12 Suite' -ErrorAction Stop"
    )
    script = r'''
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new()
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class LiveSaveMenu {
[DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr window);
[DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr menu,int position);
[DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr menu);
[DllImport("user32.dll")] public static extern uint GetMenuItemID(IntPtr menu,int position);
[DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr menu,uint position,uint flags);
[DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetMenuString(IntPtr menu,uint item,StringBuilder text,int length,uint flags);
[DllImport("user32.dll",SetLastError=true)] public static extern bool PostMessage(IntPtr window,uint message,UIntPtr wparam,IntPtr lparam);
}
'@
function MenuLabel([IntPtr]$menu,[int]$position) {
    $label=[System.Text.StringBuilder]::new(512)
    [void][LiveSaveMenu]::GetMenuString($menu,$position,$label,512,0x400)
    return ($label.ToString().Split("`t")[0]).Replace('&','')
}
''' + f"\n$processes=@({process_query})\n$expectedName={ps_literal(expected_set.stem)}\n" + r'''
if($processes.Count -ne 1){throw 'Expected exactly one Live process'}
$live=$processes[0]
if($live.ProcessName -ne 'Ableton Live 12 Suite'){throw 'The selected process is not Ableton Live 12 Suite'}
$expectedTitle='^'+[regex]::Escape($expectedName)+'\*? - Ableton Live '
if($live.MainWindowTitle -notmatch $expectedTitle){throw ('Unexpected Live document: '+$live.MainWindowTitle)}
$menu=[LiveSaveMenu]::GetMenu($live.MainWindowHandle)
if($menu -eq [IntPtr]::Zero){throw 'Live has no native menu handle'}
$matches=@()
for($top=0;$top -lt [LiveSaveMenu]::GetMenuItemCount($menu);$top++) {
    if((MenuLabel $menu $top) -ne 'File'){continue}
    $file=[LiveSaveMenu]::GetSubMenu($menu,$top)
    for($index=0;$index -lt [LiveSaveMenu]::GetMenuItemCount($file);$index++) {
        if((MenuLabel $file $index) -eq 'Save Live Set') {
            $matches += [PSCustomObject]@{Id=[LiveSaveMenu]::GetMenuItemID($file,$index);State=[LiveSaveMenu]::GetMenuState($file,$index,0x400)}
        }
    }
}
if($matches.Count -ne 1){throw 'Could not identify the exact File > Save Live Set command'}
$command=$matches[0]
if($command.Id -eq [uint32]::MaxValue -or $command.State -eq [uint32]::MaxValue){throw 'Invalid Save Live Set menu command'}
$dirty=$live.MainWindowTitle -match ('^'+[regex]::Escape($expectedName)+'\* - ')
$enabled=($command.State -band 3) -eq 0
$posted=$false
''' + (r'''
if($dirty) {
    if(-not $enabled){throw 'Save Live Set is disabled; check Live for a dialog'}
    $live.Refresh()
    if($live.MainWindowTitle -notmatch $expectedTitle){throw 'Live changed documents before save'}
    $posted=[LiveSaveMenu]::PostMessage($live.MainWindowHandle,0x111,[UIntPtr]$command.Id,[IntPtr]::Zero)
    if(-not $posted){throw 'Windows rejected the Save Live Set message'}
}
''' if execute else "") + r'''
[PSCustomObject]@{process_id=$live.Id;window=$live.MainWindowTitle;handle=$live.MainWindowHandle.ToInt64();command_id=$command.Id;enabled=$enabled;dirty=$dirty;posted=$posted}|ConvertTo-Json -Compress
'''
    return json.loads(powershell(script))


def save_live_set(expected_set: Path, process_id: int | None = None,
                  timeout: float = 30) -> dict:
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout must be a positive finite number")
    expected_set = expected_set.resolve()
    before = expected_set.stat()
    posted = inspect_or_post(expected_set, process_id, execute=True)
    if not posted["dirty"]:
        return {**posted, "saved": True, "already_saved": True, "path": str(expected_set)}
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        time.sleep(0.25)
        after = expected_set.stat()
        if after.st_mtime_ns != before.st_mtime_ns or after.st_size != before.st_size:
            current = inspect_or_post(expected_set, posted["process_id"])
            if not current["dirty"]:
                return {**current, "saved": True, "path": str(expected_set)}
    raise RuntimeError("Save did not complete in the expected file; check Live for a dialog. No keys were sent.")


def close_live_set(expected_set: Path, process_id: int | None = None,
                   timeout: float = 30) -> dict:
    """Close only the matching, already-saved set; do not force a blocked close."""
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout must be a positive finite number")
    current = inspect_or_post(expected_set.resolve(), process_id)
    if current["dirty"]:
        raise RuntimeError("The measurement set has unsaved changes; save it before closing")
    script = r'''
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class LiveCloseWindow {
[DllImport("user32.dll",SetLastError=true)] public static extern bool PostMessage(IntPtr window,uint message,UIntPtr wparam,IntPtr lparam);
}
'@
''' + f"\n$live=Get-Process -Id {current['process_id']} -ErrorAction Stop\n$expectedName={ps_literal(expected_set.stem)}\n" + r'''
$expectedTitle='^'+[regex]::Escape($expectedName)+' - Ableton Live '
if($live.ProcessName -ne 'Ableton Live 12 Suite' -or $live.MainWindowTitle -notmatch $expectedTitle){throw 'Live changed documents or has unsaved changes; close was not sent'}
''' + f"if($live.MainWindowHandle.ToInt64() -ne {current['handle']}){{throw 'Live window changed; close was not sent'}}\n" + r'''
if(-not [LiveCloseWindow]::PostMessage($live.MainWindowHandle,0x10,[UIntPtr]::Zero,[IntPtr]::Zero)){throw 'Windows rejected the close message'}
'''
    powershell(script)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        time.sleep(0.25)
        status = powershell(
            "$ProgressPreference='SilentlyContinue'; "
            f"$live=Get-Process -Id {current['process_id']} -ErrorAction SilentlyContinue; "
            "if($live){'running'}else{'exited'}"
        )
        if status == "exited":
            return {"process_id": current["process_id"], "closed": True, "path": str(expected_set.resolve())}
    raise RuntimeError("Live did not exit; check for a dialog. The process was not force-closed.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--set", dest="expected_set", type=local_path, required=True)
    parser.add_argument("--process-id", type=int)
    parser.add_argument("--execute", action="store_true", help="Save; otherwise only inspect the exact menu command")
    parser.add_argument("--close", action="store_true", help="With --execute, close the matching set only if already saved")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    try:
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            raise ValueError("--timeout must be a positive finite number")
        if args.process_id is not None and args.process_id <= 0:
            raise ValueError("--process-id must be positive")
        if not args.expected_set.is_file():
            raise ValueError("--set must be the existing, already-open measurement set")
        if not args.execute:
            result = inspect_or_post(args.expected_set, args.process_id)
        elif args.close:
            result = close_live_set(args.expected_set, args.process_id, args.timeout)
        else:
            result = save_live_set(args.expected_set, args.process_id, args.timeout)
    except Exception as exc:
        parser.exit(1, f"Native save stopped: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
