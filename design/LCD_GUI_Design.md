# Greenhouse Controller - LCD-GUI Design

## 1. Overview

### 1.1 Purpose
The LCD-GUI provides a local user interface for monitoring and basic control of the greenhouse system. The interface is designed for a 16×2 character LCD display with 4×4 matrix keypad.

### 1.2 Design Principles
- **Auto-Rotation**: Status and settings screens rotate automatically every 5 seconds
- **Manual Navigation**: Up/Down buttons allow manual scrolling through screens
- **Minimal Input**: Essential parameters only, optimized for keypad entry
- **Role-Based Access**: Different access levels for public viewing, farmer, and technician
- **Clear Feedback**: Immediate visual confirmation of actions
- **Screen Timeout**: Returns to auto-rotation after 30 seconds of inactivity

---

## 2. User Roles & Access Levels

### 2.1 Public (No Login Required)
- **Access**: Read-only monitoring
- **Available Screens** (auto-rotating):
  - Current operation status
  - Temperature, humidity, wind readings
  - Ventilation positions (roof, side)
  - Day/night setpoints (view only)
  - WiFi AP and client status
  - Active alarms (if any)

### 2.2 Farmer (PIN-based Login)
- **Access**: Basic operational control and climate safety settings
- **Authentication**: 6-digit PIN
- **Available Settings**:
  - Day temperature minimum (T_min_day)
  - Day temperature maximum (T_max_day)
  - Night temperature minimum (T_min_night)
  - Night temperature maximum (T_max_night)
  - Day humidity minimum (RH_min_day)
  - Day humidity maximum (RH_max_day)
  - Night humidity minimum (RH_min_night)
  - Night humidity maximum (RH_max_night)
  - Enable/disable humidity control
  - Wind protection: Enable / Disable (confirmation required; action is logged)
  - Conflict resolution priority: Temperature first (default) / Humidity first / Auto (deviation-based)
  - Change farmer PIN
  - Logout

> **Note:** Geographic location (for sunrise/sunset calculation), sensor poll interval, sliding average window, and window dwell times are configurable via the web GUI only — they are not accessible through the LCD menu.

> **Note:** The current day/night period and the computed sunrise/sunset times for today can be viewed in the web GUI.

### 2.3 Technician (PIN-based Login)
- **Access**: System configuration (view farmer settings, manage AP and wind protection)
- **Authentication**: 6-digit PIN (different from Farmer PIN)
- **Available Settings**:
  - View farmer settings (read-only)
  - Enable/disable WiFi Access Point
  - Wind protection: Enable / Disable (confirmation required; action is logged)
  - Change technician PIN
  - Logout

> **Note:** Sensor poll interval (30–3600 s), sliding average window (1–60 min), and window dwell times are configurable via the web GUI only — they are not accessible through the LCD menu.

---

## 3. Hardware Components

### 3.1 LCD Display
- **Type**: 16×2 Character LCD (HD44780 compatible)
- **Resolution**: 16 columns × 2 rows
- **Character Set**: Standard ASCII + custom characters
- **Backlight**: Auto-dimming (full brightness when active, dim after 30 seconds idle)

**Technical Layout**:
```
              1
    0123456789012345
   +----------------+
  0|                | -> DDRAM 0x00–0x0F
  1|                | -> DDRAM 0x40–0x4F
   +----------------+
```

**Template**:
```
   +----------------+
   |                |
   |                |
   +----------------+
```

### 3.2 Keypad (4×4 Matrix)

**Physical Layout**:

|    | Col 1 | Col 2 | Col 3 | Col 4 |
|---:|:------|:------|:------|:------|
| **Row 1** | 1 | 2 | 3 | A (▲ UP) |
| **Row 2** | 4 | 5 | 6 | B (▼ DOWN) |
| **Row 3** | 7 | 8 | 9 | C (⏎ ENTER) |
| **Row 4** | * (CLR) | 0 | # (OK) | D (← BACK) |

**Key Functions**:

| Key | Label | Function in Navigation | Function in Edit Mode |
|:---:|:------|:-----------------------|:----------------------|
| **A** | ▲ UP | Previous screen / scroll up | Increment digit |
| **B** | ▼ DOWN | Next screen / scroll down | Decrement digit |
| **C** | ⏎ ENTER | Select / confirm | Accept edited value |
| **D** | ← BACK | Go back / cancel | Cancel without saving |
| **#** | OK | Quick confirm | Same as ENTER |
| **\*** | CLR | Return to first screen | Clear/backspace |
| **0–9** | Digits | Direct PIN entry | Enter digit value |

