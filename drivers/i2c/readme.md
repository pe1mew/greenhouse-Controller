# LIB-2 — I2C Bus Driver

Mutex-aware wrapper around the Arduino Wire library for the shared I2C bus.

## Environments

| Environment | Purpose |
|-------------|---------|
| `lolin_s3`  | Upload hardware verification sketch to LOLIN S3 board |
| `native`    | Run Unity unit tests on the host PC |

## Usage

```
# Run unit tests (host)
pio test -e native

# Upload hardware verification sketch
pio run -e lolin_s3 --target upload
pio device monitor --baud 115200 --port <PORT>
```

## Dependencies

- `firmware/config/pin_config.h` — GPIO pin assignments (`PIN_I2C_SDA`, `PIN_I2C_SCL`)
- Arduino Wire library (target build only)

## Pin assignments

| Signal | GPIO |
|--------|------|
| SDA    | 1    |
| SCL    | 2    |

Bus speed: 400 kHz (Fast-mode).
