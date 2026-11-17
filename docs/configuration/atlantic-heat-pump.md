# Atlantic heat pump energy
<!-- doxygen-label: cfg_atlantic_heat_pump -->

> [!NOTE]
> Experimental, confirmed on one Atlantic heat pump (node type 22, subtype 1, two zones, two Navilink
> room thermostats, a TaHoma Switch as the main controller). Please report other models.

The `atlantic_heat_pump:` block of the hub reads the two energy counters of an Atlantic heat pump,
heating and domestic hot water, and shows them as kWh sensors that fit the Home Assistant energy
dashboard. It is read-only: it controls nothing, and no setpoint or mode of the heat pump is touched.

The Atlantic heat pump subtypes are not supported as a `climate:` entity (see
[Heating and climate](climate.md)); this block only adds
its counters.

```yaml
home_io_control:
  # ...
  atlantic_heat_pump:
    id: heat_pump
    io_device_id: "123ABC"
    update_interval: 1h
    heating_energy:
      name: "Heat pump heating energy"
    hot_water_energy:
      name: "Heat pump hot water energy"

# Optional: read the counters on demand.
button:
  - platform: template
    name: "Read heat pump energy"
    on_press:
      - component.update: heat_pump
```

- `io_device_id` (Required): the heat pump's 6-hex-character ID, as Scan Paired Devices prints it
  (`heat pump (Atlantic) (0x16/1)`).
- `update_interval` (Optional, default `1h`, minimum `5min`): how often both counters are read. Each
  read is a short radio exchange on a duty-cycled band; the counters step by 1 kWh.
- `heating_energy`, `hot_water_energy` (Optional, at least one): sensors with every
  [sensor option](https://esphome.io/components/sensor/). Both are `device_class: energy`,
  `state_class: total_increasing`, unit kWh.

What to expect:

- **The first read follows about a minute after boot**, then one every `update_interval`. A
  `component.update` action reads both counters at once.
- **Each counter takes about 2 seconds** to read (two opens, one block, close, with a 650 ms pause
  before each request after the first, as the
  [`read_object` probe](../diagnostic-probes.md#reading-a-device-object) does). The pauses are timers, not
  waits: the hub keeps serving commands and receiving frames during a read, and reads queue one behind
  the other. A data open answered empty, which the heat pump often does, is sent once more.
- **Reads give way to other controllers.** Whenever the hub hears another controller use the data
  transport (a TaHoma, including one whose node id the hub shares, or the room thermostats), reads wait
  for 15 s of quiet; a read already open is closed at once.
- **The heat pump is never read because of other traffic.** Its room thermostats write to it about
  every 90 seconds; the hub ignores that traffic for a heat pump instead of answering it with status
  reads.

## How the counters are read

A TaHoma reads these counters once an hour, at the same minute each hour, with the data transport the
[`read_object` probe](../diagnostic-probes.md#reading-a-device-object) uses, with an offset inside
the object. It opens each counter twice: `08 0C 00 10 01 00 23 00 00` (class `0x0C`, object `0x1001`,
offset `0x0023`), which the heat pump answers empty, then the same in object `0x1002`, which it answers
with three bytes, `80` and the counter in kWh, big-endian. Offset `0x0025` is hot water. This block sends
the same pair. No exchange with the heat pump was authenticated in the captures. The hub opens only the
offsets a TaHoma was heard reading (`TRANSFER_OFFSETS_ATLANTIC`); `scan()` reads them all and logs them raw.

A third entry the TaHoma reads with them, offset `0x0021`, answers `81 FF FF` on the heat pump this was
tested on (a different marker and no value), so it is not read.

The values matched the yearly totals the TaHoma app shows for heating and hot water, and the hot-water
counter stepped by 1 kWh between two reads.
