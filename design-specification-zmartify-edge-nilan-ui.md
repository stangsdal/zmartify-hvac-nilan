# Design specification: Zmartify Edge UI for Nilan Comfort 302

Status: design draft  
Date: 2026-08-26  
Product surface: `zmartify-edge` app

## 1. Product intent

The user experiences the Nilan Comfort 302 as a home-comfort product, not as a Modbus gateway. The first question answered is: “Is my home comfortable and is ventilation operating normally?” Technical details remain available for owners/installers, but not in the normal flow.

## 2. Information architecture

```text
/app/sites/:siteId/hvac
/app/sites/:siteId/hvac/nilan
/app/sites/:siteId/hvac/nilan/air-quality
/app/sites/:siteId/hvac/nilan/alarms
/app/sites/:siteId/hvac/nilan/schedule
/app/sites/:siteId/hvac/nilan/settings
/app/sites/:siteId/hvac/nilan/diagnostics
```

Recommended MVP: one generic HVAC detail page with a Nilan capability panel. Alternative 1 is a dedicated Nilan page for a more tailored experience; alternative 2 is a raw device dashboard for diagnostics only. The default user route should be the generic comfort view.

## 3. Main comfort screen

Above the fold:

- Current room temperature, prominent: `21.5°`.
- Status phrase: `Comfortable`, `Ventilating`, `Heating`, `Defrosting`, `Attention` or `Offline`.
- Current ventilation level and four-step control.
- Desired room temperature with +/- or slider.
- Compact line: `Auto`, `Level 2`, `48% humidity`, alarm indicator when relevant.

```text
  Nilan Comfort 302                         [status]

                 21.5°
              Comfortable

        [ - ]  22.0°  [ + ]

      Ventilation   1   2   3   4
                    ●   ●   ○   ○

      Auto     Humidity 48%     [Details]
```

Primary actions are setpoint, ventilation level, approved run state and supported operation mode. Show a pending state immediately after a change; show confirmed/failed from the asynchronous device outcome. Never show register numbers, MQTT topics or function codes on this screen.

## 4. Detail cards

### Airflow and temperatures

Show outdoor (T8), supply (T7), extract/room (T3 or selected source), outlet (T4), supply/extract fan levels, bypass and defrost. Unavailable values are `Not available`, never `0°`.

### Air quality

Show humidity when available. Show CO2 only when installed and fresh. If absent, explain that the Nilan unit does not currently report a CO2 sensor.

### Filter

Show `Filter change in 72 days`. When expired or alarm 19 is active, use warning styling. Offer filter reset only if firmware exposes a safe documented command; otherwise link to alarm reset with an explanation.

### Cooker hood / asymmetric airflow

Reserve a dedicated `Boost ventilation` action for a cooker-hood profile. The user can choose a duration and sees a clear summary such as `More air in than out - active for 30 minutes`. Show requested inlet and exhaust levels separately when the device reports asymmetric control. The app must also show when the physical CTS602 controller, humidity logic or defrost has overridden the request.

If the installed controller cannot confirm independent inlet/exhaust control, show the feature as unavailable rather than implying that a normal global fan-level change creates positive pressure.

## 5. Alarms and warnings

An active alarm is visible from the main screen without hiding read-only status.

States:

- Critical: `Ventilation stopped - check system`.
- Warning: `The system needs attention`.
- Informational: `Filter service due`.

Alarm detail shows code/title, severity, operational impact, first/latest observation and a short action recommendation. `Clear alarm` requires operate permission and confirmation. After reset, show `Reset requested`, then `Alarm remains active` or `Alarm cleared`; never imply that clearing the notification repaired the fault.

## 6. Schedule and temporary selections

MVP should display active week-program state, active temporary overrides and remaining time, and optionally provide `Boost ventilation` with a duration when the documented user-function registers are implemented. Defer full week-program editing until register mapping and conflict rules are validated.

Alternative: full schedule editing can be added later, but must define conflicts with the physical CTS602 panel. Always show the active source: `Normal`, `Schedule`, `Boost`, `High humidity`, `Defrost` or `Manual`. Distinguish requested ventilation level from actual level because CTS602 may override the request.

## 7. Settings and diagnostics

User settings: display name, units, notification preferences and comfort defaults.

Owner/installer settings: Modbus address only behind authorization and a warning that changing it may disconnect the controller; sensor availability/source; firmware and OTA status; device reboot.

Diagnostics: controller online, last successful poll, poll age, read/write/timeout/CRC counters, firmware, device ID, product model, raw alarm IDs and MQTT status. Raw registers may be expandable for service use but never editable through a generic field.

## 8. Permissions

Use existing site roles:

