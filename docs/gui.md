# Luminary OS — GUI Subsystem

The GUI is a kernel-mode compositor and window manager implemented in `src/kernel/gui.c`. It runs as a kernel task at priority 9 and communicates with userspace via the window syscalls (SYS_WIN_*).

## Three-Buffer Rendering

The compositor uses three distinct pixel buffers:

- **`back`** — clean scene buffer. Desktop background + all window chrome + client backbuffers are composited here. The cursor is **never** drawn into `back`.
- **`fb_hw`** — hardware framebuffer (VBE LFB). The final output displayed on screen. Cursor is drawn directly here after blitting from `back`.
- **Per-window `backbuffer`** — client area pixels only. Applications write here via syscalls (WIN_FILL_RECT, WIN_DRAW_TEXT, etc.). The compositor blits these into `back` during compositing.

**Invariant**: `back` is always cursor-free. `draw_cursor()` and `hw_putpixel()` write only to `fb_hw`. Do not break this.

## Composite Strategy

Two rendering paths depending on what changed:

**Full scene** (triggered by `scene_dirty`):
1. Draw desktop background into `back`
2. Draw all windows (chrome + client backbuffers) into `back`
3. `memcpy back → fb_hw`
4. Draw cursor on `fb_hw`

Triggered by structural changes: window create/destroy/move/resize/focus change.

**Partial update** (triggered by `compositor_dirty` only):
1. For each dirty window: redraw just that window into `back`
2. `blit_rows(window rows)` from `back → fb_hw`
3. Cursor rows are always included in the blit range
4. If cursor overlaps the blitted region, redraw cursor on `fb_hw`

No `draw_desktop()` call in the partial path — avoids a full-screen blit for routine window content updates.

**Key rule**: `gui_window_flip()` calls `gui_wake()`, not `gui_wake_scene()`. Window content updates never trigger a full scene redraw. Only structural changes call `gui_wake_scene()`.

## Dirty Flags

- `compositor_dirty`: any update needed (cursor move, window flip, etc.)
- `scene_dirty`: structural change requiring full redraw

## Back Buffer Allocation

The `back` buffer is allocated early in `init_gui()` before PMM fragmentation, obtaining physically contiguous identity-mapped frames. This is performance-critical: scattered physical frames cause TLB thrashing on the frequent `memcpy back → fb_hw`.

The back buffer is **not freed** when the compositor exits — it is owned by `init_gui()` for the lifetime of the kernel. This is intentional (see Known Bugs in `docs/roadmap.md`).

## Window Backbuffer Allocation

Per-window backbuffers are allocated via `bb_alloc(cw, ch, &nframes)` and freed via `bb_free(buf, nframes)`. These use `vmm_alloc_pages(n)` which maps non-contiguous physical frames to a contiguous virtual range at `0xC0000000+`. Do not use `kmalloc`/`kfree` for backbuffers.

`struct window` stores `bb_nframes` to track the frame count for deallocation.

## Compositor Lifecycle

- Started lazily on the first `gui_window_create()` call
- Exits via `task_kill(self)` when the last window is closed; clears the screen first; sets `compositor_task_ptr = NULL`
- Restarted by the next `gui_window_create()`

## Statusbar

22px bar at the top of the screen, always drawn, not part of any window:
- Left: taskbar buttons (one per window, up to `TASKBAR_BTN_MAX_W=140px` each) — click to focus
- Right: "Quit" button (`SB_CLOSE_W=54px`) — destroys the focused window
- Focused window button highlighted with `COL_SB_BTN_FOCUS`

## Window Management

- **Move**: left-click and drag the title bar
- **Resize**: `RESIZE_HIT_PX=6` pixels inside/outside the window border. Cursor changes to ↔/↕/↘ sprite during hover. Backbuffer realloc happens on mouse release (not during drag). `GUI_EVENT_RESIZE` is pushed to the window after realloc.
- **Close**: left-click the X button in the title bar
- **Focus**: focus-follows-mouse — `focus_window()` called when the hovered window changes
- Minimum size: `WIN_MIN_W=120`, `WIN_MIN_H=GUI_TITLE_HEIGHT+GUI_BORDER*2+24`

## Cursor Sprites

All sprites are drawn directly into `fb_hw` (never `back`) with a 1px black outline pass followed by white fill via `hw_putpixel()`:

| Cursor | Size | Constant |
|--------|------|----------|
| Normal arrow | 12×19 | `cursor_mask` |
| Horizontal resize | 11×7 | `cursor_resize_h` (centred hotspot) |
| Vertical resize | 7×11 | `cursor_resize_v` (centred hotspot) |
| Diagonal resize | 11×11 | `cursor_resize_d` (centred hotspot) |

## Public API

Called from syscall handlers in `syscall.c`:

```c
void     init_gui(void);
int      gui_window_create(int x, int y, int w, int h, const char *title);
void     gui_window_destroy(int id);
void     gui_window_fill_rect(int id, int x, int y, int w, int h, uint32_t color);
void     gui_window_draw_rect(int id, int x, int y, int w, int h, uint32_t color);
void     gui_window_draw_text(int id, int x, int y, const char *str, uint32_t fg, uint32_t bg);
void     gui_window_flip(int id);
int      gui_window_poll_event(int id, struct gui_event *ev);
void     gui_window_get_size(int id, int *cw, int *ch);
int      gui_has_windows(void);
```

## Event Types (`struct gui_event`)

| Type | Value | Fields |
|------|-------|--------|
| `GUI_EVENT_KEYPRESS` | 1 | `key` = ASCII char |
| `GUI_EVENT_MOUSE_BTN` | 2 | `x`/`y` relative to client area, `buttons` = MOUSE_BTN_* flags |
| `GUI_EVENT_RESIZE` | 3 | `x` = new client width, `y` = new client height |

## Keyboard Routing

While `gui_has_windows()` is true, `SYS_READ` (blocking keyboard read used by the shell) yields without consuming input. All keyboard input goes to the compositor's `process_keyboard()`, which routes to the focused window's event queue.

## Drawing Coordinates

GUI drawing functions (`fb_fill_rect`, `fb_draw_char`, etc.) use signed `int32_t` coordinates and clip against all four screen edges. Windows dragged off any edge render correctly.

## GUI Demo Application (`userland/gui.c`)

Four windows, event-driven with a 1s periodic redraw timer:

- **win1** — "Demo: Buttons": Hello and Clear buttons, label display
- **win2** — "Uptime": live uptime counter in whole seconds
- **win3** — "Demo: Text Input": text field, submit button
- **win4** — "Console": terminal emulator with scrollback (256 lines), built-in commands: `help`, `clear`, `uptime`, `pid`

Console implementation: `con_lines[256][81]` ring buffer, `con_line_count`, `con_input[128]`. `draw_win4()` renders scrollback + input prompt using `win_get_size` for dynamic layout. Handles `GUI_EVENT_KEYPRESS` and `GUI_EVENT_RESIZE`.
