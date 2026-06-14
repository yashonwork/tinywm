# tinyrwm Rust Implementation — Full Annotated Walkthrough

**File:** `rust/src/main.rs` (795 lines)
**Author:** Julian Andrews
**License:** 0BSD

---

## Table of Contents

1. [Imports & Module Setup (lines 1–41)](#1-imports--module-setup-lines-141)
2. [Core Data Types (lines 43–134)](#2-core-data-types-lines-43134)
3. [WindowManager Methods (lines 136–319)](#3-windowmanager-methods-lines-136319)
4. [Window Methods (lines 321–343)](#4-window-methods-lines-321343)
5. [Output Methods (lines 346–353)](#5-output-methods-lines-346353)
6. [Seat Methods (lines 355–523)](#6-seat-methods-lines-355523)
7. [Dispatch Implementations (lines 526–766)](#7-dispatch-implementations-lines-526766)
8. [Delegate Noop (lines 768–769)](#8-delegate-noop-lines-768769)
9. [Main Function (lines 771–795)](#9-main-function-lines-771795)

---

## 1. Imports & Module Setup (lines 1–41)

### Lines 4–8 — Standard library imports

```rust
use std::collections::{HashMap, VecDeque};
use std::fmt::Debug;
```

| Import | Purpose |
|--------|---------|
| `HashMap<ObjectId, T>` | Stores seats and outputs keyed by their Wayland `ObjectId`. Fast lookup by proxy identity. |
| `VecDeque<Window>` | Stores windows in rendering order (back-to-front). A deque allows efficient `rotate_left(1)` for cycling focus (pop front → push back). |
| `ObjectId` | Wayland's unique identifier for each proxy object on the connection. Used to look up our local state when events arrive. |
| `Connection` | Represents the Wayland socket connection. Created once in `main()`. |
| `Dispatch` | Trait you implement to handle incoming events for a given proxy type. The core of the event-driven pattern. |
| `Proxy` | Trait all generated proxy objects implement. Provides `.id()`, `.version()`, etc. |
| `QueueHandle` | A handle tied to a specific event queue. Used to create child proxies (e.g., `get_node()`, `get_xkb_binding()`) that dispatch back to the same queue. |

### Lines 10–19 — River protocol imports

```rust
use crate::river::{
    river_node_v1::RiverNodeV1,
    river_output_v1::RiverOutputV1,
    river_pointer_binding_v1::RiverPointerBindingV1,
    river_seat_v1::{Modifiers, RiverSeatV1},
    river_window_manager_v1::RiverWindowManagerV1,
    river_window_v1::{Edges, RiverWindowV1},
    river_xkb_binding_v1::RiverXkbBindingV1,
    river_xkb_bindings_v1::RiverXkbBindingsV1,
};
```

These are all generated types from the XML protocol files. Each maps 1:1 to a Wayland protocol interface. They serve as proxy handles — you call methods on them to send requests to the compositor, and the `Dispatch` impls you write handle incoming events.

### Lines 21–41 — `mod river` — Protocol code generation

```rust
mod river {
    pub extern crate wayland_client;  // re-export so generated code finds it
    pub use wayland_client::protocol::*;

    mod interfaces {
        pub(super) mod rwm {
            pub use wayland_client::protocol::__interfaces::*;
            wayland_scanner::generate_interfaces!("./protocol/river-window-management-v1.xml");
        }
        pub(super) mod rxkb {
            use super::rwm::*;
            wayland_scanner::generate_interfaces!("./protocol/river-xkb-bindings-v1.xml");
        }
    }

    use self::interfaces::rwm::*;
    use self::interfaces::rxkb::*;
    wayland_scanner::generate_client_code!("./protocol/river-window-management-v1.xml");
    wayland_scanner::generate_client_code!("./protocol/river-xkb-bindings-v1.xml");
}
```

**Why two steps (interfaces + client code)?**

The two protocol XML files reference each other's types (e.g., `river-window-management-v1.xml` defines `RiverSeatV1` which is used by `river-xkb-bindings-v1.xml`). The scanner needs **all** interface enums/structs available first, hence:

1. `generate_interfaces!` — generates just the enums, structs, and opcode constants (no proxy types).
2. `generate_client_code!` — generates the full proxy types (`RiverWindowV1`, etc.), Dispatch scaffolding, and request methods.

The `rxkb` submodule does `use super::rwm::*` to bring the window-management types into scope so it can reference them during its own code generation.

**Result:** At compile time, all Wayland marshalling code is generated. You never write wire protocol code.

---

## 2. Core Data Types (lines 43–134)

### `Action` — Enum of user-triggered operations (lines 43–52)

```rust
#[derive(Debug, Clone, Copy)]
enum Action {
    None,         // no pending action
    SpawnFoot,    // launch foot terminal emulator
    Close,        // close the focused window
    FocusNext,    // cycle keyboard focus to next window
    Move,         // begin interactive move
    Resize,       // begin interactive resize
    Exit,         // exit the Wayland session
}
```

Each `Seat` stores a `pending_action: Action`. When a keyboard or pointer binding fires (Pressed event), it sets the seat's `pending_action`. On the next `manage` phase, `Seat::do_action()` executes it.

`Clone + Copy` is fine because this is a simple unit enum with no heap data.

### `SeatOp` — In-progress interactive operation (lines 54–70)

```rust
#[derive(Debug, Clone)]
enum SeatOp {
    None,
    Move {
        window_proxy: RiverWindowV1,   // the window being dragged
        start_x: i32,                   // its position when drag started
        start_y: i32,
    },
    Resize {
        window_proxy: RiverWindowV1,
        start_x: i32,
        start_y: i32,
        start_width: i32,               // its size when resize started
        start_height: i32,
        edges: Edges,                   // which edges are being dragged (bitflag)
    },
}
```

`SeatOp` tracks an ongoing pointer operation across multiple frames. The `start_*` fields capture initial state — during the render phase, delta values (`op_dx`, `op_dy`) are applied relative to these start values.

`Edges` is a bitflags type (from the `bitflags` crate) generated from the protocol XML. It can be `Left`, `Right`, `Top`, `Bottom`, or any combination.

**Why not `Clone + Copy`?** `RiverWindowV1` is a proxy handle (contains `Arc`-like internals), so `Clone` is fine but `Copy` would require it to be a `Copy` type, which proxy handles are not — they manage a reference count internally.

### `AppData` — Top-level dispatch state (lines 72–77)

```rust
#[derive(Debug, Default)]
struct AppData {
    river_wm: Option<RiverWindowManagerV1>,   // bound during registry roundtrip
    river_xkb: Option<RiverXkbBindingsV1>,    // bound during registry roundtrip
    wm: WindowManager,                        // our internal state
}
```

This is the root state object. Every `Dispatch::event()` implementation receives `&mut AppData`. The two `Option` fields:
- Start as `None`.
- Filled when the `wl_registry::Event::Global` arrives for the respective interface.
- If either is still `None` after the first roundtrip, main() exits with an error.

`Default` is derived (all fields become their default: `None` for Option, `WindowManager::default()` for `wm`).

### `WindowManager` — Core state machine (lines 79–84)

```rust
#[derive(Debug, Default)]
struct WindowManager {
    windows: VecDeque<Window>,        // ordered back-to-front (last = topmost)
    outputs: HashMap<ObjectId, Output>,
    seats: HashMap<ObjectId, Seat>,
}
```

Three collections tracking everything the WM knows about:

| Collection | Key | Purpose |
|------------|-----|---------|
| `windows` | index (VecDeque) | All known window surfaces. Order = rendering order. Back (index 0) = bottom; front (last) = topmost. |
| `outputs` | `ObjectId` | Displays/monitors. Only tracked for lifecycle (removal detection). |
| `seats` | `ObjectId` | Input devices (keyboard + mouse combos). Each seat has its own focus, bindings, and pending operations. |

`Default` gives empty collections.

### `Window` — Per-window state (lines 86–99)

```rust
struct Window {
    proxy: RiverWindowV1,              // Wayland proxy to send requests
    node: RiverNodeV1,                 // spatial node proxy (for set_position, place_top)
    new: bool,                         // true until first manage_start
    closed: bool,                      // true when compositor says window closed
    x: i32, y: i32,                    // current position (in layout coordinates)
    width: i32, height: i32,           // current dimensions
    pointer_move_requested: Option<RiverSeatV1>,    // seat requesting a move
    pointer_resize_requested: Option<RiverSeatV1>,  // seat requesting a resize
    pointer_resize_requested_edges: Edges,           // edges for the pending resize
}
```

**Key fields:**
- `new` — The compositor sends window events before the first manage sequence finishes. New windows need their position/dimensions proposed on the first `manage_start`, after which `new` is set to `false`.
- `closed` — Set when the compositor sends `Event::Closed`. The window is removed during the next manage cycle.
- `pointer_move_requested` / `pointer_resize_requested` — The compositor can request a move/resize via `Event::PointerMoveRequested` / `Event::PointerResizeRequested` (e.g., when a client-side title bar sends a drag). The `Option` holds the `RiverSeatV1` that made the request, stored until the next `manage_start` where `manage_windows()` processes it.

### `Output` — Per-output state (lines 101–105)

```rust
struct Output {
    proxy: RiverOutputV1,
    removed: bool,      // compositor said this output is gone
}
```

Minimal — tinyrwm doesn't do multi-monitor layout. Just tracks lifecycle so removed outputs can be cleaned up.

### `Seat` — Per-seat state (lines 107–122)

```rust
struct Seat {
    proxy: RiverSeatV1,
    new: bool,                                         // true until bindings are created
    removed: bool,                                     // compositor said this seat is gone
    focused: Option<RiverWindowV1>,                    // currently focused window
    hovered: Option<RiverWindowV1>,                    // window under pointer
    interacted: Option<RiverWindowV1>,                 // most recently interacted window
    xkb_bindings: HashMap<ObjectId, XkbBinding>,       // keyboard shortcut bindings
    pointer_bindings: HashMap<ObjectId, PointerBinding>, // mouse button bindings
    pending_action: Action,                            // action triggered by a binding press
    op: SeatOp,                                        // ongoing move/resize operation
    op_dx: i32, op_dy: i32,                            // accumulated pointer delta this frame
    op_release: bool,                                  // true when pointer button released
}
```

| Field | Purpose |
|-------|---------|
| `focused` | Window that has keyboard focus. Set by `focus_top()`. |
| `hovered` | Window under the pointer cursor. Updated by `PointerEnter` / `PointerLeave` events. |
| `interacted` | Most recently interacted window (clicked on). Used to raise it to top of stack in `manage_seats()`. |
| `xkb_bindings` | Maps `ObjectId` → `XkbBinding`. Created once per seat in `init_new_seats()`. |
| `pending_action` | Set by binding `Pressed` events, consumed by `do_action()` during manage. |
| `op` | Active move/resize operation. Persists across multiple manage/render cycles until `op_release`. |
| `op_dx`, `op_dy` | Pointer delta since last manage. Set by `OpDelta` events. Zeroed when an op starts. |
| `op_release` | Set by `OpRelease` event. Signals that the pointer button was released and the op should end. |

### `XkbBinding` & `PointerBinding` (lines 124–134)

```rust
struct XkbBinding {
    proxy: RiverXkbBindingV1,
    action: Action,                // what to do when this binding is pressed
}
struct PointerBinding {
    proxy: RiverPointerBindingV1,
    action: Action,
}
```

Each binding wraps a Wayland proxy and the action it should trigger. The proxy is used to `enable()` the binding (which tells the compositor to listen for it) and to match events by `proxy.id()`.

---

## 3. WindowManager Methods (lines 136–319)

This is the heart of the window manager logic. All methods are called from `handle_manage_start()` or `handle_render_start()`.

### `handle_manage_start()` (lines 137–151)

```rust
fn handle_manage_start(&mut self, proxy: &RiverWindowManagerV1, river_xkb: &RiverXkbBindingsV1, qh: &QueueHandle<AppData>) {
    self.remove_outputs();       // clean up gone outputs
    self.remove_windows();       // clean up closed windows
    self.remove_seats();         // clean up removed seats
    self.init_new_windows();     // set position/dimensions on newly created windows
    self.init_new_seats(river_xkb, qh);  // create keyboard/pointer bindings for new seats
    self.manage_windows();       // process pending move/resize requests from compositor
    self.manage_seats(proxy);    // focus top, execute actions, manage pointer ops
    proxy.manage_finish();       // tell compositor we're done managing
}
```

**The manage sequence lifecycle:**
1. The compositor sends various events (new windows, closed windows, moved seats, etc.).
2. Then it sends `Event::ManageStart`.
3. We process all pending state changes in a deterministic order.
4. We call `manage_finish()` to signal we're done.

This is a **double-buffered** protocol — all state changes are batched between manage/render frames.

### `handle_render_start()` (lines 153–197)

```rust
fn handle_render_start(&mut self, proxy: &RiverWindowManagerV1) {
    for seat in &mut self.seats.values_mut() {
        match &seat.op {
            SeatOp::None => {}
            SeatOp::Move { window_proxy, start_x, start_y } => {
                // Apply accumulated delta to window position
                if let Some(window) = self.windows.iter_mut().find(|w| &w.proxy == window_proxy) {
                    window.set_position(start_x + seat.op_dx, start_y + seat.op_dy);
                }
            }
            SeatOp::Resize { window_proxy, start_x, start_y, start_width, start_height, edges } => {
                if let Some(window) = self.windows.iter_mut().find(|w| &w.proxy == window_proxy) {
                    let (mut x, mut y) = (*start_x, *start_y);
                    // When dragging left/top edge, position must adjust as width changes
                    if edges.contains(Edges::Left)   { x += start_width - window.width; }
                    if edges.contains(Edges::Top)    { y += start_height - window.height; }
                    window.set_position(x, y);
                }
            }
        }
    }
    proxy.render_finish();
}
```

**During the render phase:**
- For **move**: `window.position = start_position + delta`
- For **resize**: Position only needs to change when dragging the **left** or **top** edge (because the window's origin shifts). The right/bottom edges just change size.
- The actual dimension change during resize is sent via `propose_dimensions` in `op_manage()` (called during the manage phase), not here. Here we only adjust position.

### `remove_outputs()` (lines 199–207)

```rust
fn remove_outputs(&mut self) {
    self.outputs.retain(|_, output| {
        if output.removed {
            output.proxy.destroy();  // tell compositor we're done with this output
            return false;            // remove from HashMap
        }
        true
    });
}
```

Uses `HashMap::retain()` — if predicate returns `false`, entry is removed. Also destroys the Wayland proxy (sends a `destroy` request to the compositor).

### `remove_windows()` (lines 209–229)

```rust
fn remove_windows(&mut self) {
    let old_windows = std::mem::take(&mut self.windows);
    self.windows = old_windows.into_iter().filter(|window| {
        if window.closed {
            // If there's an active move/resize on this window, end it
            for seat in self.seats.values_mut() {
                if let SeatOp::Move { window_proxy, .. } | SeatOp::Resize { window_proxy, .. } = &seat.op {
                    if window_proxy == &window.proxy {
                        seat.op_end();
                    }
                }
            }
            return false;  // remove this window
        }
        true
    }).collect();
}
```

Uses `std::mem::take` to swap the windows deque with an empty one (avoids borrowing issues), then rebuilds it with `filter` + `collect`. If a window being removed has an active move/resize, the operation is ended gracefully.

### `remove_seats()` (lines 231–245)

```rust
fn remove_seats(&mut self) {
    self.seats.retain(|_, seat| {
        if seat.removed {
            seat.xkb_bindings.values_mut().for_each(|b| b.proxy.destroy());
            seat.pointer_bindings.values_mut().for_each(|b| b.proxy.destroy());
            seat.proxy.destroy();
            return false;
        }
        true
    });
}
```

Destroys all binding proxies and the seat proxy when a seat is removed, then removes the seat from the map.

### `init_new_windows()` (lines 247–253)

```rust
fn init_new_windows(&mut self) {
    for window in self.windows.iter_mut().filter(|w| w.new) {
        window.set_position(window.x, window.y);
        window.proxy.propose_dimensions(window.width, window.height);
        window.new = false;
    }
}
```

For windows created since the last manage cycle:
1. Set their position (via the `RiverNodeV1` proxy).
2. Propose initial dimensions (0x0 by default — lets compositor decide natural size).
3. Mark them as initialized.

### `init_new_seats()` (lines 255–277)

```rust
fn init_new_seats(&mut self, river_xkb: &RiverXkbBindingsV1, qh: &QueueHandle<AppData>) {
    // Keysyms from xkbcommon/xkbcommon-keysyms.h
    const SPACE: u32 = 0x20;
    const N: u32 = 0x6e;
    const Q: u32 = 0x71;
    const ESC: u32 = 0xff1b;
    // Button codes from linux/input-event-codes.h
    const BTN_LEFT: u32 = 0x110;
    const BTN_RIGHT: u32 = 0x111;
    let mods = Modifiers::Mod4;

    for seat in self.seats.values_mut() {
        if seat.new {
            seat.create_xkb_binding(river_xkb, qh, mods, SPACE, Action::SpawnFoot);
            seat.create_xkb_binding(river_xkb, qh, mods, Q, Action::Close);
            seat.create_xkb_binding(river_xkb, qh, mods, N, Action::FocusNext);
            seat.create_xkb_binding(river_xkb, qh, mods, ESC, Action::Exit);
            seat.create_pointer_binding(qh, mods, BTN_LEFT, Action::Move);
            seat.create_pointer_binding(qh, mods, BTN_RIGHT, Action::Resize);
            seat.new = false;
        }
    }
}
```

Creates all keyboard and pointer bindings for new seats:

| Binding | Action | Effect |
|---------|--------|--------|
| Mod4+Space | `SpawnFoot` | Launch foot terminal |
| Mod4+Q | `Close` | Close focused window |
| Mod4+N | `FocusNext` | Cycle focus to next window |
| Mod4+Escape | `Exit` | Shut down river session |
| Mod4+Left Click | `Move` | Interactive move |
| Mod4+Right Click | `Resize` | Interactive resize |

`Modifiers::Mod4` = Super/Windows key. Keysym values are **XKB keysyms** (logical characters), not hardware keycodes — this is why the river XKB binding protocol exists.

### `manage_windows()` (lines 279–296)

```rust
fn manage_windows(&mut self) {
    for window in self.windows.iter_mut() {
        if let Some(seat_proxy) = window.pointer_move_requested.take() {
            let seat = self.seats.get_mut(&seat_proxy.id()).expect("Seat not found");
            seat.pointer_move(window);
        }
        if let Some(seat_proxy) = window.pointer_resize_requested.take() {
            let seat = self.seats.get_mut(&seat_proxy.id()).expect("Seat not found");
            seat.pointer_resize(window, window.pointer_resize_requested_edges);
        }
    }
}
```

Handles **compositor-initiated** move/resize requests (e.g., a client-side title bar received a mouse-down on its drag region). These arrive as `Event::PointerMoveRequested` / `Event::PointerResizeRequested` on the window. We stash them on the window struct and process them here, during the manage phase.

### `manage_seats()` (lines 298–318)

```rust
fn manage_seats(&mut self, wm_proxy: &RiverWindowManagerV1) {
    for seat in self.seats.values_mut() {
        // Raise interacted window to top of stack
        if let Some(window_proxy) = seat.interacted.take() {
            let i = self.windows.iter().position(|w| w.proxy == window_proxy)
                .expect("Interacted window not found");
            let window = self.windows.remove(i).unwrap();
            self.windows.push_back(window);
        }
        seat.focus_top(&self.windows);
        seat.do_action(&mut self.windows, wm_proxy);
        // Handle op lifecycle
        if seat.op_release {
            seat.op_end();
            seat.op_release = false;
        } else {
            seat.op_manage();
        }
    }
}
```

Per-seat management during the manage phase:

1. **Raise interacted window** — If a window was clicked (got `WindowInteraction` event), remove it from its current position in the stack and push it to the back (topmost position).

2. **Focus top window** — Focus the last window in the deque (topmost).

3. **Execute pending action** — Run whatever action was triggered by a binding press.

4. **Op lifecycle** — If the pointer was released, end the operation; otherwise, update the operation state (e.g., send new proposed dimensions during resize).

---

## 4. Window Methods (lines 321–343)

### `Window::new()` (lines 322–337)

```rust
fn new(proxy: RiverWindowV1, qh: &QueueHandle<AppData>) -> Self {
    let node = proxy.get_node(qh, ());
    Window {
        proxy, node,
        new: true, closed: false,
        x: 0, y: 0,
        width: 0, height: 0,
        pointer_move_requested: None,
        pointer_resize_requested: None,
        pointer_resize_requested_edges: Edges::None,
    }
}
```

Creates a new window wrapper. Crucially, it calls `proxy.get_node(qh, ())` to obtain the `RiverNodeV1` — a secondary proxy that controls the window's spatial properties (position, stacking order). The `()` is the "data" argument that Wayland dispatch passes back to your event handler (we don't need it, so `()`).

### `Window::set_position()` (lines 339–343)

```rust
fn set_position(&mut self, x: i32, y: i32) {
    self.node.set_position(x, y);
    self.x = x;
    self.y = y;
}
```

Sends `set_position` via the node proxy and caches the value locally.

---

## 5. Output Methods (lines 346–353)

### `Output::new()` (lines 347–352)

Straightforward constructor. `removed` starts as `false`, set to `true` when `Event::Removed` arrives.

---

## 6. Seat Methods (lines 355–523)

### `Seat::new()` (lines 356–372)

Constructs a new seat with all fields initialized to defaults. Notable: `self.proxy.id()` is the `ObjectId` used as the key in the `seats` HashMap.

### `Seat::create_xkb_binding()` (lines 374–386)

```rust
fn create_xkb_binding(&mut self, river_xkb: &RiverXkbBindingsV1, qh: &QueueHandle<AppData>,
    mods: Modifiers, keysym: u32, action: Action)
{
    let proxy = river_xkb.get_xkb_binding(&self.proxy, keysym, mods, qh, self.proxy.id());
    proxy.enable();
    let binding = XkbBinding { proxy, action };
    self.xkb_bindings.insert(binding.proxy.id(), binding);
}
```

1. `river_xkb.get_xkb_binding(&self.proxy, keysym, mods, qh, self.proxy.id())` — asks the compositor to create a binding that matches `Modifiers + keysym` on this seat. Passes `self.proxy.id()` as the "dispatch data" so that events for this binding get routed with the seat's ObjectId (we use that to look up the seat in our HashMap).
2. `proxy.enable()` — activates the binding (compositor starts listening).
3. Stores it in `xkb_bindings` keyed by the binding's own `ObjectId`.

### `Seat::create_pointer_binding()` (lines 388–401)

Same pattern, but uses `self.proxy.get_pointer_binding(button, mods, qh, self.proxy.id())` to create a pointer button binding.

### `Seat::do_action()` (lines 403–445)

```rust
fn do_action(&mut self, windows: &mut VecDeque<Window>, wm_proxy: &RiverWindowManagerV1) {
    match self.pending_action {
        Action::None => {}
        Action::SpawnFoot => {
            match std::process::Command::new("foot")
                .env_remove("WAYLAND_DEBUG")
                .spawn()
            {
                Ok(_) => {}
                Err(e) => eprintln!("Failed to spawn foot: {e}"),
            }
        }
        Action::Close => {
            if let Some(window_proxy) = self.focused.as_ref() {
                window_proxy.close();
            }
        }
        Action::FocusNext => {
            windows.rotate_left(1);           // bottom window goes to top
            self.focus_top(windows);           // focus the new top
        }
        Action::Move => {
            if let (Some(window_proxy), SeatOp::None) = (self.hovered.as_ref(), &self.op) {
                let window = windows.iter().find(|w| &w.proxy == window_proxy)
                    .expect("Hovered window not found");
                self.pointer_move(window);
            }
        }
        Action::Resize => {
            if let (Some(window_proxy), SeatOp::None) = (self.hovered.as_ref(), &self.op) {
                let window = windows.iter().find(|w| &w.proxy == window_proxy)
                    .expect("Hovered window not found");
                self.pointer_resize(window, Edges::Bottom.union(Edges::Right));
            }
        }
        Action::Exit => wm_proxy.exit_session(),
    }
    self.pending_action = Action::None;
}
```

| Action | Behavior |
|--------|----------|
| `None` | No-op |
| `SpawnFoot` | Spawns `foot` via `std::process::Command`. Strips `WAYLAND_DEBUG` from environment to reduce noise. On failure, prints to stderr (doesn't crash). |
| `Close` | Sends `close()` to the focused window proxy. |
| `FocusNext` | Calls `rotate_left(1)` on the VecDeque (bottom → top), then focuses the new top. |
| `Move` | Starts interactive move on the **hovered** window (not focused one). Only if no op is already in progress (`SeatOp::None`). |
| `Resize` | Starts interactive resize on the hovered window, dragging from **bottom-right** edge. |
| `Exit` | Calls `exit_session()` which tells river to shut down the compositor. |

The `env_remove("WAYLAND_DEBUG")` detail is a nice touch — when debugging the WM with `WAYLAND_DEBUG=1`, you don't want spawned processes to inherit it and flood their stderr.

### `Seat::op_end()` (lines 447–453)

```rust
fn op_end(&mut self) {
    if let SeatOp::Resize { window_proxy, .. } = &self.op {
        window_proxy.inform_resize_end();  // notify client resize is done
    }
    self.proxy.op_end();    // tell compositor the pointer op is over
    self.op = SeatOp::None;
}
```

Called when pointer button is released. Sends `inform_resize_end()` to the window (so the client knows to commit the new size) and `op_end()` to the seat (compositor stops sending deltas). Resets op state.

### `Seat::op_manage()` (lines 455–481)

```rust
fn op_manage(&mut self) {
    match &self.op {
        SeatOp::None | SeatOp::Move { .. } => {}  // move position updated in render phase
        SeatOp::Resize { window_proxy, start_width, start_height, edges, .. } => {
            let (mut width, mut height) = (*start_width, *start_height);
            if edges.contains(Edges::Left)   { width  -= self.op_dx; }
            if edges.contains(Edges::Right)  { width  += self.op_dx; }
            if edges.contains(Edges::Top)    { height -= self.op_dy; }
            if edges.contains(Edges::Bottom) { height += self.op_dy; }
            window_proxy.propose_dimensions(width, height);
        }
    }
}
```

During an active resize, sends `propose_dimensions()` to the window with the new size based on the pointer delta and which edges are being dragged. Positive/negative direction depends on which edge moves.

### `Seat::focus_top()` (lines 483–495)

```rust
fn focus_top(&mut self, windows: &VecDeque<Window>) {
    match windows.back() {
        Some(window) => {
            self.proxy.focus_window(&window.proxy);
            window.node.place_top();
            self.focused = Some(window.proxy.clone());
        }
        None => {
            self.proxy.clear_focus();
            self.focused = None;
        }
    }
}
```

- Focuses the topmost window (`.back()` of VecDeque = last element = front of stack).
- Calls `place_top()` on the node to ensure it's rendered above others.
- If no windows exist, clears focus.

### `Seat::pointer_move()` / `Seat::pointer_resize()` (lines 497–523)

These both follow the same pattern:

```rust
fn pointer_move(&mut self, window: &Window) {
    self.interacted = Some(window.proxy.clone());  // raise this window
    self.proxy.op_start_pointer();                  // tell compositor to start sending deltas
    self.op = SeatOp::Move {                        // record initial state
        window_proxy: window.proxy.clone(),
        start_x: window.x,
        start_y: window.y,
    };
    self.op_dx = 0;
    self.op_dy = 0;
}
fn pointer_resize(&mut self, window: &Window, edges: Edges) {
    self.interacted = Some(window.proxy.clone());
    self.proxy.op_start_pointer();
    window.proxy.inform_resize_start();              // tell client resize is beginning
    self.op = SeatOp::Resize {
        window_proxy: window.proxy.clone(),
        start_x: window.x,
        start_y: window.y,
        start_width: window.width,
        start_height: window.height,
        edges,
    };
    self.op_dx = 0;
    self.op_dy = 0;
}
```

`op_start_pointer()` transitions the seat into pointer-operation mode. From then on, pointer motion generates `OpDelta` events (instead of normal pointer motion) until `op_end()` is called. This is how river implements interactive move/resize — the compositor handles the pointer grab, and delta events are frame-perfect.

---

## 7. Dispatch Implementations (lines 526–766)

This is where the event loop connects Wayland events to our state. Each `Dispatch<SomeProxyType> for AppData` handles events for that proxy.

### `Dispatch<WlRegistry>` — Binding globals (lines 526–578)

```rust
impl Dispatch<wl_registry::WlRegistry, ()> for AppData {
    fn event(state: &mut Self, registry: &wl_registry::WlRegistry,
             event: wl_registry::Event, _data: &(), _conn: &Connection, qh: &QueueHandle<Self>)
    {
        if let wl_registry::Event::Global { name, interface, version } = event {
            match interface.as_str() {
                "river_window_manager_v1" => {
                    // Check version >= 4, bind it
                    let wm = registry.bind::<RiverWindowManagerV1, _, _>(
                        name, RIVER_WINDOW_MANAGER_V1_VERSION, qh, ());
                    state.river_wm = Some(wm);
                }
                "river_xkb_bindings_v1" => {
                    // Check version >= 1, bind it
                    let xkb = registry.bind::<RiverXkbBindingsV1, _, _>(
                        name, RIVER_XKB_BINDINGS_V1_VERSION, qh, ());
                    state.river_xkb = Some(xkb);
                }
                _ => {}  // ignore other globals
            }
        }
    }
}
```

Handles `wl_registry::Event::Global` — the compositor advertises available protocols. We:
1. Check the version meets our minimum requirement (4 for window management, 1 for xkb bindings).
2. Call `registry.bind::<T, _, _>(name, version, qh, ())` — this creates a proxy of type `T` bound to the global, which sends the `wl_registry::bind` request.
3. Store it in `AppData`.

If the server doesn't support the required protocol versions, we exit with an error.

### `Dispatch<RiverWindowManagerV1>` — Main WM events (lines 580–621)

```rust
impl Dispatch<RiverWindowManagerV1, ()> for AppData {
    fn event(state: &mut Self, proxy: &RiverWindowManagerV1,
             event: <RiverWindowManagerV1 as Proxy>::Event,
             _data: &(), _conn: &Connection, qh: &QueueHandle<Self>)
    {
        match event {
            Event::Unavailable => { /* another WM is running, exit */ }
            Event::Finished => std::process::exit(0),  // compositor shut down
            Event::ManageStart => state.wm.handle_manage_start(proxy, &river_xkb, qh),
            Event::RenderStart => state.wm.handle_render_start(proxy),
            Event::SessionLocked | Event::SessionUnlocked => {}  // not handled
            Event::Window { id } => state.wm.windows.push_back(Window::new(id, qh)),
            Event::Output { id } => { state.wm.outputs.insert(id.id(), Output::new(id)); }
            Event::Seat { id } => { state.wm.seats.insert(id.id(), Seat::new(id)); }
        }
    }
}
```

| Event | Action |
|-------|--------|
| `Unavailable` | Another WM is already running. Exit with error. |
| `Finished` | Compositor is shutting down. Exit cleanly. |
| `ManageStart` | Process all pending state changes (add/remove windows/seats, execute actions). |
| `RenderStart` | Apply visual state changes (window positions during move). |
| `Window { id }` | A new window appeared. Create our wrapper and push to back of stack. |
| `Output { id }` | A new output (display) appeared. Track it. |
| `Seat { id }` | A new seat (keyboard+mouse) appeared. Track it. |

The `id` field in `Window`, `Output`, `Seat` events is the child proxy itself — created by the compositor. We pass it directly to the constructor.

The `event_created_child!` macro at lines 616–620 tells the wayland-client framework which child proxy types to expect and what dispatch data to associate with them:

```rust
wayland_client::event_created_child!(AppData, RiverWindowManagerV1, [
    river::river_window_manager_v1::EVT_WINDOW_OPCODE => (RiverWindowV1, ()),
    river::river_window_manager_v1::EVT_OUTPUT_OPCODE => (RiverOutputV1, ()),
    river::river_window_manager_v1::EVT_SEAT_OPCODE => (RiverSeatV1, ())
]);
```

This means: "when `RiverWindowManagerV1` emits an event at opcode `EVT_WINDOW_OPCODE`, the child proxy type is `RiverWindowV1` with dispatch data `()`." This enables the framework to auto-create the child proxy with the right Dispatch impl.

### `Dispatch<RiverWindowV1>` — Window events (lines 623–667)

```rust
impl Dispatch<RiverWindowV1, ()> for AppData {
    fn event(state: &mut Self, proxy: &RiverWindowV1,
             event: <RiverWindowV1 as Proxy>::Event,
             _data: &(), _conn: &Connection, _qh: &QueueHandle<Self>)
    {
        // Find our local Window struct for this proxy
        let window = match state.wm.windows.iter_mut().find(|o| &o.proxy == proxy) {
            Some(window) => window,
            None => return,
        };
        match event {
            Event::Closed => window.closed = true,
            Event::Dimensions { width, height } => (window.width, window.height) = (width, height),
            Event::PointerMoveRequested { seat } => window.pointer_move_requested = Some(seat),
            Event::PointerResizeRequested { seat, edges } => {
                window.pointer_resize_requested = Some(seat);
                window.pointer_resize_requested_edges = edges.into_result().expect("Invalid edges");
            }
            // Unhandled events are ignored
            Event::DimensionsHint { .. } | Event::AppId { .. } | Event::Title { .. } => {}
            Event::Parent { .. } | Event::DecorationHint { .. } => {}
            Event::ShowWindowMenuRequested { .. } | Event::MaximizeRequested => {}
            Event::UnmaximizeRequested | Event::FullscreenRequested { .. } => {}
            Event::ExitFullscreenRequested | Event::MinimizeRequested => {}
            Event::UnreliablePid { .. } | Event::PresentationHint { .. } => {}
            Event::Identifier { .. } => {}
        }
    }
}
```

Handles events for individual windows. The `find()` at line 633 is necessary because Wayland events include the proxy that generated them, and we need to find our corresponding `Window` struct.

Key handled events:
| Event | Action |
|-------|--------|
| `Closed` | Mark as closed (removed during next manage cycle) |
| `Dimensions { width, height }` | Update cached size (sent by compositor before `ManageStart`) |
| `PointerMoveRequested { seat }` | Stash the requesting seat — compositor wants us to start a move |
| `PointerResizeRequested { seat, edges }` | Stash for later processing |

### `Dispatch<RiverOutputV1>` — Output events (lines 669–694)

Only cares about `Event::Removed` — marks the output for cleanup.

### `Dispatch<RiverSeatV1>` — Seat events (lines 696–721)

```rust
impl Dispatch<RiverSeatV1, ()> for AppData {
    fn event(state: &mut Self, proxy: &RiverSeatV1,
             event: <RiverSeatV1 as Proxy>::Event, ...)
    {
        let seat = state.wm.seats.get_mut(&proxy.id()).expect("Seat not found");
        match event {
            Event::Removed => seat.removed = true,
            Event::PointerEnter { window } => seat.hovered = Some(window),
            Event::PointerLeave => seat.hovered = None,
            Event::WindowInteraction { window } => seat.interacted = Some(window),
            Event::OpDelta { dx, dy } => (seat.op_dx, seat.op_dy) = (dx, dy),
            Event::OpRelease => seat.op_release = true,
            _ => {}  // WlSeat, ShellSurfaceInteraction, PointerPosition are ignored
        }
    }
}
```

| Event | Action |
|-------|--------|
| `Removed` | Mark for cleanup |
| `PointerEnter { window }` | Track which window the cursor is over |
| `PointerLeave` | Clear hovered window |
| `WindowInteraction { window }` | User clicked on a window — mark as interacted (will be raised to top) |
| `OpDelta { dx, dy }` | While a pointer operation is active, accumulates the movement delta |
| `OpRelease` | Pointer button released — end the ongoing operation |

### `Dispatch<RiverXkbBindingV1>` — Keyboard binding events (lines 723–744)

```rust
impl Dispatch<RiverXkbBindingV1, ObjectId> for AppData {
    fn event(state: &mut Self, proxy: &RiverXkbBindingV1,
             event: <RiverXkbBindingV1 as Proxy>::Event,
             data: &ObjectId, ...)  // <-- data is the seat's ObjectId!
    {
        let seat = state.wm.seats.get_mut(data).expect("Seat not found");
        let binding = seat.xkb_bindings.get(&proxy.id()).expect("xkb_binding not found");
        match event {
            Event::Pressed => seat.pending_action = binding.action,
            Event::Released | Event::StopRepeat => {}
        }
    }
}
```

**Key detail:** The dispatch data (`data: &ObjectId`) is the seat's ObjectId — we passed `self.proxy.id()` as the data argument when creating the binding. This lets us look up the seat directly without iterating.

When a key binding is pressed, we copy the binding's `action` to the seat's `pending_action`. It will be executed during the next `manage_start` cycle.

### `Dispatch<RiverPointerBindingV1>` — Pointer binding events (lines 746–766)

Same pattern as xkb bindings. On `Pressed`, copies the binding's action to `seat.pending_action`.

### `Dispatch<RiverNodeV1>` and `Dispatch<RiverXkbBindingsV1>` (lines 768–769)

```rust
wayland_client::delegate_noop!(AppData: ignore RiverXkbBindingsV1);
wayland_client::delegate_noop!(AppData: ignore RiverNodeV1);
```

These two types never send events to us (or we don't care about their events). `delegate_noop!` implements `Dispatch` for these types as a no-op — events are silently discarded. Required because the framework requires a `Dispatch` impl for every proxy type in our object tree.

---

## 8. Main Function (lines 771–795)

```rust
fn main() -> Result<(), Box<dyn std::error::Error>> {
    // 1. Connect to Wayland display
    let conn = Connection::connect_to_env()?;
    let display = conn.display();
    let mut event_queue = conn.new_event_queue();
    let _registry = display.get_registry(&event_queue.handle(), ());

    // 2. Initialize state
    let mut app_data = AppData::default();

    // 3. Roundtrip: flush requests and receive initial globals
    event_queue.roundtrip(&mut app_data)?;

    // 4. Verify required globals were bound
    if app_data.river_wm.is_none() { /* error */ }
    if app_data.river_xkb.is_none() { /* error */ }

    // 5. Event loop
    loop {
        event_queue.blocking_dispatch(&mut app_data)?;
    }
}
```

**Step-by-step:**

1. **`Connection::connect_to_env()`** — connects to the Wayland server using the `WAYLAND_DISPLAY` environment variable (defaults to `wayland-0`). Returns `Err` if no server is running.

2. **`conn.display()`** — gets the `WlDisplay` proxy (the root object, always ID 1).

3. **`conn.new_event_queue()`** — creates an event queue. Wayland connections can have multiple event queues; we use one.

4. **`display.get_registry(&event_queue.handle(), ())`** — sends `wl_display.get_registry` request. This triggers the compositor to send `wl_registry.global` events listing all available protocols.

5. **`event_queue.roundtrip(&mut app_data)`** — crucial call. This:
   - Flushes all pending outgoing requests to the socket.
   - Blocks until the compositor replies with a `wl_display.sync` roundtrip.
   - Dispatches all received events to our `Dispatch` impls, populating `app_data.river_wm` and `app_data.river_xkb`.

6. **Validation** — If either global wasn't advertised, exit with a helpful error message.

7. **`event_queue.blocking_dispatch(&mut app_data)`** — infinite loop that blocks on the Wayland socket, reads events, and dispatches them. This is the main event loop. The compositor will send `ManageStart` / `RenderStart` events in a regular cadence, and we respond by updating state.

---

## Architecture Summary

```
                   ┌─────────────────────────────┐
                   │        river compositor      │
                   │  (handles KMS, DRM, input)   │
                   └──────────┬──────────────────┘
                              │ Wayland protocol
                              │ (unix socket)
                   ┌──────────▼──────────────────┐
                   │       wayland-client         │
                   │  (connection, event queue)   │
                   └──────────┬──────────────────┘
                              │ Dispatch::event() calls
                   ┌──────────▼──────────────────┐
                   │         AppData              │
                   │  ├─ river_wm (RiverWindowManagerV1)   │
                   │  ├─ river_xkb (RiverXkbBindingsV1)    │
                   │  └─ wm (WindowManager)                 │
                   └─────────────────────────────┘
                              │
                   ┌──────────▼──────────────────┐
                   │       WindowManager          │
                   │  ├─ windows: VecDeque<Window> │
                   │  ├─ outputs: HashMap<...>   │
                   │  └─ seats: HashMap<...>     │
                   └─────────────────────────────┘
```

### Data flow for a key press:

```
1. User presses Mod4+Space
2. river compositor detects the binding
3. compositor sends RiverXkbBindingV1::Pressed
4. Dispatch<RiverXkbBindingV1>::event() called
5. seat.pending_action = Action::SpawnFoot
6. compositor sends RiverWindowManagerV1::ManageStart
7. handle_manage_start() called
8. manage_seats() calls seat.do_action()
9. do_action() spawns `foot` process
10. proxy.manage_finish() called
```

### Data flow for interactive move:

```
1. User presses Mod4+LeftClick on window
2. compositor sends RiverPointerBindingV1::Pressed
3. seat.pending_action = Action::Move
4. compositor sends ManageStart
5. do_action() finds hovered window, calls pointer_move()
   - Sets SeatOp::Move with start position
   - Calls op_start_pointer()
6. proxy.manage_finish()
7. User drags mouse
8. compositor sends OpDelta { dx, dy }
9. compositor sends RenderStart
10. handle_render_start() applies window.position = start + delta
11. proxy.render_finish()
12. Steps 7-11 repeat each frame
13. User releases mouse button
14. compositor sends OpRelease
15. compositor sends ManageStart
16. op_release = true → op_end() called
17. op = SeatOp::None
18. proxy.manage_finish()
```

### Key design patterns:

| Pattern | Where Used |
|---------|------------|
| **Double-buffered state** | Manage phase: process logic/handle actions. Render phase: apply visual changes. |
| **Deferred action execution** | Binding press → set `pending_action` → execute during next manage. |
| **Delta-based interaction** | `op_dx`/`op_dy` accumulated per frame, applied relative to `start_*` values. |
| **Proxy as key** | `ObjectId` (from `proxy.id()`) used as HashMap key for fast lookups. |
| **Dispatch data routing** | Seat's `ObjectId` passed as dispatch data to child bindings, so event handlers can find the seat directly. |
| **Code generation** | Protocol XML → Rust code via `wayland_scanner` macros at compile time. |

---

## Build & Dependencies

From `Cargo.toml`:

```toml
[dependencies]
bitflags = "2.11.0"        # Bitflag type for Edges, Modifiers
wayland-backend = "0.3.14"  # Low-level Wayland protocol (syscall layer)
wayland-client = "0.31.13"  # High-level Wayland client (Dispatch, Proxy, Connection)
wayland-scanner = "0.31.9"  # Compile-time protocol code generation from XML
```

### Build:

```bash
cd rust
cargo build
cargo run
```

Must be launched by river: `river -c /path/to/tinyrwm/target/debug/tinyrwm`
