#pragma once

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

#include "hub_ports.h"

#include <string>

namespace esphome {
namespace home_io_control {

/// @brief The `atlantic_heat_pump:` block of the hub: polls the heating and hot-water energy counters of an
/// Atlantic heat pump (node type 22, subtype 1) into two kWh sensors.
///
/// Read-only. The heat pump is registered as DeviceType::HEAT_PUMP, which the hub never reads because of the
/// traffic its room thermostats send it every ~90 s; the only frames this block sends are the counter reads
/// queued through HubDevicePort::queue_atlantic_read(), once per `update_interval` (and on
/// `component.update`), plus whatever scan() or read() is asked for.
/// @ingroup hioc_hub
class AtlanticHeatPumpEnergy : public PollingComponent {
 public:
  void set_parent(HubDevicePort *parent) { this->parent_ = parent; }
  void set_device_id(const std::string &device_id) { this->device_id_ = device_id; }
  void set_heating_energy_sensor(sensor::Sensor *sensor) { this->heating_energy_ = sensor; }
  void set_hot_water_energy_sensor(sensor::Sensor *sensor) { this->hot_water_energy_ = sensor; }

  void setup() override;
  void update() override;
  /// Read every entry of TRANSFER_OFFSETS_ATLANTIC, one after the other, and log each raw (INFO, "entry
  /// 0x....: n bytes" then the bytes, TRANSFER_LOG_BYTES_PER_LINE per line). Read-only, to learn their
  /// layout; a scan already running is left alone.
  void scan();
  /// Queue a read of the data-object entry at @p offset (one of TRANSFER_OFFSETS_ATLANTIC); @p done gets the
  /// raw answer, from loop(). Never blocks: see AtlanticReader::queue().
  void read(uint16_t offset, uint16_t max, AtlanticReadDone done);
  void dump_config() override;
  /// After the hub, whose setup() brings the radio up and fills the registry.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// Read the counter at @p offset and publish it when it decodes.
  void read_into_(uint16_t offset, sensor::Sensor *sensor);

  bool scanning_{false};
  HubDevicePort *parent_{nullptr};
  std::string device_id_;
  sensor::Sensor *heating_energy_{nullptr};
  sensor::Sensor *hot_water_energy_{nullptr};
};

}  // namespace home_io_control
}  // namespace esphome
