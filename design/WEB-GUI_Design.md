# Greenhouse Controller - WEB-GUI Design

## 1. Overview

### 1.1 Purpose
The WEB-GUI provides a user-friendly interface for monitoring and controlling the greenhouse system. The interface is designed with a mobile-first approach while maintaining full functionality on larger screens.

### 1.2 Design Principles
- **Mobile-First**: Primary focus on smartphone usability
- **Simple Navigation**: Minimal taps/clicks to reach any function
- **Vertical Scrolling**: All content on a single page arranged vertically for easy thumb navigation
- **Role-Based Access**: Different views for public, farmer, and technician
- **Two-Step Configuration**: Apply (validate) → Commit (execute) workflow
- **Responsive**: Adapts seamlessly to laptop/desktop screens

---

## 2. User Roles & Access Levels

### 2.1 Public (No Login Required)
- **Access**: Read-only
- **Available Views**:
  - Current operation status
  - Real-time sensor data
  - System state information
  - Event logs

### 2.2 Farmer (PIN-based Login)
- **Access**: Configuration of operational parameters
- **Authentication**: 6-digit PIN
- **Available Settings**:
  - Setpoint configuration (temperature, humidity)
  - Operation schedules
  - Ventilation preferences
  - Alarm thresholds
  - Climate presets

### 2.3 Technician (PIN-based Login)
- **Access**: Advanced configuration and system maintenance
- **Authentication**: 6-digit PIN (different from Farmer PIN)
- **Available Settings**:
  - Climate Control (view-only, configured by farmer)
  - All farmer settings (schedules, alarms)
  - Hardware calibration
  - Sensor configuration
  - System parameters
  - Network settings
  - Firmware updates
  - Diagnostic tools
  - Factory reset

---

## 3. Navigation Structure

### 3.1 Header (Always Visible)
```
┌─────────────────────────────────────┐
│ ☰ Menu    Greenhouse Control  👤   │
└─────────────────────────────────────┘
```

**Components**:
- **☰ Menu**: Hamburger menu for navigation
- **Title**: Current page name
- **👤 User Icon**: Login status / User menu

### 3.2 Footer (Always Visible)
```
┌─────────────────────────────────────┐
│  ✅ Input validated successfully    │
└─────────────────────────────────────┘
```

**Purpose**: Provides immediate feedback on user actions and input validation

**Feedback States**:

