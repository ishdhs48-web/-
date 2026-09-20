# VantaESP

External overlay ESP DLL for Unity IL2CPP games (dump from `Assembly-CSharp`).  
Targets `GameManager.players` → `Dictionary<int, PlayerManager>` — reads `username`, `dead`, `hpRatio`, world position via transform cached pointer chain.

No ImGui. GDI+ overlay window with colorkey transparency, always-on-top, follows game window.

---

## Features

| Toggle | Default | Key |
|---|---|---|
| Master ESP | ON | INSERT (menu) |
| Boxes | ON | menu |
| Names | ON | menu |
| Health bars | ON | menu |
| Distance | ON | menu |
| Show dead | OFF | menu |
| Tracers | OFF | menu |

**Controls**
- `INSERT` — open/close menu
- `NUMPAD 8 / 2` — navigate up/down
- `NUMPAD 5` — toggle selected item
- `END` — unload DLL

---

## Build locally

Requirements: Windows, Visual Studio 2022 (MSVC), CMake ≥ 3.20.

```bat
cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build/VantaESP.dll`

---

## Build via GitHub Actions

Push to `main` → Actions tab → `Build VantaESP` → download artifact from the run.

---

## Injection

Any standard DLL injector works (manual map or LoadLibrary). The DLL auto-detects the game window (`Len's Island` / `UnityWndClass`) and spawns the overlay thread.

---

## Per-build derivation notes

Two offsets **must be re-derived** when the game updates:

1. **Transform position** (`cached_ptr + 0x90`) — dump with Il2CppDumper, find `UnityEngine.Transform::get_position`, follow the internal engine struct. Common range: `0x80`–`0xA0`.

2. **Vtable slots** for `get_position` (`0x24`) and `WorldToScreenPoint` (`0x4C`) — attach x64dbg, set BP on `Camera::WorldToScreenPoint`, check vtable index from the klass pointer. These drift by 1–3 slots between Unity patch versions.

3. **Dictionary `_entries` layout** — confirmed for Unity 2020.3 IL2CPP x64. If the game is on Unity 2021+ the entry stride may be `0x20` instead of `0x18` (alignment change). Adjust `ENTRY_SIZE` in `dict_entries()`.

4. **`PlayerManager.hpRatio` field name** — tried `"hpRatio"` then `"currentHpRatio"`. If both return null, run Il2CppDumper on the current build and search for `SetHpRatio` to find the backing field name.
