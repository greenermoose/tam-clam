# tam-clam key bindings

`tam-clam` follows the Tamlinux-wide keybinding standard in
`tamlinux/arch/keybindings.md`. This document specifies how `tam-clam`
implements that standard in its line editor (`src/edit/`, 0.2.0+),
which universal `Ctrl` chords are active vs. no-op at the shell prompt,
the shell-specific bindings added on top, the `termios` configuration
for child processes, and the `libtam` input mapping.

## Principles (from `tamlinux/arch/keybindings.md`)

- **No double-acting `Ctrl` codes.** Every `Ctrl` key keeps its single
  Tamlinux meaning. Universal `Ctrl` chords that do not apply at a shell
  prompt (`Ctrl+D` close, `Ctrl+N` new, `Ctrl+O` open, `Ctrl+R` replace,
  `Ctrl+S` save, `Ctrl+U` update, and reserved `Ctrl+B`/`T`/`W`) are
  no-ops rather than taking on unrelated meanings.
- **Three-physical-key ceiling.** Every default binding uses at most two
  modifiers plus one primary key (including any `Shift` needed to type a
  symbol).
- **Physical key names only.** All docs, guidance (`Ctrl+G`), and `bind`
  commands use `Meta`, `Ctrl`, `Alt`, `Shift`, and named keys (`Enter`,
  `Backspace`, `Delete`, `Tab`, `Escape`, `Left`, `Home`, ...).
- **Named widgets.** Every editor action has a canonical lowercase
  hyphenated widget name so users can search/run it via `Ctrl+E`
  (`execute`) or rebind it.
- **One default map, expressive rebinding.** No vi mode and no built-in
  readline map. The `bind` format supports single keys and multi-key
  chords so an optional user-supplied readline file can express chords
  such as `Ctrl+X Ctrl+E` later.
- **First-class `TERM=linux` support.** Every essential action has an
  `Alt` or plain-key path that works on the Linux console where
  `Ctrl+Nav` and `Shift+Nav` are not distinguished and `Alt+Left/Right`
  and `Alt+F1`-`F12` are captured by the kernel VT driver.

## Terminal modes and `termios`

