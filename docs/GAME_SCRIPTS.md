# The game's scripts and menus

Proving Ground's menus, options and most game flow are QB scripts (Neversoft's script
language), loaded from `DATA\COMPRESSED\*.pak.xen`. This is how they are stored and run, found
in the generated code, and how a native hook changes or extends them without touching the game's
files. `src/pg_camera.cpp` (the Project 8 camera mode) is the worked example.

## Checksums

Every name in a script is a 32-bit checksum: CRC-32 of the lowercase name without the final
xor, `zlib.crc32(name.lower()) ^ 0xFFFFFFFF`. Proving Ground's `dbg.pak` has the table of names
back from checksums. In QB files checksums are big-endian; inside script bytecode they are
little-endian.

## Symbols

All scripts, structures, arrays and global values live in one symbol table. Its address is in
the word at `0x82B79DF0`: 0x8000 buckets (index `checksum & 0x7FFF`), each a chain of entries:

| Offset | What |
| --- | --- |
| +2 | type byte (1 int, 2 float, 3 string, 4 wide string, 5 pair, 6 vector, 7 script, 10 structure, 12 array, 13 checksum, 26 pointer to another symbol) |
| +4 | checksum |
| +12 | value (for a script: its data, below) |
| +16 | next entry in the bucket |

`sub_82360408(checksum)` looks a symbol up (used for structures and arrays). The script cache
(`sub_823437C0`) walks the table itself, following type 26 pointers.

## Script data and the cache

A script's value points to `{checksum, size, packed size}` (big-endian) followed by the bytes.
When packed size < size they are LZSS packed (4 KiB window, 18-byte matches, ring start 4078,
flag bit 1 = literal: `sub_82498FD0`). Before running, the cache (`sub_823437C0`) unpacks into
its own copy, keyed by the script's name; when size <= packed size it copies the bytes as they
are. So an unpacked replacement is simply `{checksum, n, n, bytes}`.

Replacing a script: put a copy in guest memory
(`rex::system::kernel_memory()->SystemHeapAlloc`) and store its address at the symbol's +12.
Do it before the script first runs (the cache keeps what it unpacked); `pg_camera.cpp` checks
once per main-loop frame, so a reloaded QB is patched again at once. Build the copy from the
game's own script at run time and check the bytes you expect, so nothing from the game is
shipped and an unexpected version is left alone.

## Bytecode

Tokens used by the menu scripts (one byte each unless noted):

| Byte | Token |
| --- | --- |
| 01 | end of line |
| 03 / 04 | `{` / `}` structure |
| 07 | `=` |
| 0B | `+` |
| 0E / 0F | `(` / `)` |
| 12 / 14 | `<` / `>` |
| 16 + u32 | name (checksum) |
| 17 + s32 | integer |
| 1A + f32 | float |
| 1B + u32 length + bytes | string |
| 24 | end of script |
| 28 | endif |
| 29 | return |
| 2D | `<...>` argument prefix (`<Mode>`) |
| 3C / 3D | switch / endswitch |
| 3E / 3F | case / default |
| 42 | `.` member call (`SkaterCam0.SC_GetMode`) |
| 47 + u16 | if (offset to the else or endif) |
| 48 + u16 | else (offset past endif) |
| 49 + u16 | jump (`case`'s skip to the next case; `break`) |
| 4A + u16 + bytes | packed structure (`{NEAR = 6.0 ...}` in one block) |
| 4C + u32 length + UTF-16BE | wide string |

Jump offsets are little-endian and relative to the byte after the token (the target is
`position + 1 + offset`). Inserting bytes means moving every jump that crosses them.

The disassembler and packers used to find all this are small Python scripts (LZSS, QB items,
bytecode); they are not in the repository because they read the game's own files.

## The camera mode menu (worked example)

Options > Controls > Camera mode runs two scripts from `gamemenu_options.qb`:

- `menu_alter_camera_mode`: `SkaterCam0.SC_GetMode`, then on Left
  `if (<Mode> > 2) Mode = (<Mode> - 1) else Mode = 4`, on Right
  `if (<Mode> < 4) Mode = (<Mode> + 1) else Mode = 2`, then `SkaterCam0.SC_SetMode Mode = <Mode>`,
  `Change player_desired_camera_mode = <Mode>`, `get_camera_mode_text` and
  `SetScreenElementProps Id = {menu_skater_camera_mode child = extra_text} Text = <camera_text>`.
- `get_camera_mode_text`: `switch <Mode>`: case 2 `"STANDARD"`, case 4 `"CLASSIC"`, default
  `"DYNAMIC"` (mode 3, the default).

Natively, `SC_SetMode` is `sub_82135898` (reads `Mode`, then `sub_82134728(component, mode)`),
`SC_GetMode` is `sub_821353D8` (returns component+24), and `sub_8222A648` / `sub_8222AF18`
load and save `player_desired_camera_mode` in the profile as an integer.
`sub_82135908` steps 1..4 and does nothing at 5 or more.

`pg_camera.cpp` makes the steps 2..5 (two integers) and adds `case 5: "PROJECT 8"` to the text
switch; mode 5 is set up as Standard with Project 8's values and the component keeps 5.

## Adding an option to a game menu

The same approach adds any setting to the game's own menus:

1. Find the menu's scripts (the item's `Id`, its `*_alter_*` and `get_*_text` scripts).
2. Give the setting a value the scripts can step through, stored where the game already stores
   options (a global the profile saves, like `player_desired_camera_mode`) or in our own config.
3. Replace the scripts with patched copies as above, and act on the value natively.

New menu items (rather than new values for an existing one) need the menu's creation script
too: it builds the items with `CreateScreenElement`. That is the way to an integrated graphics
menu; see [GAME_GRAPHICS.md](GAME_GRAPHICS.md) for the commands such a menu would call.

## Script commands

Script commands are native functions in a table of `{name, function}` pairs around
`0x828AF000-0x828B3000` (1508 of them). For example `SetScreenElementProps` is `sub_8234B718`,
`CreateScreenElement` and the other screen element commands are next to it. A native function
receives the parameters structure in r3 and reads values with the getters below
(structure r3, checksum r4, out r5, required r6), returning 1 if found:

| Function | Reads |
| --- | --- |
| `sub_8232E138` | integer |
| `sub_8232E1A0` | float |
| `sub_820D94A0` | TRUE/FALSE checksum as a byte |
| `sub_8232E058` | checksum |
| `sub_8232EA48` | whether a flag name is present (`GotParam`-style) |
| `sub_8232FF78` | adds an integer to a structure (return values: `SC_GetMode`) |