---

## 4. Display Screens (Public View - Auto-Rotating)

**Auto-Rotation**: Screens automatically cycle every 5 seconds when no user interaction
**Manual Control**: Press ▲ (up) or ▼ (down) to navigate manually (pauses auto-rotation for 30 seconds)

### 4.1 Screen 1: PIN Entry (Initial Screen)

```
   +----------------+
   |PIN:[      ]  # |
   |Guest:Press ▼ B |
   +----------------+
```

**Description**:
- Default screen when system powers on or after logout
- User enters 6-digit PIN using number keys (0-9)
- Press # or ENTER to confirm PIN
- Press ▼ to skip login and view as guest (read-only)
- PIN automatically determines user role (Farmer/Technician)
- Display shows `●●●●●●` (filled dots) as digits are entered

**Feedback**:
- Valid PIN: Shows "Login OK" for 1 second, then proceeds to role-specific view
- Invalid PIN: Shows "Wrong PIN!" for 2 seconds, clears entry
- 3 failed attempts: Shows "Locked 5min" and locks keypad

### 4.2 Screen 2: System Status

```
   +----------------+
   |Sys:OPERATIONAL |
   |T=23°C H=65% W=3|
   +----------------+
```

**Status Values**:
- **OPERATIONAL**: Normal operation
- **WIND ALARM**: Wind speed safety limit exceeded
- **HIGH TEMP**: Temperature above threshold
- **HIGH HUMID**: Humidity above threshold
- **SENSOR FAIL**: Sensor communication error
- **MANUAL**: Manual control mode active

**Data Display**:
- T = Temperature (interior, °C, integer)
- H = Humidity (interior, %, integer)
- W = Wind speed (exterior, Beaufort scale, integer)

### 4.3 Screen 3: Ventilation Status - Roof

```
   +----------------+
   |Roof Vent: AUTO |
   |Position:40%    |
   +----------------+
```

**Mode Values**: AUTO, MANUAL, LOCKED
**Position**: 0% (closed) to 100% (fully open)

### 4.4 Screen 4: Ventilation Status - Side

```
   +----------------+
   |Side Vent: AUTO |
   |Position:25%    |
   +----------------+
```

**Mode Values**: AUTO, MANUAL, LOCKED
**Position**: 0% (closed) to 100% (fully open)

### 4.5 Screen 5: Day Setpoints (View Only - Public)

```
   +----------------+
   |Day Setpoints:  |
   |T:22°C  H:70%   |
   +----------------+
```

**Note**: Press # to jump to PIN entry for editing (Farmer access required)

### 4.6 Screen 6: Night Setpoints (View Only - Public)

```
   +----------------+
   |Night Setpoints:|
   |T:18°C  H:70%   |
   +----------------+
```

**Note**: Press # to jump to PIN entry for editing (Farmer access required)

### 4.7 Screen 7: WiFi Status

```
   +----------------+
   |WiFi AP: ON     |
   |SSID:Green.-XXXX|
   +----------------+
```

**AP Status Values**:
- **ON**: Access Point is enabled (for technician access)
- **OFF**: Access Point is disabled

**Client Status Values**:
- **Connected**: Shows network SSID name (up to 10 characters)
- **Disconnected**: No WiFi connection
- **Connecting...**: Attempting to connect

**State Examples**:
WiFi client disbled:
```
   +----------------+
   |WiFi: OFF SSID: |
   |DISCONNECTED    |
   +----------------+
```

WiFi client enabled, connecting to AP:
```
   +----------------+
   |WiFi: ON  SSID: |
   |CONNECTING      |
   +----------------+
```

WiFi client enabled, connected to ssid "HomeWifi":
```
   +----------------+
   |WiFi: ON  SSID: |
   |HomeWifi        |
   +----------------+
```

WiFi client enabled, connected to ssid "HomeWifi" received ip address 10.0.12.11:
```
   +----------------+
   |WiFi: ON  IP:   |
   |10.0.12.11      |
   +----------------+
```

**Note**: AP can be toggled by technician (requires PIN login)

### 4.8 Screen 8: Active Alarms (if any)

