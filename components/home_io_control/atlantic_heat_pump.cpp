#include "atlantic_heat_pump.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome {
namespace home_io_control {

namespace {
const char *const TAG = "home_io_control.atlantic";
/// The first poll waits this long after boot: the hub's radio and its first status reads come first.
constexpr uint32_t ATLANTIC_FIRST_READ_DELAY_MS = 60000;
/// An energy counter is three bytes: a marker byte and a big-endian u16.
constexpr uint16_t ATLANTIC_COUNTER_SIZE = 3;
/// Marker byte that leads a counter answer (and every short answer seen from the heat pump).
constexpr uint8_t ATLANTIC_COUNTER_MARKER = 0x80;
constexpr size_t ATLANTIC_OFFSET_COUNT = sizeof(TRANSFER_OFFSETS_ATLANTIC) / sizeof(TRANSFER_OFFSETS_ATLANTIC[0]);
}  // namespace

void AtlanticHeatPumpEnergy::setup() {
  // Registered so the reader finds its node id and power class. As HEAT_PUMP, it gets no status polls,
  // no reaction to foreign frames, and no cover/climate entity: this block is its only user.
  DeviceConfig config;
  config.type = DeviceType::HEAT_PUMP;
  config.subtype = 1;
  config.entity_class = DeviceCapabilityClass::SENSOR;
  this->parent_->add_device(this->device_id_, config);
}

void AtlanticHeatPumpEnergy::update() {
  // PollingComponent runs update() once right after setup(); the radio is not worth a read that early.
  if (millis() < ATLANTIC_FIRST_READ_DELAY_MS) {
    this->set_timeout("first_read", ATLANTIC_FIRST_READ_DELAY_MS - millis(), [this]() { this->update(); });
    return;
  }
  if (this->heating_energy_ != nullptr)
    this->read_into_(TRANSFER_OFFSET_ATLANTIC_HEATING_ENERGY, this->heating_energy_);
  if (this->hot_water_energy_ != nullptr)
    this->read_into_(TRANSFER_OFFSET_ATLANTIC_HOT_WATER_ENERGY, this->hot_water_energy_);
}

void AtlanticHeatPumpEnergy::read_into_(uint16_t offset, sensor::Sensor *sensor) {
  // One block is plenty for a 3-byte counter. Captured by value: the callback runs long after this returns.
  this->read(offset, TRANSFER_BLOCK_SIZE, [this, offset, sensor](bool complete, const uint8_t *data, uint16_t len) {
    // A failed read publishes nothing and keeps the last value; the next update() tries again.
    if (!complete)
      return;
    // `81 FF FF` is the heat pump's "no value right now", seen now and then on any entry: not a counter.
    if (len != ATLANTIC_COUNTER_SIZE || data[0] != ATLANTIC_COUNTER_MARKER) {
      ESP_LOGW(TAG, "%s: offset 0x%04X: unexpected answer (%u bytes)", this->device_id_.c_str(),
               static_cast<unsigned>(offset), static_cast<unsigned>(len));
      return;
    }
    const auto kwh = static_cast<uint16_t>((data[1] << 8) | data[2]);
    ESP_LOGD(TAG, "%s: offset 0x%04X = %u kWh", this->device_id_.c_str(), static_cast<unsigned>(offset),
             static_cast<unsigned>(kwh));
    sensor->publish_state(kwh);
  });
}

void AtlanticHeatPumpEnergy::read(uint16_t offset, uint16_t max, AtlanticReadDone done) {
  this->parent_->queue_atlantic_read(this->device_id_, offset, max, std::move(done));
}

void AtlanticHeatPumpEnergy::scan() {
  if (this->scanning_) {
    ESP_LOGW(TAG, "%s: scan already running", this->device_id_.c_str());
    return;
  }
  this->scanning_ = true;
  ESP_LOGI(TAG, "%s: scanning %u entries", this->device_id_.c_str(), static_cast<unsigned>(ATLANTIC_OFFSET_COUNT));
  // The reads queue up and run one after the other; the last callback ends the scan. The reader calls back
  // every read once and in order, so this holds even when some reads fail. The bytes are logged raw: the
  // layout of most entries is still unknown, and the log is what people can share to decode them.
  for (size_t i = 0; i < ATLANTIC_OFFSET_COUNT; i++) {
    const uint16_t offset = TRANSFER_OFFSETS_ATLANTIC[i];
    const bool last = i + 1 == ATLANTIC_OFFSET_COUNT;
    this->read(offset, TRANSFER_READ_MAX_BYTES, [this, offset, last](bool complete, const uint8_t *data, uint16_t len) {
      if (len > 0 || complete) {
        ESP_LOGI(TAG, "%s: entry 0x%04X: %u bytes%s", this->device_id_.c_str(), static_cast<unsigned>(offset),
                 static_cast<unsigned>(len), complete ? "" : ", incomplete");
      }
      for (uint16_t at = 0; at < len; at += TRANSFER_LOG_BYTES_PER_LINE) {
        char hex[format_hex_pretty_size(TRANSFER_LOG_BYTES_PER_LINE)];
        format_hex_pretty_to(hex, data + at, std::min<uint16_t>(TRANSFER_LOG_BYTES_PER_LINE, len - at), ' ');
        ESP_LOGI(TAG, "%s:   0x%04X +%03u: %s", this->device_id_.c_str(), static_cast<unsigned>(offset),
                 static_cast<unsigned>(at), hex);
      }
      if (last) {
        this->scanning_ = false;
        ESP_LOGI(TAG, "%s: scan done", this->device_id_.c_str());
      }
    });
  }
}

void AtlanticHeatPumpEnergy::dump_config() {
  ESP_LOGCONFIG(TAG, "Atlantic heat pump energy:");
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Heating energy", this->heating_energy_);
  LOG_SENSOR("  ", "Hot water energy", this->hot_water_energy_);
}

}  // namespace home_io_control
}  // namespace esphome
