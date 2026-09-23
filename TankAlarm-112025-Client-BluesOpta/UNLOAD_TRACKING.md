# Tank Unload Tracking Feature

## Overview

The unload tracking feature monitors fill-and-empty tanks (like fuel delivery tanks, milk tanks, etc.) and logs when they are unloaded. This is different from tanks that fluctuate through in/out ports - it's specifically for tanks that:
- Fill up gradually over time
- Are then emptied (unloaded) in a single event
- Repeat this cycle

## How It Works

### Detection Algorithm

1. **Peak Tracking**: Once a tank reaches a minimum fill level (default 12 inches), the system starts tracking the peak (highest) level.

2. **Unload Detection**: When the level drops by a significant percentage (default 50%) from the peak, an unload event is triggered.

3. **Empty Height Handling**: If the level drops to or below the sensor mount height, a configurable default empty height is used instead of the actual reading (which may be unreliable at very low levels).

4. **Debouncing**: Requires multiple consecutive low readings (default 3) to confirm an unload, preventing false triggers from sensor noise.

5. **State-Based Rate Limiting**: After an unload is detected, tracking stops until the tank refills above the minimum peak height (default 12 inches). This prevents duplicate logging while the tank remains empty.

## Configuration

### Per-Tank Settings

Add these settings to each tank's configuration in the client:

```json
{
  "monitors": [
    {
      "name": "Fuel Tank",
      "trackUnloads": true,           // Enable unload tracking for this tank
      "unloadEmptyHeight": 2.0,       // Default empty reading when at/below sensor (inches)
      "unloadDropThreshold": 0.0,     // Absolute drop threshold (0 = use percentage)
      "unloadDropPercent": 50.0,      // Drop percentage to trigger (default 50%)
      "unloadAlarmSms": true,         // Send SMS notification on unload
      "unloadAlarmEmail": true        // Include in email summary
    }
  ]
}
```

### Configuration Options

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `trackUnloads` | bool | false | Enable/disable unload tracking for this tank |
| `unloadEmptyHeight` | float | 2.0 | Height to report when tank is at/below sensor |
| `unloadDropThreshold` | float | 0.0 | Absolute drop in inches to trigger (0 = use percentage) |
| `unloadDropPercent` | float | 50.0 | Percentage drop from peak to trigger (10-95%) |
| `unloadAlarmSms` | bool | false | Send SMS notification when tank is unloaded |
| `unloadAlarmEmail` | bool | true | Include unload events in daily email summary |

### Tuning Tips

- **For small tanks (< 48")**: Consider lowering `unloadDropPercent` to 40-50%
- **For large tanks (> 100")**: Consider using `unloadDropThreshold` with an absolute value
- **For tanks that always empty completely**: Set `unloadEmptyHeight` to the actual empty reading
- **For tanks with unreliable low-level readings**: Set `unloadEmptyHeight` to a conservative value (e.g., 2-5 inches)

## Notifications

### SMS Notification

When `unloadAlarmSms` is enabled and SMS is configured on the server:

```
Site Name #1 unloaded: 85.5 in delivered (peak 90.0, now 4.5)
```

### Email Summary

When `unloadAlarmEmail` is enabled, unload events are included in the daily email summary.

## API Endpoints

### GET /api/unloads

Returns the unload event log (up to 50 most recent events):

```json
{
  "count": 5,
  "unloads": [
    {
      "t": 1735142400,           // Event timestamp (epoch)
      "pt": 1735056000,          // Peak timestamp
      "s": "Main Site",          // Site name
      "c": "dev:xxx",            // Client UID
      "n": "Fuel Tank",          // Tank label
      "k": 1,                    // Tank number
      "pk": 90.0,                // Peak height (inches)
      "em": 4.5,                 // Empty height (inches)
      "dl": 85.5,                // Delivered amount (inches)
      "pma": 19.2,               // Peak sensor mA (if available)
      "ema": 4.3,                // Empty sensor mA (if available)
      "sms": true                // SMS notification sent
    }
  ]
}
```

## Notecard Message Format

The client sends unload events via the `unload.qo` notefile (delivered as `unload.qi` on the server via the ClientToServerRelay route):

```json
{
  "c": "dev:xxx",          // Client UID
  "s": "Main Site",        // Site name
  "n": "Fuel Tank",        // Tank label
  "k": 1,                  // Tank number
  "type": "unload",        // Event type
  "pk": 90.0,              // Peak height
  "em": 4.5,               // Empty height
  "pt": 1735056000,        // Peak timestamp (epoch, whole minute)
  "t": 1735142400,         // Event timestamp (epoch, whole minute)
  "pma": 19.2,             // Peak sensor mA
  "ema": 4.3,              // Empty sensor mA
  "sms": true,             // Request SMS notification
  "email": true,           // Include in email
  "mu": "inches"           // Measurement unit
}
```

## Best Practices

1. **Enable only for appropriate tanks**: Don't enable `trackUnloads` for tanks that fluctuate gradually through in/out ports.

2. **Set appropriate thresholds**: If your tank normally fills to ~80% before unloading, set `unloadDropPercent` to 60-70% to avoid false triggers.

3. **Use absolute thresholds for consistency**: If tank capacity varies, use `unloadDropThreshold` with an absolute value (e.g., 48 inches).

4. **Consider notification fatigue**: Enable `unloadAlarmSms` only for critical tanks to avoid alert fatigue.

5. **Monitor the unload log**: Check `/api/unloads` periodically to verify the system is detecting unloads correctly.

## Troubleshooting

### Unloads not being detected

- Check that `trackUnloads` is enabled for the tank
- Verify the tank reaches the minimum peak height (12 inches by default)
- Check if the drop percentage or threshold is set too high
- Ensure the sensor is reading correctly at low levels

### False unload triggers

- Increase `unloadDropPercent` or `unloadDropThreshold`
- Check for sensor noise or intermittent readings
- The debounce count (3 readings) should filter out most noise

### SMS not being sent

- Verify `unloadAlarmSms` is enabled for the tank
- Check server SMS configuration (`smsPrimary`, `smsSecondary`)
- Check server logs for rate limiting messages