1. **While editing at the prompt (`tam-clam` line editor):**
   - `tam-clam` enables raw mode via `libtam`'s `tam_tty_raw(t, true)`
     (`ISIG` and `IXON` disabled) and restores the saved `termios` on
     every exit path (including signals and fatal errors).
   - `Ctrl+C`, `Ctrl+Z`, `Ctrl+S`, `Ctrl+Q`, `Ctrl+P`, and `Ctrl+\`
     arrive as ordinary bytes.
   - `tam-clam` enables bracketed paste via `tam_tty_paste(t, true)` so
     pasted text is inserted literally into the buffer without running
     embedded newlines until the user presses `Enter`.
2. **While running a foreground child command:**
   - Before handing the terminal to a foreground job, `tam-clam`
     configures the PTY `termios` per `tamlinux/arch/keybindings.md`:
     - `VINTR` = `Ctrl+\` (`0x1C`) -> `SIGINT` (**Break**)
     - `VQUIT` = `_POSIX_VDISABLE` (so `Ctrl+\` interrupts cleanly
       without dumping core)
     - `VSUSP` = `Ctrl+P` (`0x10`) -> `SIGTSTP` (**Pause**)
   - `Ctrl+C` and `Ctrl+Z` are never kernel signals locally; they stay
     Copy and Undo across Tamlinux.

## Universal `Ctrl` map in `tam-clam`

The four legacy ASCII collisions (`Ctrl+H` == `Backspace`, `Ctrl+I` ==
`Tab`, `Ctrl+J`/`Ctrl+M` == `Enter`, `Ctrl+[` == `Escape`) are never
assigned separately. All 22 ASCII-clean `Ctrl+<letter>` slots plus
`Ctrl+\` follow `tamlinux/arch/keybindings.md` verbatim:

| Key | Widget | Behavior in `tam-clam` | Phase |
| --- | --- | --- | --- |
| `Ctrl+A` | `select-all` | Select entire command buffer | 0.2.0 |
| `Ctrl+B` | *(reserved)* | No-op | — |
| `Ctrl+C` | `copy` | Copy selection (or current line if no selection); OSC 52 write + internal clipboard | 0.2.0 |
| `Ctrl+D` | `close` | No-op (shell has no tabs/buffers; `Ctrl+Q` quits) | — |
| `Ctrl+E` | `execute` / `chord-compose` | Open unified one-handed chord composer & command menu (`Tab` cycles modifiers, `Up`/`Down` + `Enter` runs any widget, `Escape` / empty `Backspace` cancels) | 0.2.0 |
| `Ctrl+F` | `find` | Incremental history search (prompt); next match while searching | 0.2.1 |
| `Ctrl+G` | `guidance` | Show `tam-clam` keybinding guidance / contextual help | 0.2.0 |
| `Ctrl+K` | `kill-line` | Clear current prompt buffer (saves to clipboard/undo) | 0.2.0 |
| `Ctrl+L` | `clear-screen` | Clear terminal screen and redraw prompt + buffer | 0.2.0 |
| `Ctrl+N` | `new` | No-op | — |
| `Ctrl+O` | `open` | No-op (`Alt+E` opens buffer in `$VISUAL`/`$EDITOR`) | — |
| `Ctrl+P` | `pause` | Foreground child `VSUSP` (`SIGTSTP`); no-op at prompt | 0.2.2 |
| `Ctrl+Q` | `quit` | Exit `tam-clam` when prompt is empty (no-op when non-empty) | 0.2.0 |
| `Ctrl+R` | `replace` | No-op at prompt | — |
| `Ctrl+S` | `save` | No-op (`IXON` disabled in raw mode) | — |
| `Ctrl+T` | *(reserved)* | No-op | — |
| `Ctrl+U` | `update` | No-op | — |
| `Ctrl+V` | `paste` | Paste from clipboard, replacing selection if any (bracketed paste in `tam-last`) | 0.2.0 |
| `Ctrl+W` | *(reserved)* | No-op | — |
| `Ctrl+X` | `cut` | Cut selection (or current line if no selection); OSC 52 write + internal clipboard | 0.2.0 |
| `Ctrl+Y` | `redo` | Redo last undone edit | 0.2.0 |
| `Ctrl+Z` | `undo` | Undo last edit | 0.2.0 |
| `Ctrl+\` | `break` | Foreground child `VINTR` (`SIGINT`); at prompt, cancels current buffer like `kill-line` | 0.2.0 |

## Navigation, selection, and deletion

| Category | Primary key | `TERM=linux` / compact fallback | Widget | Phase |
| --- | --- | --- | --- | --- |
| **Char move** | `Left` / `Right` | `Left` / `Right` | `char-left` / `char-right` (`Right` at EOL accepts suggestion in 0.2.4) | 0.2.0 / 0.2.4 |
| **Word move** | `Ctrl+Left` / `Ctrl+Right` | `Alt+,` / `Alt+.` | `word-left` / `word-right` (`Ctrl+Right` at EOL accepts suggestion word in 0.2.4) | 0.2.0 / 0.2.4 |
| **Line move** | `Home` / `End` | `Home` / `End` | `line-start` / `line-end` | 0.2.0 |
| **Buffer move** | `Ctrl+Home` / `Ctrl+End` | `Alt+<` / `Alt+>` | `buffer-start` / `buffer-end` | 0.2.0 |
| **Line / history** | `Up` / `Down` | `Up` / `Down` | `up-or-history-prev` / `down-or-history-next` | 0.2.0 / 0.2.1 |
| **Select char** | `Shift+Left` / `Shift+Right` | `Alt+M` then `Left` / `Right` | `select-char-left` / `select-char-right` | 0.2.0 |
| **Select word** | `Ctrl+Shift+Left` / `Ctrl+Shift+Right` | `Alt+M` then `Alt+,` / `Alt+.` | `select-word-left` / `select-word-right` | 0.2.0 |
| **Select line** | `Shift+Home` / `Shift+End` | `Alt+M` then `Home` / `End` | `select-line-start` / `select-line-end` | 0.2.0 |
| **Select all** | `Ctrl+A` | `Ctrl+A` | `select-all` | 0.2.0 |
| **Mark toggle** | `Alt+M` | `Alt+M` | `toggle-mark` (anchors/clears keyboard selection) | 0.2.0 |
| **Delete char** | `Backspace` / `Delete` | `Backspace` / `Delete` | `delete-char-left` / `delete-char-right` (deletes selection if active) | 0.2.0 |
| **Delete word** | `Ctrl+Backspace` / `Ctrl+Delete` | `Alt+Backspace` / `Alt+D` | `delete-word-left` / `delete-word-right` | 0.2.0 |
| **Kill line** | `Ctrl+K` | `Ctrl+K` | `kill-line` | 0.2.0 |
| **Delete partial** | `Alt+K` / `Alt+Shift+K` | `Alt+K` / `Alt+Shift+K` | `delete-to-line-end` / `delete-to-line-start` | 0.2.0 |

## Shell-specific bindings

| Key | Widget | What it does | Phase |
| --- | --- | --- | --- |
| `Enter` | `accept-line` | Submit buffer if syntax is complete; otherwise insert newline | 0.2.0 |
| `Alt+Enter` | `insert-newline` | Always insert a literal newline (multi-line editing; works on `TERM=linux`) | 0.2.0 |
| `Escape` | `cancel-mode` | Dismiss guidance, command palette, history search, completion menu, or active selection (keeps buffer) | 0.2.0 |
| `Alt+E` | `open-in-editor` | Edit current buffer in `$VISUAL` / `$EDITOR` | 0.2.0 |
| `Alt+V` | `paste-prev` | Cycle to older clipboard/kill-ring entry immediately after `paste` | 0.2.0 |
| `Alt+Q` | `insert-verbatim` | Insert next key/byte literally (e.g. a real Tab or Escape) | 0.2.0 |
| `Alt+#` | `comment-line` | Prefix buffer line(s) with `# ` and submit to history | 0.2.0 |
| `Alt+L` | `insert-last-arg` | Insert last word of previous command (repeat to walk further back) | 0.2.1 |
| `Alt+N` / `Alt+Shift+N` | `find-next` / `find-prev` | Next / previous match during `Ctrl+F` history search | 0.2.1 |
| `Tab` | `complete-next` | Trigger completion / cycle forward | 0.2.3 |
| `Shift+Tab` | `complete-prev` | Cycle completion backward | 0.2.3 |

Note on `Alt+L`: `Alt+.` is `word-right` in `tamlinux/arch/keybindings.md`,
so `insert-last-arg` uses `Alt+L` (**L**ast argument) to stay conflict-free.

## Clipboard and OSC 52 policy

- `cut` (`Ctrl+X`) and `copy` (`Ctrl+C`) always store text in `tam-clam`'s
  internal clipboard ring **and** emit an **OSC 52 write** sequence
  (`ESC ] 52 ; c ; <base64> BEL`) when not on `TERM=linux`, syncing with
  the Wayland clipboard under `tam-last` / `foot`.
- `tam-clam` never requests **OSC 52 read** (disabled by default in
  `tam-last` for security). Pasting external text from `tam-last` arrives
  via bracketed paste; pressing `Ctrl+V` inside `tam-clam` pastes from
  `tam-clam`'s internal clipboard ring (with `Alt+V` cycling older
  entries).