```
   +----------------+
   |ALARM: HIGH HUM |
   |Value: 92% > 90%|
   +----------------+
```

**Alarm Types**:
- HIGH TEMP (above threshold)
- LOW TEMP (below threshold)
- HIGH HUM (high humidity)
- LOW HUM (low humidity)
- WIND ALARM (speed above safe limit)
- SENSOR FAIL (communication error)
- POWER FAIL (backup power active)

**Display**: Shows most critical alarm first
**Cycling**: If multiple alarms, rotates every 3 seconds

---

## 5. Settings Pages (After Login)

### 5.1 Farmer Menu

After successful Farmer PIN entry, status screen shows:

```
   +----------------+
   |Sys:OPERATIONAL |
   |User: FARMER  ▼B|
   +----------------+
```

**Farmer Settings Menu** (navigate with ▲/▼, select with ENTER):

#### 5.1.1 Day Temperature Setting

```
   +----------------+
   |Day Temp: [22]°C|
   |▲▼:Chg C:OK D:X |
   +----------------+
```

**Edit Mode** (press ENTER to edit):
```
   +----------------+
   |Day Temp: [22]°C|
   |▲=Inc ▼=Dec C:OK|
   +----------------+
```

- Press ▲ to increment (range: 15-30°C)
- Press ▼ to decrement
- Press C (ENTER) or # to confirm
- Press D (BACK) to cancel
- Valid range: 15°C - 30°C
- Changes applied immediately on confirm

#### 5.1.2 Night Temperature Setting

```
   +----------------+
   |Night Temp:[18]°|
   |▲▼:Chg C:OK D:X |
   +----------------+
```

**Edit Mode**:
- Same behavior as Day Temperature
- Valid range: 10°C - 25°C
- Must be ≤ Day Temperature

#### 5.1.3 Day Humidity Setting

```
   +----------------+
   |Day Humidity:70%|
   |▲▼:Chg C:OK D:X |
   +----------------+
```

**Edit Mode**:
- Press ▲ to increment by 5% (range: 40-90%)
- Press ▼ to decrement by 5%
- Press C (ENTER) or # to confirm
- Press D (BACK) to cancel

#### 5.1.4 Night Humidity Setting

```
   +----------------+
   |Night Humid: 70%|
   |▲▼:Chg C:OK D:X |
   +----------------+
```

**Edit Mode**:
- Same behavior as Day Humidity
- Valid range: 40% - 90%

#### 5.1.5 Change Farmer PIN

```
   +----------------+
   |Change PIN:     |
   |New:[      ]  # |
   +----------------+
```

**PIN Change Flow**:
1. Enter 6-digit new PIN
2. Press # or ENTER
3. Re-enter same PIN to confirm
4. Shows "PIN Changed!" on success
5. Returns to menu

#### 5.1.6 Logout

```
   +----------------+
   |Logout?         |
   |C:Yes  D:Cancel |
   +----------------+
```

Press C (ENTER) to logout, D (BACK) to cancel

---

### 5.2 Technician Menu

After successful Technician PIN entry, status screen shows:

```
   +----------------+
   |Sys:OPERATIONAL |
   |User: TECH    ▼B|
   +----------------+
```

**Technician Settings Menu** (navigate with ▲/▼, select with ENTER):

#### 5.2.1 View Farmer Settings (Read-Only)

```
   +----------------+
   |Farmer Settings:|
   |Day:22°C/70% RO |
   +----------------+
```

Navigate through screens showing current farmer settings:
- Day Temperature & Humidity
- Night Temperature & Humidity
- Note: "RO" indicates Read-Only - technician cannot modify

#### 5.2.2 WiFi Access Point Control

```
   +----------------+
   |WiFi AP: [ON]   |
   |▲▼:Tog C:OK D:X |
   +----------------+
```

**Edit Mode** (press ENTER to toggle):
```
   +----------------+
   |WiFi AP: [OFF]  |
   |C:ON D:X        |
   +----------------+
```

- Press ▲ or ▼ to toggle ON/OFF
- Press C (ENTER) to confirm change
- Press D (BACK) to cancel
- Shows "AP Enabled" or "AP Disabled" on confirmation
- AP used for technician access to WEB-GUI during maintenance

#### 5.2.3 Change Technician PIN

```
   +----------------+
   |Change PIN:     |
   |New:[      ]  # |
   +----------------+
```

