# Notification actions and data for Lua

Status: events are implemented; data and tap actions are design only. The
binding sends, lists, gets and dismisses notifications and the runner exposes a
`subscribe(callback)` event stream. `Bundle` data and actions are still not
exposed.

`tactility.notification.subscribe(callback)` registers a caller-owned
`NotificationSubscription` and polls its queue from a 200 ms LVGL timer, so the
callback runs on the LVGL task with the state that created it. The subscription
is pinned in the registry until `:cancel()` or until the runner closes, and is
cancelled before the timers are swept at shutdown. Events are live-only: a
subscription does not replay history, and events posted before subscribing or
after cancelling are not kept. The module queues up to 8 events per
subscription; a faster burst drops the newest until the next poll. Each event is
`{ id, type, timestamp, order, bumped }` with `type` one of `EVENT.POSTED`,
`EVENT.DISMISSED`, `EVENT.CLEARED` or `EVENT.UPDATED`.

## Constraints from the C API

`notification_context_set_action()` stores a **C function pointer and a
`void* user_data`** that must stay valid for as long as the notification can be
listed. The action runs on the UI task, which holds locks and has a small stack;
it must only forward work and return. A Lua function, or a Lua registry
reference, cannot meet that lifetime: the runner can close while its
notification is still on screen.

The action's `user_data` has the same problem. The notification module has no
"dismissed" callback, so per-notification allocations would leak or dangle.

The Bundle is the one piece with a safe ownership model: the notification
system takes it over on send and returns a clone on `get`/`get_all`.

## Phase 1: data (no callbacks)

Add a `tactility.bundle` binding and let notifications carry data:

```lua
local id = tactility.notification.send{
    message = "Build finished",
    data = {
        action = "open-build",
        path = "/sdcard/lua/build.lua",
        success = true,
        exit_code = 0,
    },
}

local item = tactility.notification.get(id)
-- item.data is a plain Lua table; unknown bundle types are omitted
```

Bundle values: string, integer, boolean and nested tables. Keep floats out until
the Bundle API has a stable representation.

This needs no callback lifetime design and covers the common "tap a notification
to reopen the right screen" case when combined with the launcher/arguments.

## Phase 2: actions as data + a fixed C trampoline

Do **not** store a Lua callback in the notification. Instead:

1. The runtime installs one module-global C action for every notification it
   sends: `lua_notification_action(notification_id, NULL)`.
2. `action_data` stays `NULL`; the action reads the notification's Bundle by id.
   Nothing per-notification is allocated in C.
3. The Bundle carries the intent:
   `{ action = "<name>", app = "<app id>", script = "<path>", payload = ... }`.
4. The trampoline does the minimum: it queues an app-start request for `app`, or
   `lua.runner` with `--notification <id>`, and returns.

`lua_runner` gains `--notification <id>` (and optionally `--action <name>`).
After `on_show`, it calls an optional Lua callback:

```lua
function on_notification(id, action, data)
    -- data is notification.data; the notification is still readable
end
```

If the notification is not sticky, the UI dismisses it after the action, so the
runner must read the data promptly. For work that outlives the tap (network,
file copy), scripts should send a sticky notification and dismiss it from the
runner with `tactility.notification.dismiss(id)` when done.

### Why not store the Lua function

- The runner closes; the C notification keeps the pointer and would call freed
  code.
- The Lua registry belongs to one state; a second runner instance cannot use it.
- The action must not take the Lua lock on the UI task.

### Live-app dispatch (optional)

The trampoline can first ask the app scheduler whether the owning app instance is
alive and, if so, post a small app event to it. That needs a registry from
`source_id` (or notification id) to the live runner instance and a defined
cleanup when the runner closes. Phase 2 deliberately starts the app instead:
simpler, and correct after a reboot or a closed runner.

## Phase 3: actions in the event stream (next)

The notification events above are implemented. The remaining piece is the tap
callback, which turns an action tap into an event/start request:

```lua
tactility.notification.subscribe(function(event)
    -- event = { id, type = "posted" | "dismissed" | "cleared" | "updated" }
end)
```

An action tap is not an event in the C API: it calls the notification's
`NotificationAction` function pointer. Phase 2's fixed C trampoline is therefore
still the delivery mechanism for taps; once it exists it can also post a
`"action"` event into the live subscription. The runner already has an app-event
loop, but the Lua callback runs on the LVGL task, so a trampoline must marshal
the tap there rather than touching the Lua state on the UI task.

## Tests

- `tests/notification_events.lua`: subscribe, send/dismiss/clear and verify the
  posted/dismissed/cleared order, timestamps, ordering and cancel. Implemented.
- `tests/notification_data.lua`: send/read/update a nested Bundle, verify types
  and that a clone survives dismissal.
- `tests/notification_action.lua`: send a sticky notification with an action,
  invoke it through the notification UI (or a shell helper), verify the owning
  app starts with `--notification`, `on_notification` receives the action/data,
  and the app dismisses it.
- A close-path test: close the runner while a notification is listed, tap the
  action, and verify it starts a fresh runner instead of calling a dead state.

## Deliverables

1. `lua_runtime` bundle binding + `notification.send{data=...}` / `get().data`.
2. Runner `--notification` argument and `on_notification`.
3. A documented `action` + `app`/`script` convention in the Bundle.
4. Live-app notification events. Done: `subscribe(callback)` with the
   posted/dismissed/cleared/updated stream, dispatched on the LVGL task.