1. **Success (Green Background #4CAF50)**:
   - "✅ Input validated successfully"
   - "✅ Changes applied - ready to commit"
   - "✅ Configuration committed successfully"
   - "✅ Settings saved"

2. **Error (Light Red Background #FFCDD2)**:
   - "❌ Temperature must be between 15-30°C"
   - "❌ Night setpoint cannot exceed day setpoint"
   - "❌ Invalid humidity range (40-90%)"
   - "❌ Commit failed - check connection"

3. **Warning (Light Amber Background #FFE082)**:
   - "⚠️ Value outside recommended range"
   - "⚠️ Changes pending - apply to validate"

4. **Idle (Light Gray Background #F5F5F5)**:
   - "ℹ️ Ready" (default state when no action)
   - Auto-hides after 5 seconds for success messages
   - Persists until corrected for error messages

**Design Specifications**:
- Height: 44px (touch-friendly)
- Font: Roboto Medium, 14px
- Text color: White for success/error, Dark gray for idle
- Padding: 12px
- Fixed position at bottom of viewport
- Slides up with smooth animation (200ms)
- Error messages include specific correction guidance

### 3.3 Menu Structure

#### Public Menu
```
├── Dashboard (Home)
├── Current Status
├── Logs
└── Login
```

#### Farmer Menu
```
├── Dashboard (Home)
├── Current Status
├── Logs
├── Settings
│   ├── Climate Control
│   ├── Schedules
│   ├── Alarms
│   └── Account
└── Logout
```

#### Technician Menu
```
├── Dashboard (Home)
├── Current Status
├── Logs
├── Settings
│   ├── Climate Control (View Only)
│   ├── Schedules
│   ├── Alarms
│   ├── Hardware Config
│   ├── System Settings
│   ├── Diagnostics
│   └── Account
└── Logout
```

---

## 4. Page Designs

### 4.1 Dashboard (Home) - Public View

**Layout**: Single vertical column, swipe/scroll enabled

```
┌─────────────────────────────────────┐
│ ☰ Menu    Dashboard          👤    │
├─────────────────────────────────────┤
│                                     │
│  Greenhouse Status                  │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│  🟢 OPERATIONAL                     
│  Mode: Auto Climate Control         │
│  Last Update: 2 seconds ago         │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Current Conditions                 │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  🌡️ Temperature (Interior)          │
│  Current: 23°C    Target: 22°C      │
│                                     │
│  💧 Humidity (Interior)             │
│  Current: 65%     Target: 70%       │
│                                     │
│  💨 Wind (Exterior)                 │
│  Speed: 2 Bft   Direction: NW (315°)│
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Active Systems                     │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  🔄 Ventilation: AUTO (40%)         │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Recent Events                      │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  12:34 | Ventilation adjusted       │
│  12:15 | Temperature setpoint met   │
│  10:23 | Wind safety check passed   │
│                                     │
│  [View Full Logs]                   │
├─────────────────────────────────────┤
│  ℹ️ Ready                           │
└─────────────────────────────────────┘
```

### 4.2 Current Status Page

**Purpose**: Detailed real-time monitoring

```
┌─────────────────────────────────────┐
│ ☰ Menu    Current Status      👤   │
├─────────────────────────────────────┤
│                                     │
│  System Mode                        │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│  🟢 Auto Climate Control            │
│  Uptime: 5d 12h 34m                 │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Climate Sensors                    │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Temperature (Interior)             │
│  Current: 23°C                      │
│  Target:  22°C                      │
│  Trend:   ↗️ +1°C/hour              │
│  Range: 15°C - 30°C                 │
│  Sensor: FG6485A (Modbus)           │
│                                     │
│  Humidity (Interior)                │
│  Current: 65%                       │
│  Target:  70%                       │
│  Trend:   ↗️ +2%/hour               │
│  Range: 40% - 90%                   │
│  Sensor: FG6485A (Modbus)           │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Wind Sensor (Exterior)             │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Wind Speed: 2 Bft                  │
│  Wind Direction: NW (315°)          │
│  Sensor: SenseCAP S200 (Modbus)     │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Actuator Status                    │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Roof Ventilation                   │
│  Position: 40% open                 │
│  Mode: AUTO                         │
│                                     │
│  Side Ventilation                   │
│  Position: 25% open                 │
│  Mode: AUTO                         │
├─────────────────────────────────────┤
│  ℹ️ Ready                           │
└─────────────────────────────────────┘
```

### 4.3 Logs Page

**Purpose**: Event history and system logging

```
┌─────────────────────────────────────┐
│ ☰ Menu    Logs                👤   │
├─────────────────────────────────────┤
│                                     │
│  Filter & Search                    │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  [🔍 Search logs...]                │
│                                     │
│  Level: [All ▼] [Info ▼]            │
│  Date:  [Today ▼]                   │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Event Log                          │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  ℹ️ 12:34:56 | System               │
│  Ventilation adjusted to 40%        │
│                                     │
│  ✅ 12:15:23 | Climate              │
│  Temperature setpoint reached       │
│  Target: 22°C, Actual: 22°C         │
│                                     │
│  ⚠️ 11:58:04 | Climate              │
│  High humidity detected             │
│  Value: 85%, Threshold: 80%         │
│  Action: Ventilation increased      │
│                                     │
│  ℹ️ 09:30:15 | Wind                 │
│  Wind speed: 4 Bft (safe)           │
│                                     │
│  ℹ️ 00:00:01 | System               │
│  Daily log rotation complete        │
│                                     │
│  [Load More...]                     │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Log Legend                         │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│  ℹ️ Info  ⚠️ Warning  ❌ Error     │
│  ✅ Success  🔧 Maintenance        │
│                                     │
├─────────────────────────────────────┤
│  ℹ️ Ready                           │
└─────────────────────────────────────┘
```

### 4.4 Login Page

```
┌─────────────────────────────────────┐
│ ☰ Menu    Login               👤   │
├─────────────────────────────────────┤
│                                     │
│                                     │
│         🏡                          │
│    Greenhouse Control               │
│                                     │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Enter PIN                          │
│                                     │
│  [  ●  ●  ●  ●  ●  ●  ]             │
│                                     │
│  (Use device keyboard)              │
│                                     │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  View as guest (read-only)          │
│                                     │
├─────────────────────────────────────┤
│  ℹ️ Ready                           │
└─────────────────────────────────────┘
```

**Footer States**:
- Idle: "ℹ️ Ready"
- Invalid PIN: "❌ Invalid PIN - Please try again" (light red background)
- Locked: "❌ Too many attempts - locked for 5 minutes" (light red background)
- Success: "✅ Login successful" (green background, auto-hides)

**Note**: PIN automatically determines user role (Farmer/Technician) based on the entered code.

---

## 5. Settings Pages (After Login)

### 5.1 Climate Control Settings (Farmer - Editable)

**Purpose**: Configure temperature and humidity setpoints
**Access**: Farmer only (technicians have read-only view)

```
┌─────────────────────────────────────┐
│ ☰ Menu    Climate Control    👤 F  │
├─────────────────────────────────────┤
│                                     │
│  Temperature Control                │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Day Setpoint                       │
│  [22] °C                            │
│  Range: 15 - 30                     │
│                                     │
│  Night Setpoint                     │
│  [18] °C                            │
│  Range: 10 - 25                     │
│                                     │
│  Deadband (Hysteresis)              │
│  [± 1] °C                           │
│  Range: 1 - 3                       │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Humidity Control                   │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Target Humidity                    │
│  [70] %                             │
│  Range: 40 - 90                     │
│                                     │
│  Maximum Humidity                   │
│  [85] %                             │
│  Range: 60 - 95                     │
│                                     │
│  Dehumidify Action                  │
│  [✓] Enable ventilation             │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Validation Status                  │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│  ✅ All values within valid ranges  │
│                                     │
│         [   Apply   ]               │
│                                     │
│  ⚠️ Changes pending                 │
│  Last applied: 2024-04-15 14:23     │
│                                     │
│       [   ✓ Commit   ]              │
├─────────────────────────────────────┤
│  ✅ Input validated successfully    │
└─────────────────────────────────────┘
```

**Footer Behavior**:
- After entering valid input: "✅ Input validated successfully" (green)
- After clicking Apply: "✅ Changes applied - ready to commit" (green)
- After clicking Commit: "✅ Configuration committed successfully" (green, auto-hides)
- Invalid input example: "❌ Night setpoint (25°C) cannot exceed day setpoint (22°C)" (light red)
- Out of range: "❌ Temperature must be between 15-30°C" (light red)

When a technician accesses Climate Control, the same page is displayed but:
- All input fields are disabled (grayed out)
- Apply and Commit buttons are hidden
- A banner at the top states: "ℹ️ View Only - Only farmers can modify climate settings"
- Current values are visible for reference

```
┌─────────────────────────────────────┐
│ ☰ Menu    Climate Control    👤 T  │
├─────────────────────────────────────┤
│  ℹ️ View Only - Farmer access only  │
├─────────────────────────────────────┤
│                                     │
│  Temperature Control                │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Day Setpoint: 22 °C                │
│  Night Setpoint: 18 °C              │
│  Deadband: ± 1 °C                   │
│                                     │
│  Humidity Control                   │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Target Humidity: 70%               │
│  Maximum Humidity: 85%              │
│  Dehumidify: Ventilation enabled    │
│                                     │
│  Last modified: 2024-04-15 14:23    │
│  Modified by: Farmer                │
├─────────────────────────────────────┤
│  ℹ️ View Only - Settings managed    │
│     by farmer                       │
└─────────────────────────────────────┘
```

### 5.2 Schedules (Farmer)

```
┌─────────────────────────────────────┐
│ ☰ Menu    Schedules           👤 F │
├─────────────────────────────────────┤
│                                     │
│  Day/Night Cycle                    │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Day Start Time                     │
│  ● Sunrise (auto)                   │
│  ○ Fixed: [06:00]                   │
│                                     │
│  Night Start Time                   │
│  ● Sunset (auto)                    │
│  ○ Fixed: [20:00]                   │
│                                     │
│  Current: Day (until 18:34)         │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Climate Profiles                   │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Standard Profile                   │
│  Temp: 22°C Day / 18°C Night        │
│  Humidity: 70%                      │
│  [Edit]                             │
│                                     │
│  Weekend Profile                    │
│  Temp: 20°C Day / 16°C Night        │
│  Humidity: 65%                      │
│  [Edit]                             │
│                                     │
│  [+ Create New Profile]             │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Validation Status                  │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│  ✅ Schedule valid                  │
│                                     │
│         [   Apply   ]               │
│                                     │
│       [   ✓ Commit   ]              │
├─────────────────────────────────────┤
│  ✅ Schedule valid - ready to apply │
└─────────────────────────────────────┘
```

### 5.3 Alarm Settings (Farmer)

**⚠️ DESIGN NOTE - PENDING DISCUSSION**

There is ongoing discussion about whether alarm functionality is relevant for this greenhouse control system. The alarm system design shown below is comprehensive but may be over-engineered for the actual use case.

**Items Requiring Discussion & Decision:**

1. **Alarm Necessity**
   - [ ] Is an alarm/notification system actually needed for this greenhouse?
   - [ ] What are the real operational scenarios that would require immediate notification?
   - [ ] Can the system operate safely without alarms if the farmer checks the dashboard regularly?

2. **Notification Methods**
   - [ ] Email notifications: Are they useful if the farmer is not constantly checking email?
   - [ ] SMS notifications: Worth the complexity and cost of integration?
   - [ ] Web GUI alerts only: Sufficient for the use case?
   - [ ] Push notifications via PWA: Better alternative to email/SMS?

3. **Alarm Thresholds**
   - [ ] Which conditions truly require immediate attention vs. logging only?
   - [ ] Temperature alarms: Necessary if ventilation system is automated?
   - [ ] Humidity alarms: Relevant given the system primarily controls via ventilation?
   - [ ] Rapid change detection: Is this actually actionable by the farmer?

4. **System Complexity**
   - [ ] Does alarm system add unnecessary complexity to implementation?
   - [ ] Maintenance overhead: Who monitors alarm configuration validity?
   - [ ] False alarm management: How to prevent alarm fatigue?

5. **Alternative Approaches**
   - [ ] Simple logging with manual dashboard review sufficient?
   - [ ] Event history with filtering (already in Logs page) adequate?
   - [ ] Critical-only alarms (sensor failure, power loss) vs. full alarm suite?

6. **Priority Levels**
   - [ ] Are three priority levels (Info, Warning, Critical) necessary?
   - [ ] Should system only alert on Critical events?

**Decision Required:** Determine scope of alarm functionality before final implementation. Options:
- **Full alarm system** as designed below
- **Minimal critical-only alarms** (sensor/communication/power failure)
- **No alarms** - logging and dashboard monitoring only

---

```
┌─────────────────────────────────────┐
│ ☰ Menu    Alarms              👤 F │
├─────────────────────────────────────┤
│                                     │
│  Notification Settings              │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Notification Methods               │
│  ☑️ Web GUI Alert                   │
│  ☑️ Email                           │
│  ☐ SMS (requires setup)             │
│                                     │
│  Email Address                      │
│  [farmer@example.com]               │
│                                     │
│  Quiet Hours                        │
│  ☑️ Enable                          │
│  From: [22:00] To: [06:00]          │
│  (Critical alarms only)             │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Temperature Alarms                 │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  High Temperature                   │
│  ☑️ Enabled                         │
│  Threshold: [32] °C                 │
│  Priority: ⚠️ Warning               │
│                                     │
│  Low Temperature                    │
│  ☑️ Enabled                         │
│  Threshold: [12] °C                 │
│  Priority: ❌ Critical              │
│                                     │
│  Rapid Temperature Change           │
│  ☑️ Enabled                         │
│  Threshold: [5] °C in 30 min        │
│  Priority: ⚠️ Warning               │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Humidity Alarms                    │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  High Humidity                      │
│  ☑️ Enabled                         │
│  Threshold: [90] %                  │
│  Priority: ⚠️ Warning               │
│                                     │
│  Low Humidity                       │
│  ☑️ Enabled                         │
│  Threshold: [35] %                  │
│  Priority: ℹ️ Info                  │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  System Alarms                      │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Sensor Failure                     │
│  ☑️ Enabled                         │
│  Priority: ❌ Critical              │
│                                     │
│  Communication Loss                 │
│  ☑️ Enabled                         │
│  Timeout: [5] minutes               │
│  Priority: ❌ Critical              │
│                                     │
│  Power Failure                      │
│  ☑️ Enabled                         │
│  Priority: ❌ Critical              │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Active Alarms                      │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│  🟢 No active alarms                │
│                                     │
│  Last 24h: 2 warnings, 0 critical   │
│  [View History]                     │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Validation Status                  │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│  ✅ Configuration valid             │
│                                     │
│         [   Apply   ]               │
│                                     │
│       [   ✓ Commit   ]              │
├─────────────────────────────────────┤
│  ℹ️ Ready                           │
└─────────────────────────────────────┘
```

### 5.4 System Settings (Technician Only)

```
┌─────────────────────────────────────┐
│ ☰ Menu    System Settings     👤 T │
├─────────────────────────────────────┤
│                                     │
│  Network Configuration              │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  WiFi Client Settings               │
│  SSID: [GreenHouse_WiFi]            │
│  Status: 🟢 Connected               │
│  Signal: -45 dBm (Excellent)        │
│  [Change Network]                   │
│                                     │
│  IP Configuration:                  │
│  ● DHCP (Automatic)                 │
│  ○ Static (Manual)                  │
│                                     │
│  Current IP: 192.168.1.150          │
│  Gateway: 192.168.1.1               │
│  DNS: 192.168.1.1                   │
│                                     │
│  ━━━ Static IP Settings ━━━         │
│  (Available when Static selected)   │
│                                     │
│  IP Address                         │
│  [192.168.1.___]                    │
│                                     │
│  Subnet Mask                        │
│  [255.255.255.0 ▼]                  │
│  Common: /24, /16, /8               │
│                                     │
│  Default Gateway                    │
│  [192.168.1.1]                      │
│                                     │
│  DNS Server                         │
│  [192.168.1.1]                      │
│  (Primary)                          │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  WiFi Access Point Settings         │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  AP Status                          │
│  ● Disabled (On demand)             │
│  ○ Enabled                          │
│  ℹ️ AP starts on admin command only │
│                                     │
│  AP SSID (Auto-generated)           │
│  Greenhouse-A3F2                    │
│  ℹ️ Based on device MAC address     │
│                                     │
│  AP Password                        │
│  [●●●●●●●●●●●●●●●●●]                │
│  👁️ Show                            │
│  Minimum 8 characters               │
│                                     │
│  Auto-Shutdown Timeout              │
│  [15] minutes                       │
│  Range: 5 - 120 minutes             │
│  ℹ️ AP disables when no clients     │
│     connected for this duration     │
│                                     │
│  ℹ️ Web server runs on port 80      │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Time & Location                    │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Time Zone                          │
│  [Europe/Amsterdam ▼]               │
│                                     │
│  NTP Server                         │
│  ☑️ Auto-sync time                  │
│  Server: [pool.ntp.org]             │
│  Last Sync: 2024-04-16 12:34        │
│                                     │
│  Location (for sunrise/sunset)      │
│  Latitude:  [52.0907] °N            │
│  Longitude: [5.1214] °E             │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Data Management                    │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Logging Level                      │
│  [Info ▼]                           │
│  Options: Debug, Info, Warning      │
│                                     │
│  Data Retention                     │
│  Detailed logs: [30] days           │
│  Summary data: [365] days           │
│                                     │
│  Storage Usage                      │
│  Used: 245 MB / 2 GB (12%)         │
│  [Clear Old Logs]                   │
│                                     │
│  Backup & Export                    │
│  Last Backup: 2024-04-15 03:00     │
│  [Download Config Backup]           │
│  [Export Historical Data]           │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Firmware & Updates                 │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Current Version                    │
│  Firmware: v2.3.1                   │
│  Web UI: v1.5.0                     │
│  Released: 2024-03-15               │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Security                           │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  │
│                                     │
│  Session Timeout                    │
│  [30] minutes                       │
│                                     │
│  PIN Policy                         │
│  Length: 6 digits                   │
│  ☑️ Require unique PINs             │
│  ☑️ Lock after 3 failed attempts    │
│                                     │
│  PIN Management (Administrative)    │
│  ℹ️ Set user PINs without requiring │
│     current PIN (admin function)    │
│                                     │
│  [Change Farmer PIN]                │
│  [Change Technician PIN]            │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Factory Reset                      │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  │
│                                     │
│  ⚠️ DANGER ZONE                     │
│                                     │
│  Reset Options:                     │
│  [  Reset to Defaults  ]            │
│  (Keeps network & users)            │
│                                     │
│  [  Full Factory Reset  ]           │
│  (Erases all settings)              │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Validation Status                  │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  │
│  ✅ Configuration valid             │
│                                     │
│         [   Apply   ]               │
│                                     │
│       [   ✓ Commit   ]              │
├─────────────────────────────────────┤
│  ℹ️ Ready                           │
└─────────────────────────────────────┘
```

#### 5.4.1 PIN Management (Administrative)

**Purpose**: Technician can administratively set PINs for both Farmer and Technician roles
**Access**: Technician only
**Trigger**: Clicking [Change Farmer PIN] or [Change Technician PIN] in System Settings
**Key Difference**: Administrative PIN change does NOT require the current PIN (unlike user self-service PIN change in Account Settings)

---

**Change Farmer PIN (Administrative)**

```
┌─────────────────────────────────────┐
│ ← Back    Change Farmer PIN   👤 T │
├─────────────────────────────────────┤
│                                     │
│  Set New Farmer PIN                 │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  ⚠️ Administrative Function         │
│  You are setting a new PIN for the  │
│  Farmer role. Current PIN is NOT    │
│  required for this operation.       │
│                                     │
│  New Farmer PIN                     │
│  [  ●  ●  ●  ●  ●  ●  ]             │
│  👁️ Show                            │
│  6 digits required                  │
│                                     │
│  Confirm New PIN                    │
│  [  ●  ●  ●  ●  ●  ●  ]             │
│  👁️ Show                            │
│                                     │
│  ℹ️ Requirements:                   │
│  • Exactly 6 digits (0-9)           │
│  • Must differ from Technician PIN  │
│    (if unique PINs enforced)        │
│                                     │
│         [ Set PIN ]                 │
│        [ Cancel ]                   │
│                                     │
├─────────────────────────────────────┤
│  ℹ️ Ready                           │
└─────────────────────────────────────┘
```

**Change Technician PIN (Administrative)**

```
┌─────────────────────────────────────┐
│ ← Back  Change Technician PIN 👤 T │
├─────────────────────────────────────┤
│                                     │
│  Set New Technician PIN             │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  ⚠️ Administrative Function         │
│  You are setting a new PIN for the  │
│  Technician role. Current PIN is    │
│  NOT required for this operation.   │
│                                     │
│  ⚠️ Warning: After setting a new    │
│  Technician PIN, you will be logged │
│  out and must login with the new    │
│  PIN.                               │
│                                     │
│  New Technician PIN                 │
│  [  ●  ●  ●  ●  ●  ●  ]             │
│  👁️ Show                            │
│  6 digits required                  │
│                                     │
│  Confirm New PIN                    │
│  [  ●  ●  ●  ●  ●  ●  ]             │
│  👁️ Show                            │
│                                     │
│  ℹ️ Requirements:                   │
│  • Exactly 6 digits (0-9)           │
│  • Must differ from Farmer PIN      │
│    (if unique PINs enforced)        │
│                                     │
│         [ Set PIN ]                 │
│        [ Cancel ]                   │
│                                     │
├─────────────────────────────────────┤
│  ℹ️ Ready                           │
└─────────────────────────────────────┘
```

**Administrative PIN Change Workflow**:

1. **Technician clicks** [Change Farmer PIN] or [Change Technician PIN] in System Settings
2. **Form appears** with new PIN entry fields (NO current PIN required)
3. **Technician enters** new PIN and confirmation
4. **Click [Set PIN]**: Sends request to backend
5. **Backend validation**:
   - New PIN is exactly 6 digits
   - New PIN matches confirmation
   - New PIN differs from other role PIN (if unique PINs enforced)
6. **Result**:
   - **Farmer PIN changed**: "✅ Farmer PIN updated successfully" → returns to System Settings
   - **Technician PIN changed**: "✅ Technician PIN updated - logging out..." → auto-logout after 3 seconds (must re-login with new PIN)

**Footer States**:
- Idle: "ℹ️ Ready"
- New PIN invalid: "❌ PIN must be exactly 6 digits" (red)
- PINs don't match: "❌ New PIN and confirmation don't match" (red)
- PIN not unique: "❌ New PIN must differ from [other role] PIN" (red)
- Success (Farmer): "✅ Farmer PIN updated successfully" (green, auto-hides)
- Success (Technician): "✅ Technician PIN updated - logging out..." (green, auto-logout)

**Security & Logging**:
- All administrative PIN changes are logged with timestamp and technician identity
- No current PIN validation required (administrative privilege)
- Changing Technician's own PIN via this method forces immediate logout
- Changing Farmer PIN does not affect current Technician session

**Key Distinctions**:

| Feature | Account Settings (User Self-Service) | System Settings (Administrative) |
|---------|-------------------------------------|----------------------------------|
| **Access** | Farmer or Technician (own PIN only) | Technician only (both PINs) |
| **Current PIN Required** | ✅ Yes | ❌ No |
| **Can Change Farmer PIN** | Farmer only | Technician (admin) |
| **Can Change Technician PIN** | Technician only | Technician (admin) |
| **Force Logout** | ✅ Always | ✅ Only if changing own (Technician) PIN |
| **Use Case** | Regular user PIN maintenance | Administrative reset / recovery |

---

#### 5.4.2 Network Configuration Flow (Change Network)

**Purpose**: Connect to a different WiFi network
**Access**: Technician only
**Trigger**: Clicking [Change Network] button in System Settings

---

**Step 1: Network Scan Page**

```
┌─────────────────────────────────────┐
│ ← Back    WiFi Networks       👤 T │
├─────────────────────────────────────┤
│                                     │
│  Scanning for networks...           │
│  🔄                                 │
│                                     │
├─────────────────────────────────────┤
│  ℹ️ Scanning...                     │
└─────────────────────────────────────┘
```

After scan completes (typically 5-10 seconds):

```
┌─────────────────────────────────────┐
│ ← Back    WiFi Networks       👤 T │
├─────────────────────────────────────┤
│                                     │
│  Available Networks (8 found)       │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  📶 GreenHouse_WiFi  ✓             │
│  🔒 -45 dBm (Excellent) Connected   │
│                                     │
│  📶 HomeNetwork_5G                  │
│  🔒 -52 dBm (Good)                  │
│                                     │
│  📶 Office_Guest                    │
│  🔓 -58 dBm (Good)                  │
│                                     │
│  📶 Neighbor_WiFi                   │
│  🔒 -67 dBm (Fair)                  │
│                                     │
│  📶 CafeWiFi                        │
│  🔓 -72 dBm (Weak)                  │
│                                     │
│  📶 IoT_Network                     │
│  🔒 -78 dBm (Weak)                  │
│                                     │
│  [Scan Again]                       │
│                                     │
├─────────────────────────────────────┤
│  ℹ️ Select a network                │
└─────────────────────────────────────┘
```

**Network List Details**:
- **📶 Icon**: Signal strength indicator
- **🔒 Locked**: Network requires password (WPA/WPA2)
- **🔓 Unlocked**: Open network (no password)
- **✓ Checkmark**: Currently connected network
- **Signal Strength**:
  - -50 dBm or better: Excellent
  - -51 to -60 dBm: Good
  - -61 to -70 dBm: Fair
  - -71 dBm or worse: Weak
- **Sorting**: Networks sorted by signal strength (strongest first)

---

**Step 2: Network Selection (for secured network)**

User taps on a secured network (e.g., "HomeNetwork_5G"):

```
┌─────────────────────────────────────┐
│ ← Back    WiFi Password       👤 T │
├─────────────────────────────────────┤
│                                     │
│  Network: HomeNetwork_5G            │
│  Signal: -52 dBm (Good)             │
│  Security: WPA2                     │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Password                           │
│  [●●●●●●●●●●●●●●●●●]                │
│  👁️ Show                            │
│                                     │
│  (Use device keyboard)              │
│                                     │
│         [ Connect ]                 │
│                                     │
├─────────────────────────────────────┤
│  ℹ️ Enter network password          │
└─────────────────────────────────────┘
```

**Step 2: Network Selection (for open network)**

User taps on an open network (e.g., "Office_Guest"):

```
┌─────────────────────────────────────┐
│ ← Back    Connect WiFi        👤 T │
├─────────────────────────────────────┤
│                                     │
│  Network: Office_Guest              │
│  Signal: -58 dBm (Good)             │
│  Security: None (Open)              │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  ⚠️ Warning: Open Network           │
│  This network is not encrypted.     │
│  Data may be visible to others.     │
│                                     │
│         [ Connect ]                 │
│        [ Cancel ]                   │
│                                     │
├─────────────────────────────────────┤
│  ⚠️ Open network - not encrypted    │
└─────────────────────────────────────┘
```

---

**Step 3: Connection Progress**

After clicking [Connect]:

```
┌─────────────────────────────────────┐
│       Connecting...           👤 T │
├─────────────────────────────────────┤
│                                     │
│  Network: HomeNetwork_5G            │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│         🔄                          │
│    Connecting to WiFi...            │
│                                     │
│  This may take up to 30 seconds     │
│                                     │
│  Do not close this page or power    │
│  off the device.                    │
│                                     │
├─────────────────────────────────────┤
│  ⏳ Connecting...                   │
└─────────────────────────────────────┘
```

**Connection Sequence**:
1. Authenticating... (3-5 seconds)
2. Obtaining IP address... (5-10 seconds)
3. Verifying connection... (2-3 seconds)

---

**Step 4a: Connection Result - Success**

```
┌─────────────────────────────────────┐
│       WiFi Connected          👤 T │
├─────────────────────────────────────┤
│                                     │
│  ✅ Successfully Connected          │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Network: HomeNetwork_5G            │
│  Signal: -52 dBm (Good)             │
│  IP Address: 192.168.1.87           │
│  Gateway: 192.168.1.1               │
│  DNS: 192.168.1.1                   │
│                                     │
│  Connection test: ✅ Internet OK    │
│                                     │
│    [ Return to Settings ]           │
│                                     │
├─────────────────────────────────────┤
│  ✅ Connected successfully          │
└─────────────────────────────────────┘
```

**Auto-redirect**: After 5 seconds, automatically returns to System Settings page showing new network connection.

---

**Step 4b: Connection Result - Failure (Wrong Password)**

```
┌─────────────────────────────────────┐
│       Connection Failed       👤 T  │
├─────────────────────────────────────┤
│                                     │
│  ❌ Connection Failed               │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Network: HomeNetwork_5G            │
│  Reason: Authentication failed      │
│                                     │
│  The password you entered may be    │
│  incorrect. Please check and        │
│  try again.                         │
│                                     │
│         [ Try Again ]               │
│    [ Choose Different Network ]     │
│                                     │
│  ℹ️ Still connected to:             │
│  GreenHouse_WiFi                    │
│                                     │
├─────────────────────────────────────┤
│  ❌ Wrong password - Try again      │
└─────────────────────────────────────┘
```

---

**Step 4c: Connection Result - Failure (Network Not Found)**

```
┌─────────────────────────────────────┐
│       Connection Failed       👤 T │
├─────────────────────────────────────┤
│                                     │
│  ❌ Connection Failed               │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Network: HomeNetwork_5G            │
│  Reason: Network not found          │
│                                     │
│  The network may be out of range    │
│  or temporarily unavailable.        │
│                                     │
│         [ Scan Again ]              │
│    [ Choose Different Network ]     │
│                                     │
│  ℹ️ Still connected to:             │
│  GreenHouse_WiFi                    │
│                                     │
├─────────────────────────────────────┤
│  ❌ Network not found               │
└─────────────────────────────────────┘
```

---

**Step 4d: Connection Result - Failure (Timeout/Other)**

```
┌─────────────────────────────────────┐
│       Connection Failed       👤 T │
├─────────────────────────────────────┤
│                                     │
│  ❌ Connection Failed               │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Network: HomeNetwork_5G            │
│  Reason: Connection timeout         │
│                                     │
│  Could not obtain IP address from   │
│  the router. The network may be     │
│  busy or have DHCP issues.          │
│                                     │
│         [ Try Again ]               │
│    [ Choose Different Network ]     │
│                                     │
│  ℹ️ Still connected to:             │
│  GreenHouse_WiFi                    │
│                                     │
├─────────────────────────────────────┤
│  ❌ Connection timeout              │
└─────────────────────────────────────┘
```

---

**Network Change Workflow Summary**:

1. **Scan Networks**: ESP32 scans for available WiFi networks (5-10 seconds)
2. **Select Network**: User taps desired network from list
3. **Enter Password**: For secured networks, user enters password (open networks skip this)
4. **Connect**: ESP32 attempts connection (up to 30 seconds timeout)
5. **Result**: 
   - **Success**: Show connection details, auto-return to settings after 5 seconds
   - **Failure**: Show error message with retry/cancel options
   - **Fallback**: Original network remains connected if new connection fails

**Safety Features**:
- Previous network stays connected until new connection succeeds
- Connection timeout prevents indefinite waiting
- Clear error messages guide user to resolution
- Option to return to network list or settings at any time

**Footer Behavior Throughout Flow**:
- Scan: "ℹ️ Scanning..." → "ℹ️ Select a network"
- Password entry: "ℹ️ Enter network password" → "❌ Password required" (if empty)
- Connecting: "⏳ Connecting..."
- Success: "✅ Connected successfully" (green, auto-hides)
- Failure: "❌ [Error message]" (red, persists)

---

### 5.5 Account Settings (Farmer & Technician)

**Purpose**: User account management - change own PIN
**Access**: Both Farmer and Technician roles

```
┌─────────────────────────────────────┐
│ ☰ Menu    Account Settings    👤 F │
├─────────────────────────────────────┤
│                                     │
│  User Information                   │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Role: Farmer                       │
│  Session Started: 14:23             │
│  Session Timeout: 30 minutes        │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Change My PIN                      │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Current PIN                        │
│  [  ●  ●  ●  ●  ●  ●  ]             │
│  👁️ Show                            │
│                                     │
│  New PIN                            │
│  [  ●  ●  ●  ●  ●  ●  ]             │
│  👁️ Show                            │
│  6 digits required                  │
│                                     │
│  Confirm New PIN                    │
│  [  ●  ●  ●  ●  ●  ●  ]             │
│  👁️ Show                            │
│                                     │
│  ℹ️ Requirements:                   │
│  • Exactly 6 digits (0-9)           │
│  • Must differ from current PIN     │
│  • Must differ from other role PIN  │
│    (if unique PINs enforced)        │
│                                     │
│         [ Change PIN ]              │
│                                     │
├─────────────────────────────────────┤
│  ℹ️ Ready                           │
└─────────────────────────────────────┘
```

**For Technician** (same page with role indicator showing "Technician"):

```
┌─────────────────────────────────────┐
│ ☰ Menu    Account Settings    👤 T │
├─────────────────────────────────────┤
│                                     │
│  User Information                   │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Role: Technician                   │
│  Session Started: 14:23             │
│  Session Timeout: 30 minutes        │
│                                     │
├─────────────────────────────────────┤
│                                     │
│  Change My PIN                      │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│                                     │
│  Current PIN                        │
│  [  ●  ●  ●  ●  ●  ●  ]             │
│  👁️ Show                            │
│                                     │
│  New PIN                            │
│  [  ●  ●  ●  ●  ●  ●  ]             │
│  👁️ Show                            │
│  6 digits required                  │
│                                     │
│  Confirm New PIN                    │
│  [  ●  ●  ●  ●  ●  ●  ]             │
│  👁️ Show                            │
│                                     │
│  ℹ️ Requirements:                   │
│  • Exactly 6 digits (0-9)           │
│  • Must differ from current PIN     │
│  • Must differ from Farmer PIN      │
│    (if unique PINs enforced)        │
│                                     │
│         [ Change PIN ]              │
│                                     │
├─────────────────────────────────────┤
│  ℹ️ Ready                           │
└─────────────────────────────────────┘
```

**PIN Change Workflow**:

1. **User enters current PIN**: Validated against stored hash
2. **User enters new PIN**: Client-side validation (6 digits)
3. **User confirms new PIN**: Must match new PIN exactly
4. **Click [Change PIN]**: Sends request to backend
5. **Backend validation**:
   - Current PIN correct
   - New PIN meets requirements (6 digits)
   - New PIN differs from current PIN
   - New PIN differs from other role PIN (if unique PINs enforced)
6. **Result**:
   - **Success**: "✅ PIN changed successfully - please login again" → auto-logout after 3 seconds
   - **Failure**: "❌ [Error message]" with specific reason

**Footer States**:
- Idle: "ℹ️ Ready"
- Current PIN wrong: "❌ Current PIN incorrect" (red)
- New PIN invalid: "❌ PIN must be exactly 6 digits" (red)
- PINs don't match: "❌ New PIN and confirmation don't match" (red)
- PIN not unique: "❌ New PIN must differ from Farmer PIN" (red, technician only)
- Success: "✅ PIN changed successfully - logging out..." (green, auto-logout)

**Security Notes**:
- Changing PIN requires re-authentication with current PIN
- Successful PIN change forces immediate logout (user must login with new PIN)
- Failed attempts are logged and count toward lockout threshold
- Session remains active during PIN change process (until success)

**Access Control**:
- **Farmer**: Can change only their own PIN via this page
- **Technician**: Can change only their own PIN via this page
- **Technician** (via System Settings): Can change both Farmer PIN and Technician PIN (administrative function)

---

## 6. UI/UX Features

### 6.1 Mobile Optimization

**Touch-Friendly Elements**:
- Minimum tap target size: 44x44 pixels
- Adequate spacing between interactive elements (minimum 8px)
- Large, readable fonts (minimum 16px body text)
- High contrast for outdoor visibility

**Gestures**:
- Swipe down to refresh current page
- Swipe left/right on graphs for time navigation
- Pinch to zoom on charts (optional)
- Pull-down menu with smooth animation

### 6.2 Responsive Breakpoints

**Mobile** (< 768px):
- Single column layout
- Collapsed menu (hamburger)
- Stacked cards
- Full-width buttons

**Tablet** (768px - 1024px):
- Two-column layout where appropriate
- Expanded navigation sidebar option
- Side-by-side comparisons

**Desktop** (> 1024px):
- Multi-column dashboard
- Persistent sidebar navigation
- Larger data visualization
- Quick-access toolbar

### 6.3 Visual Feedback

**Input Validation**:
- ✅ Green border: Valid input
- ❌ Red border: Invalid input with error message
- ⚠️ Yellow border: Warning (acceptable but not optimal)

**State Indicators**:
- 🟢 Green: Operational / Normal
- 🟡 Yellow: Warning / Attention needed
- 🔴 Red: Error / Critical
- ⚪ Gray: Offline / Disabled

**Footer Feedback** (Always Visible):
- **Success** (Green #4CAF50): Input validated, apply succeeded, commit succeeded
  - Examples: "✅ Input validated successfully", "✅ Configuration committed successfully"
  - Auto-hides after 5 seconds
- **Error** (Light Red #FFCDD2): Input validation failed, operation error
  - Examples: "❌ Temperature must be between 15-30°C", "❌ Night setpoint cannot exceed day setpoint"
  - Includes specific correction guidance
  - Persists until user corrects the input
- **Warning** (Light Amber #FFE082): Non-critical issues
  - Examples: "⚠️ Value outside recommended range"
- **Idle** (Light Gray #F5F5F5): Default state
  - Example: "ℹ️ Ready"

**Loading States**:
- Skeleton screens for initial page load
- Progress indicators for actions
- Smooth transitions (200-300ms)
- Footer shows "⏳ Processing..." during operations

### 6.4 Apply/Commit Workflow

**Two-Step Process**:

1. **Apply** (Client-side validation):
   - Validates input ranges
   - Checks for logical conflicts
   - Shows preview of changes
   - Stores changes temporarily
   - Does NOT send to controller yet
   - Visual indicator: "Changes Pending"

2. **Commit** (Server-side execution):
   - Only enabled after successful Apply
   - Sends validated changes to controller
   - Shows progress indicator
   - Confirms successful update
   - Updates "Last Changed" timestamp
   - Logs the configuration change
   - Footer shows "✅ Configuration committed successfully" (auto-hides after 5 seconds)

**Benefits**:
- Prevents invalid configurations from reaching hardware
- Allows user to review changes before execution
- Provides two-level safety mechanism
- Clear separation between validation and execution
- Immediate visual feedback via persistent footer

---

## 7. Technical Specifications

### 7.1 Technology Stack Recommendations

**Frontend**:
- **Framework**: React.js or Vue.js (lightweight, reactive)
- **UI Library**: Material-UI or Tailwind CSS (responsive components)
- **Charts**: Chart.js or Recharts (data visualization)
- **State Management**: Redux or Vuex (application state)
- **PWA**: Progressive Web App support (offline capability)

**Backend**:
- **Web Server**: ESP32 built-in web server or lightweight Node.js/Python
- **API**: RESTful API with JSON responses
- **WebSocket**: Real-time data updates (for live monitoring)
- **Authentication**: JWT (JSON Web Tokens) for session management

**Communication Protocol**:
- HTTP/HTTPS for configuration
- WebSocket for real-time sensor data
- MQTT (optional) for IoT integration

### 7.2 API Endpoints (Examples)

```
# Authentication
POST /api/login
POST /api/logout
GET  /api/session

# Status & Monitoring
GET  /api/status/current
GET  /api/status/sensors
GET  /api/status/actuators
GET  /api/logs?from=<timestamp>&level=<level>

# Configuration (Authenticated)
GET  /api/config/climate
POST /api/config/climate/apply
POST /api/config/climate/commit

GET  /api/config/schedules
POST /api/config/schedules/apply
POST /api/config/schedules/commit

# Technician Only
GET  /api/config/system
POST /api/system/update
POST /api/system/reset

# Real-time (WebSocket)
WS   /ws/live-data
```

### 7.3 Data Update Intervals

- **Real-time sensors**: 2-5 seconds (WebSocket)
- **Status display**: 10 seconds
- **Logs**: On event occurrence
- **Configuration sync**: On commit
- **Historical data**: On request

### 7.4 Security Considerations

**Authentication**:
- Secure PIN storage (hashed with bcrypt/argon2)
- 6-digit PIN for Farmer and Technician roles
- Different PINs for each role
- Lock after 3 failed attempts (5-minute timeout)
- Session timeout (configurable, default 30 min)
- Role-based access control (RBAC)
- HTTPS/TLS encryption recommended

**Data Protection**:
- Input sanitization
- SQL injection prevention (if using database)
- XSS protection
- CSRF tokens for state-changing operations

**Access Control**:
- Public: Read-only access to status and logs
- Farmer: Full access to operational settings (climate control, schedules, alarms)
- Technician: Read-only access to climate control; full access to system settings, hardware configuration, and diagnostics

---

## 8. Implementation Notes

### 8.1 Progressive Web App (PWA)

**Benefits**:
- Install on smartphone home screen
- Offline capability (cached data)
- Push notifications for alarms
- Fast loading with service workers

**Required Files**:
- `manifest.json`: App metadata, icons
- `service-worker.js`: Caching strategy
- Icon sets: 192x192, 512x512 PNG

### 8.2 Accessibility

- WCAG 2.1 Level AA compliance
- Keyboard navigation support
- Screen reader compatibility
- High contrast mode option
- Adjustable font sizes

### 8.3 Internationalization (Future)

- Multi-language support structure
- Locale-based date/time formatting
- Unit conversion (°C/°F, Beaufort to mph, etc.)
- Translation files (JSON)

### 8.4 Testing Considerations

- Responsive design testing (multiple devices)
- Touch gesture testing
- Network failure handling
- Validation logic verification
- Role-based access testing
- Performance testing (low-end devices)

---

## 9. Design Mockup Summary

### Color Scheme (Recommended)

**Primary Colors**:
- Primary: #4CAF50 (Green - represents plants/nature)
- Secondary: #2196F3 (Blue - represents water/sky)
- Accent: #FF9800 (Orange - for warnings)
- Error: #F44336 (Red - for errors/critical)

**Neutrals**:
- Background: #FAFAFA (Light gray)
- Surface: #FFFFFF (White)
- Text: #212121 (Dark gray)
- Text Secondary: #757575 (Medium gray)

**Status Colors**:
- Success: #4CAF50 (Green)
- Warning: #FFC107 (Amber)
- Error: #F44336 (Red)
- Info: #2196F3 (Blue)

### Typography

- **Headers**: Roboto Bold, 20-24px
- **Body**: Roboto Regular, 16px
- **Labels**: Roboto Medium, 14px
- **Captions**: Roboto Regular, 12px

### Spacing

- Standard padding: 16px
- Section spacing: 24px
- Element spacing: 8px
- Button padding: 12px 24px

---

## 10. Conclusion

This WEB-GUI design provides:
- ✅ Mobile-first approach with vertical scrolling
- ✅ Simple, intuitive navigation
- ✅ Role-based access control (Public, Farmer, Technician)
- ✅ Two-step configuration (Apply → Commit)
- ✅ Always-visible header and footer for consistent navigation and feedback
- ✅ Color-coded footer feedback (green for success, light red for errors with correction guidance)
- ✅ Real-time monitoring and control
- ✅ Responsive design for all device sizes
- ✅ Clear visual feedback and validation
- ✅ Comprehensive logging and alarm system

The design emphasizes usability, safety, and efficiency for greenhouse management across different user roles and devices.