**PIN Change Flow**:
1. Enter 6-digit new PIN
2. Press # or ENTER
3. Re-enter same PIN to confirm
4. Shows "PIN Changed!" on success
5. Returns to menu

#### 5.2.4 Logout

```
   +----------------+
   |Logout?         |
   |C:Yes  D:Cancel |
   +----------------+
```

Press C (ENTER) to logout, D (BACK) to cancel

---

## 6. Navigation & Behavior

### 6.1 Auto-Rotation Mode

**Trigger**: No user interaction for 30 seconds, or manual activation
**Rotation Sequence**:
1. System Status (5 seconds)
2. Roof Ventilation (5 seconds)
3. Side Ventilation (5 seconds)
4. Day Setpoints (5 seconds)
5. Night Setpoints (5 seconds)
6. WiFi Status (5 seconds)
7. Active Alarms (3 seconds each, if any)
8. Return to System Status

**Pause Auto-Rotation**:
- Press ▲ or ▼ to manually navigate (pauses rotation for 30 seconds)
- Enter settings menu (stops rotation until logout)
- Press \* to return to first screen and resume auto-rotation

### 6.2 Manual Navigation

**In Auto-Rotation Mode**:
- **▲ (UP)**: Previous screen, pauses auto-rotation
- **▼ (DOWN)**: Next screen, pauses auto-rotation
- **\* (CLR)**: Jump to first screen (System Status), resume auto-rotation
- **# (OK)**: Jump to PIN entry screen (for login)

**In Settings Menu**:
- **▲ (UP)**: Previous menu item
- **▼ (DOWN)**: Next menu item
- **C (ENTER)** or **# (OK)**: Select/edit current item
- **D (BACK)**: Go back one level or cancel

**In Edit Mode**:
- **▲ (UP)**: Increment value
- **▼ (DOWN)**: Decrement value
- **C (ENTER)** or **# (OK)**: Confirm changes
- **D (BACK)**: Cancel changes without saving

### 6.3 Backlight Control

**Brightness States**:
- **Full Brightness**: Active when user interacts (any keypress)
- **Dimmed (50%)**: After 30 seconds of inactivity
- **Dim Timeout**: Backlight remains on but dimmed, never turns completely off

---

## 7. Feedback & Error Handling

### 7.1 Visual Feedback

**Successful Actions**:
```
   +----------------+
   |✓ Saved!        |
   |T:22°C H:70%    |
   +----------------+
```
Displays for 1 second, then returns to previous screen

**Error Messages**:
```
   +----------------+
   |✗ Invalid Range |
   |Must be 15-30°C |
   +----------------+
```
Displays for 2 seconds, returns to edit mode

**PIN Entry Feedback**:
```
   +----------------+
   |PIN:[●●●●●●]  # |
   |              ▼B|
   +----------------+
```
Shows filled dots as digits entered

### 7.2 Error Conditions

**Wrong PIN**:
```
   +----------------+
   |✗ Wrong PIN!    |
   |Try again       |
   +----------------+
```
Displays for 2 seconds, clears PIN entry

**PIN Locked (3 failed attempts)**:
```
   +----------------+
   |LOCKED 5 Minutes|
   |Too many tries  |
   +----------------+
```
Keypad disabled for 5 minutes, shows countdown

**Value Out of Range**:
```
   +----------------+
   |✗ Out of Range  |
   |Range: 15-30°C  |
   +----------------+
```

**PIN Mismatch (during change)**:
```
   +----------------+
   |✗ PINs Mismatch |
   |Try again       |
   +----------------+
```

---

## 8. Custom LCD Characters

**Custom Character Definitions** (for enhanced display):

- **↑ (Up Arrow)**: Indicator for "press up to scroll"
- **↓ (Down Arrow)**: Indicator for "press down to scroll"
- **✓ (Checkmark)**: Success confirmation
- **✗ (X mark)**: Error/cancel indicator
- **● (Filled Dot)**: PIN entry masking
- **° (Degree Symbol)**: Temperature display

---

## 9. Screen Priority & Interrupts

### 9.1 Alarm Interrupts

When critical alarm occurs:
- Immediately interrupts auto-rotation
- Shows alarm screen with flashing backlight
- Remains on alarm screen until acknowledged
- Press any key to acknowledge (except during PIN lock)
- Returns to normal rotation after acknowledgment