## Rebinding format (`bind` / key files)

`tam-clam` uses the shared Tamlinux line-oriented format from
`tamlinux/arch/keybindings.md`:

```text
# Comments start with #
bind Ctrl+X cut
bind Ctrl+C copy
bind Ctrl+V paste
bind Ctrl+Z undo
bind Ctrl+Y redo
bind Ctrl+G guidance
bind Shift+Left select-char-left
bind "Ctrl+X Ctrl+E" open-in-editor
unbind Alt+#
```

- Canonical modifier order: `Meta+Ctrl+Alt+Shift+<Key>` (`Meta` is parsed
  by the format for completeness, though `tam-pane` owns it).
- Space-separated multi-key sequences inside quotes allow optional
  user-supplied key files (including a readline compatibility file)
  without baking alternate modes into `tam-clam`.

## `libtam` (`tam_key`) notes for 0.2.0

Every key in the tables above decodes through `libtam/src/tam_key.c`
except for one small gap to close in `0.2.0`:

- **Bracketed paste markers (`CSI 200~` / `CSI 201~`):** `tam_key.c`
  currently ignores unknown `CSI <n> ~` parameters. For `0.2.0`, either
  add `TAM_KEY_PASTE_START` / `TAM_KEY_PASTE_END` to `libtam`'s
  `tam_key` or intercept `CSI 200~` / `CSI 201~` in `tam-clam`'s input
  reader before feeding `tam_key`.
- **`Ctrl+Backspace`:** Terminals vary (`0x08`, `CSI 127;5u`, or `0x7F`
  identical to `Backspace`). `Alt+Backspace` (`ESC 0x7F` / `ESC 0x08`)
  already decodes cleanly in `tam_key` everywhere and serves as the
  guaranteed fallback on both `tam-last` and `TERM=linux`.
