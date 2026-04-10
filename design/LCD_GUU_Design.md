# LCD Display GUI Design

The Greenhouse Ventilation Controller is equipped with:
 - 16×2 Character LCD 
 - 4×4 Keypad

## key
### Keypad layout (4×4)

|    | Col 1 | Col 2 | Col 3 | Col 4 |
|---:|:------|:------|:------|:------|
| **Row 1** | 1 | 2 | 3 | A (▲ UP) |
| **Row 2** | 4 | 5 | 6 | B (▼ DOWN) |
| **Row 3** | 7 | 8 | 9 | C (⏎ ENTER) |
| **Row 4** | * (CLR) | 0 | # (OK) | D (← BACK) |

---

### Key mapping / behavior

| Key | Label | Function in menus | Function in edit mode |
|:---:|:------|:------------------|:----------------------|
| A | ▲ UP | Move cursor up / previous item | Increment digit / previous option |
| B | ▼ DOWN | Move cursor down / next item | Decrement digit / next option |
| C | ⏎ ENTER | Confirm selection / enter submenu | Accept edited value and return |
| D | ← BACK | Go back one level / cancel | Cancel edit without saving |
| * | CLR | Return to idle screen from any depth | Backspace / clear last digit |
| # | OK | Shortcut: confirm current item | Confirm value (same as C) |
| 0–9 | Digits | Quick-jump to menu item by number | Enter digit at cursor position |

## Display

### Techncal layout

```
              1
    0123456789012345
   +----------------+
  0|                | -> DDRAM 0x00–0x0F
  1|                | -> DDRAM 0x40–0x4F
   +----------------+
``` 

*Template*

```
   +----------------+
   |                |
   |                |
   +----------------+
```



### Not logged in:

```
   +----------------+
   |PIN:[________]#⏎|
   |              ▼B|
   +----------------+
   +----------------+   +----------------+
   |Act:Normaal   ▲A|   |Act:WINDALARM ▲A|
   |T=22,H=87,W=2 ▼B|   |T=22,H=87,W=2 ▼B|
   +----------------+   +----------------+
   +----------------+
   |T=22,H=87,W=2 ▲A|
   |  °C   %    BF▼B|
   +----------------+
   +----------------+   +----------------+
   |Dak noord:    ▲A|   |Dak noord:    ▲A|
   |  0% Open     ▼B|   |100% Open     ▼B|
   +----------------+   +----------------+
   +----------------+   +----------------+
   |Dak zuid:     ▲A|   |Dak zuid:     ▲A|
   |  0% Open     ▼B|   |100% Open     ▼B|
   +----------------+   +----------------+
   +----------------+   +----------------+
   |Wand noord:   ▲A|   |Wand noord:   ▲A|
   |  0% Open     ▼B|   |100% Open     ▼B|
   +----------------+   +----------------+
   +----------------+
   |Daginstelling:▲A|
   |T=22°C,H=87%  ▼B| - key'#' wil jump directly to "PIN"
   +----------------+
      +----------------+
      |                |
      |                |
      +----------------+
   +----------------+
   |Nachtinst.:   ▲A|
   |T=22°C,H=87%  ▼B| - key'#' wil jump directly to "PIN"
   +----------------+
      +----------------+
      |                |
      |                |
      +----------------+

```

### Loggin in as a farmer:

```
   +----------------+
   |PIN:[________]#⏎|
   |              ▼B|
   +----------------+
   +----------------+   +----------------+   +----------------+
   |Act:Normaal   ▲A|   |Act:WINDALARM ▲A|   |Act:OP:Boer   ▲A|
   |T=22,H=87,W=2 ▼B|   |T=22,H=87,W=2 ▼B|   |T=22,H=87,W=2 ▼B|
   +----------------+   +----------------+   +----------------+
   +----------------+
   |T=22,H=87,W=2 ▲A|
   |  °C   %    BF▼B|
   +----------------+
   +----------------+   +----------------+
   |Dak noord:    ▲A|   |Dak noord:    ▲A|
   |  0% Open     ▼B|   |100% Open     ▼B|
   +----------------+   +----------------+
   +----------------+   +----------------+
   |Dak zuid:     ▲A|   |Dak zuid:     ▲A|
   |  0% Open     ▼B|   |100% Open     ▼B|
   +----------------+   +----------------+
   +----------------+   +----------------+
   |Wand noord:   ▲A|   |Wand noord:   ▲A|
   |  0% Open     ▼B|   |100% Open     ▼B|
   +----------------+   +----------------+
   +----------------+
   |Daginstelling:▲A|
   |T=22°C,H=87%  ▼B|
   +----------------+
   +----------------+
   |Nachtinst.:   ▲A|
   |T=22°C,H=87%  ▼B| - key'#' wil jump directly to "PIN"
   +----------------+

```

### Logged in as Technician:

```
   +----------------+
   |PIN:[________]#⏎|
   |              ▼B|
   +----------------+
   +----------------+   +----------------+   +----------------+
   |Act:Normaal   ▲A|   |Act:WINDALARM ▲A|   |Act:OP:Tech   ▲A|
   |T=22,H=87,W=2 ▼B|   |T=22,H=87,W=2 ▼B|   |T=22,H=87,W=2 ▼B|
   +----------------+   +----------------+   +----------------+