**Critical Alarms** (interrupt auto-rotation):
- SENSOR FAIL
- POWER FAIL
- WIND ALARM (during high wind)

**Non-Critical Alarms** (shown in rotation only):
- HIGH TEMP
- LOW TEMP
- HIGH HUM
- LOW HUM
- WIND PROT OFF (persistent; shown every rotation cycle for as long as wind protection is disabled — cannot be acknowledged/dismissed; cleared automatically when wind protection is re-enabled)
- HUM CTRL OFF (shown in rotation when humidity control is disabled; informational)

### 9.2 Priority Sequence

1. **PIN Lock Screen** (highest priority - cannot be bypassed)
2. **Critical Alarms** (interrupts normal flow)
3. **Settings Menu** (user interaction)
4. **Manual Navigation** (user scrolling)
5. **Auto-Rotation** (default mode)

---

## 10. Implementation Notes

### 10.1 State Machine

**States**:
- `IDLE_AUTO_ROTATE`: Default auto-rotating through status screens
- `IDLE_MANUAL_NAV`: User manually navigating, auto-rotation paused
- `PIN_ENTRY`: Awaiting PIN input
- `FARMER_MENU`: Farmer logged in, in settings menu
- `FARMER_EDIT`: Farmer editing a parameter
- `TECH_MENU`: Technician logged in, in settings menu
- `TECH_EDIT`: Technician editing a parameter
- `ALARM_DISPLAY`: Showing critical alarm
- `PIN_LOCKED`: Keypad locked after failed attempts

### 10.2 Timeout Behavior

| State | Timeout Duration | Action on Timeout |
|:------|:-----------------|:------------------|
| IDLE_MANUAL_NAV | 30 seconds | Resume auto-rotation |
| PIN_ENTRY | 60 seconds | Clear entry, return to auto-rotation |
| FARMER_MENU | 120 seconds | Auto-logout, return to PIN entry |
| FARMER_EDIT | 60 seconds | Cancel edit, return to menu |
| TECH_MENU | 120 seconds | Auto-logout, return to PIN entry |
| TECH_EDIT | 60 seconds | Cancel edit, return to menu |
| PIN_LOCKED | 5 minutes | Unlock keypad, return to PIN entry |

### 10.3 Data Persistence

**Settings Saved to Non-Volatile Storage**:
- Farmer PIN (hashed)
- Technician PIN (hashed)
- Day temperature setpoint
- Night temperature setpoint
- Day humidity setpoint
- Night humidity setpoint
- WiFi AP enable/disable state

**Display Format Standards**:
- Temperature: Integer values only (no decimals) - "22°C"
- Humidity: Integer percentages - "70%"
- Wind speed: Integer values - "2" (Beaufort scale)
- Ventilation position: Integer percentages - "40%"

---

## 11. Comparison with WEB-GUI

| Feature | WEB-GUI | LCD-GUI |
|:--------|:--------|:--------|
| **Display Size** | Unlimited scrolling | 16×2 characters |
| **Navigation** | Touch/mouse | 4×4 keypad |
| **Auto-Rotation** | No | Yes (5-second intervals) |
| **Farmer Controls** | All climate settings | Day/night T & H only |
| **Technician Controls** | Full system config | AP toggle only |
| **Alarm System** | Full configuration | View only |
| **Logging** | Full event history | Not available |
| **Schedules** | Full profile management | Not available |
| **Network Config** | Full WiFi setup | Not available (use WEB-GUI) |
| **Sensor Details** | Full sensor info | Basic readings only |
| **Feedback** | Always-visible footer | Temporary messages |

**Design Philosophy**:
- **WEB-GUI**: Full-featured interface for complete system management
- **LCD-GUI**: Quick monitoring and essential parameter adjustment on-site

---

## 12. Conclusion

This LCD-GUI design provides:
- ✅ Auto-rotating status display for passive monitoring
- ✅ Manual navigation with up/down buttons
- ✅ PIN-based role authentication (Farmer/Technician)
- ✅ Minimal but essential parameter control
- ✅ Alarm viewing (no logging)
- ✅ Farmer: Temperature & humidity setpoints for day/night
- ✅ Technician: AP management, view-only farmer settings
- ✅ Clear feedback and error handling
- ✅ Optimized for 16×2 LCD and 4×4 keypad constraints

The design complements the WEB-GUI by providing essential local access while directing advanced configuration to the web interface.