| Role | Read state | Operate | Configure/diagnostics |
|---|---:|---:|---:|
| Viewer | Yes | No | No |
| User | Yes | Yes | No |
| Owner | Yes | Yes | Yes |

The UI hides unauthorized controls, while API authorization remains authoritative. Viewers still see alarms and offline status. Owner-only routes must not become accessible by manually changing a URL.

## 9. Realtime and command states

Subscribe through the existing authenticated realtime mechanism. Every control follows:

```text
idle -> sending -> accepted -> confirmed
                         \-> failed / timeout
```

Disable only the submitted control. Keep the last confirmed value visible while pending. Show local validation errors beside the control and device/offline errors for timeout or controller-unavailable. Reconcile with the next reported state even if an outcome arrives late. Do not claim a setpoint is active before confirmation.

The mobile API should add a Nilan-oriented device detail shape or capability extension rather than pretending the unit has multiple thermostatic zones. A single virtual `Ventilation` zone is acceptable only as a compatibility projection at the API boundary.

## 10. Visual and accessibility direction

Follow the existing Edge/CarbEurope language: primary `#301E96`, interactive accent `#67FBFF`, secondary `#7D85FF`, background `#F1F2F2`, and Poppins where already supported. Use color plus text/icon, never color alone. Critical alarms require icon, title and action. Touch targets are at least 48 px. Temperature controls need keyboard and screen-reader labels such as “Increase desired temperature to 22 degrees”.

## 11. Data and API requirements

The app needs one stable read model with explicit freshness:

```ts
type NilanHvacState = {
  device_id: string;
  online: boolean;
  controller_online: boolean;
  freshness_age_ms: number | null;
  run: boolean | null;
  operation_mode: 'off' | 'heat' | 'cool' | 'auto' | null;
  control_state: string | null;
  ventilation_level: number | null;
  actual_inlet_level: number | null;
  actual_exhaust_level: number | null;
  room_temperature_c: number | null;
  target_temperature_c: number | null;
  humidity_pct: number | null;
  co2_ppm: number | null;
  bypass_open: boolean | null;
  defrost_active: boolean | null;
  filter_days_remaining: number | null;
  airflow?: {
    requested_inlet_level: number | null;
    requested_exhaust_level: number | null;
    actual_inlet_level: number | null;
    actual_exhaust_level: number | null;
    active_profile: string | null;
    profile_remaining_s: number | null;
    control_source: string | null;
    asymmetric_control: 'available' | 'confirmed' | 'emulated' | 'unsupported' | 'pending' | 'stale';
  };
  alarms: Array<{ code: number; severity: string; title: string; active: boolean }>;
};
```

Writes should return `command_id`, `pending`, `command_state` and the updated projection. Persist outcomes and freshness so reload does not lose an in-flight or failed operation.

For Home Automation integrations, keep the semantic command stable and transport-independent:

```json
{
  "command_type": "hvac.set_airflow_profile",
  "parameters": {
    "profile": "cooker_hood",
    "inlet_level": 4,
    "exhaust_level": 2,
    "duration_s": 1800,
    "restore_previous": true
  }
}
```

The API response and realtime outcome must distinguish `pending`, `confirmed`, `emulated`, `unsupported`, `failed` and `timeout`. A home-automation system can therefore integrate the feature now and react correctly on installations where CTS602 later proves unable to provide direct asymmetric control.

## 12. Delivery phases

1. Observe: device card, online/freshness, temperatures, fan levels, bypass/defrost, filter and alarms; no app writes.
2. Operate: setpoint, ventilation, run state and supported mode with realtime outcomes and role enforcement.
3. Comfort automation: boost, temporary overrides and validated schedule read/edit.
4. Service: owner/installer diagnostics and validated configuration view; no factory settings.

## 13. Acceptance criteria

- First-time user sees comfort state and changes ventilation within two taps.
- Pending is distinct from confirmed.
- Offline, stale and controller-alarm states are distinguishable.
- Optional sensors never appear as false zeros.
- Viewer is genuinely read-only.
- Alarm reset is explicit and does not promise fault repair.
- Design supports one Nilan unit and future HVAC products.

## 14. Sources

- `S24_Comfort_GB HMI interface.pdf`: HMI, primary screen, settings separation and alarm interaction.
- `2016-02-16_CTS602_Modbus_protokol.pdf`: state, sensor, alarm and command semantics.
- `stangsdal/zmartify-edge/docs/architecture/navigation-v2.md`: site-oriented routes.
- `stangsdal/zmartify-edge/docs/architecture/authorization-v2.md`: role permissions.
- `stangsdal/zmartify-edge/contracts/mqtt-v2/*.schema.json`: command, state and outcome shapes.
- `stangsdal/zmartify-hvac-ahc9000/docs/mobile-app-design-guide.md`: existing visual/comfort-first direction.
