// AtlanticHeatPumpEnergy: the `atlantic_heat_pump:` block, driven against a HubDevicePort double that
// keeps each queued read so the test answers it.

#include "atlantic_heat_pump.h"

#include "hub_port_doubles.h"

#include "esphome/core/hal.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace esphome::home_io_control;

namespace {

constexpr const char *PUMP = "ABC123";

class AtlanticHeatPumpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    this->block.set_parent(&this->hub);
    this->block.set_device_id(PUMP);
    this->block.set_heating_energy_sensor(&this->heating);
    this->block.set_hot_water_energy_sensor(&this->hot_water);
    this->block.setup();
  }

  /// Answer the @p index-th queued read with @p bytes.
  void answer(size_t index, bool complete, const std::vector<uint8_t> &bytes) {
    this->hub.atlantic_reads_.at(index).done(complete, bytes.data(), static_cast<uint16_t>(bytes.size()));
  }

  esphome::test_clock::ManualClock clock{120000};  // past the first-read delay after boot
  test::DevicePortDouble hub;
  esphome::sensor::Sensor heating;
  esphome::sensor::Sensor hot_water;
  AtlanticHeatPumpEnergy block;
};

TEST_F(AtlanticHeatPumpTest, RegistersTheHeatPumpAsAHeatPump) {
  const IoDevice *dev = this->hub.get_device(PUMP);
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::HEAT_PUMP);
  EXPECT_EQ(dev->subtype, 1);
}

TEST_F(AtlanticHeatPumpTest, PublishesBothCountersInKwh) {
  this->block.update();
  ASSERT_EQ(this->hub.atlantic_reads_.size(), 2u);
  EXPECT_EQ(this->hub.atlantic_reads_[0].offset, TRANSFER_OFFSET_ATLANTIC_HEATING_ENERGY);
  EXPECT_EQ(this->hub.atlantic_reads_[1].offset, TRANSFER_OFFSET_ATLANTIC_HOT_WATER_ENERGY);
  EXPECT_EQ(this->hub.atlantic_reads_[0].device_id, PUMP);

  this->answer(0, true, {0x80, 0x2C, 0xBD});
  this->answer(1, true, {0x80, 0x24, 0xAC});
  EXPECT_FLOAT_EQ(this->heating.state, 11453);
  EXPECT_FLOAT_EQ(this->hot_water.state, 9388);
}

TEST_F(AtlanticHeatPumpTest, PublishesNothingForAnAnswerThatIsNotACounter) {
  this->block.update();
  this->answer(0, true, {0x81, 0xFF, 0xFF});  // the marker of an entry with no value
  this->answer(1, true, {0x80, 0x24});        // too short
  EXPECT_TRUE(std::isnan(this->heating.state));
  EXPECT_TRUE(std::isnan(this->hot_water.state));

  this->answer(0, false, {0x80, 0x2C, 0xBD});  // an incomplete read is never published
  EXPECT_TRUE(std::isnan(this->heating.state));
}

TEST_F(AtlanticHeatPumpTest, WaitsAfterBootBeforeTheFirstRead) {
  esphome::test_clock::set_ms(1000);
  this->block.update();
  EXPECT_TRUE(this->hub.atlantic_reads_.empty());
}

TEST_F(AtlanticHeatPumpTest, ScanQueuesEveryListedOffsetOnceAtATime) {
  this->block.scan();
  const size_t count = sizeof(TRANSFER_OFFSETS_ATLANTIC) / sizeof(TRANSFER_OFFSETS_ATLANTIC[0]);
  ASSERT_EQ(this->hub.atlantic_reads_.size(), count);
  EXPECT_EQ(this->hub.atlantic_reads_.front().offset, TRANSFER_OFFSETS_ATLANTIC[0]);
  EXPECT_EQ(this->hub.atlantic_reads_.back().offset, TRANSFER_OFFSETS_ATLANTIC[count - 1]);

  this->block.scan();  // already running: ignored
  EXPECT_EQ(this->hub.atlantic_reads_.size(), count);

  this->answer(count - 1, true, {0x80, 0x00, 0x01});  // the last read ends the scan
  this->block.scan();
  EXPECT_EQ(this->hub.atlantic_reads_.size(), 2 * count);
}

}  // namespace
