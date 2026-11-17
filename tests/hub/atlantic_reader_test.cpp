// AtlanticReader: the queued, non-blocking read of an Atlantic heat pump entry (atlantic_reader.h),
// driven through a fake hub: timers run in order, the exchange answers from a script.

#include "atlantic_reader.h"

#include "proto_commands.h"
#include "proto_frame.h"

#include "esphome/core/hal.h"

#include <gtest/gtest.h>

#include <cstring>
#include <deque>
#include <optional>
#include <vector>

using namespace esphome::home_io_control;

namespace {

const uint8_t HUB_ID[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
const uint8_t PUMP_ID[NODE_ID_SIZE] = {0xAB, 0xC1, 0x23};
const uint8_t THERMOSTAT_ID[NODE_ID_SIZE] = {0x12, 0x34, 0x56};
constexpr const char *PUMP = "ABC123";
constexpr uint16_t HOT_WATER = TRANSFER_OFFSET_ATLANTIC_HOT_WATER_ENERGY;

/// A frame from @p src to @p dst with the given command and payload.
IoFrame make_frame(const uint8_t src[3], const uint8_t dst[3], uint8_t cmd, const std::vector<uint8_t> &payload) {
  IoFrame frame{};
  init_frame(frame, true, false, true, false);
  set_dst(frame, dst);
  set_src(frame, src);
  EXPECT_TRUE(set_cmd(frame, cmd, payload.data(), static_cast<uint8_t>(payload.size())));
  return frame;
}

IoFrame open_reply(uint8_t handle, uint8_t status, uint16_t length) {
  return make_frame(PUMP_ID, HUB_ID, CMD_TRANSFER_OPEN_RESP,
                    {handle, status, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length & 0xFF)});
}

IoFrame block_reply(uint8_t handle, uint16_t chunk, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> payload = {handle, static_cast<uint8_t>(chunk >> 8), static_cast<uint8_t>(chunk & 0xFF)};
  payload.insert(payload.end(), data.begin(), data.end());
  return make_frame(PUMP_ID, HUB_ID, CMD_UNKNOWN4A_RESP, payload);
}

/// The heat pump's usual answer to the prepare open.
IoFrame empty_open() { return open_reply(0x00, TRANSFER_STATUS_ACCEPTED, 0); }

/// The write open (0x48) the room thermostats send; no builder exists for it, on purpose.
constexpr uint8_t TRANSFER_WRITE_OPEN = 0x48;

/// The hub as AtlanticReader sees it: a timer list, a radio that can be held, one heat pump and a
/// scripted exchange (each request takes the next reply; std::nullopt is a request left unanswered).
class AtlanticReaderTest : public ::testing::Test {
 protected:
  AtlanticReaderTest() : reader(HUB_ID, this->make_hub()) { std::memcpy(this->pump.node_id, PUMP_ID, NODE_ID_SIZE); }

  AtlanticReaderHub make_hub() {
    AtlanticReaderHub hub;
    hub.set_timeout = [this](const char *, uint32_t delay_ms, std::function<void()> callback) {
      this->delays.push_back(delay_ms);
      this->timers.push_back(std::move(callback));
    };
    hub.radio_held = [this]() { return this->held; };
    hub.find_device = [this](const std::string &device_id) -> const IoDevice * {
      return device_id == PUMP ? &this->pump : nullptr;
    };
    hub.exchange = [this](const IoFrame &request, IoFrame &reply) {
      this->sent.push_back(request);
      if (this->replies.empty() || !this->replies.front().has_value()) {
        if (!this->replies.empty())
          this->replies.pop_front();
        return false;
      }
      reply = *this->replies.front();
      this->replies.pop_front();
      return true;
    };
    return hub;
  }

  /// Run the pending timers one after the other (a step schedules at most the next one).
  void run(size_t max_steps = 100) {
    for (size_t i = 0; i < max_steps && !this->timers.empty(); i++) {
      auto timer = std::move(this->timers.front());
      this->timers.pop_front();
      timer();
    }
  }

  void queue(uint16_t offset = HOT_WATER, uint16_t max = TRANSFER_BLOCK_SIZE) {
    this->reader.queue(PUMP, offset, max, [this](bool complete, const uint8_t *data, uint16_t len) {
      this->results.push_back({complete, std::vector<uint8_t>(data, data + len)});
    });
  }

  /// The command and payload of the @p index-th request sent.
  std::vector<uint8_t> sent_bytes(size_t index) const {
    const IoFrame &frame = this->sent.at(index);
    std::vector<uint8_t> bytes = {frame.cmd};
    bytes.insert(bytes.end(), frame.data, frame.data + frame.data_len);
    return bytes;
  }

  struct Result {
    bool complete;
    std::vector<uint8_t> data;
  };

  std::deque<std::function<void()>> timers;
  std::vector<uint32_t> delays;
  bool held{false};
  IoDevice pump{};
  std::vector<IoFrame> sent;
  std::deque<std::optional<IoFrame>> replies;
  std::vector<Result> results;
  AtlanticReader reader;
};

TEST_F(AtlanticReaderTest, ReadsAnEntryTheWayATahomaDoes) {
  // Captured from a TaHoma reading the hot-water counter: prepare open (empty), data open, one block, close.
  this->replies = {empty_open(), open_reply(0x41, TRANSFER_STATUS_ADAPTED, 3), block_reply(0x41, 1, {0x80, 0x24, 0xAC}),
                   block_reply(0x41, 0, {})};
  this->queue();
  this->run();

  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_TRUE(this->results[0].complete);
  EXPECT_EQ(this->results[0].data, (std::vector<uint8_t>{0x80, 0x24, 0xAC}));
  ASSERT_EQ(this->sent.size(), 4u);
  EXPECT_EQ(this->sent_bytes(0), (std::vector<uint8_t>{0x46, 0x08, 0x0C, 0x00, 0x10, 0x01, 0x00, 0x25, 0x00, 0x00}));
  EXPECT_EQ(this->sent_bytes(1), (std::vector<uint8_t>{0x46, 0x08, 0x0C, 0x00, 0x10, 0x02, 0x00, 0x25, 0x00, 0x00}));
  EXPECT_EQ(this->sent_bytes(2), (std::vector<uint8_t>{0x4A, 0x41, 0x00, 0x01}));
  EXPECT_EQ(this->sent_bytes(3), (std::vector<uint8_t>{0x4A, 0x41, 0x00, 0x00}));
  // The first step runs on the next loop pass; every later request waits the gap the heat pump needs.
  ASSERT_EQ(this->delays.size(), 4u);
  EXPECT_EQ(this->delays[0], 0u);
  for (size_t i = 1; i < this->delays.size(); i++)
    EXPECT_EQ(this->delays[i], TRANSFER_BLOCK_GAP_MS);
}

TEST_F(AtlanticReaderTest, ReadsAnEntryOfSeveralBlocksDownToChunkOne) {
  std::vector<uint8_t> first(TRANSFER_BLOCK_SIZE, 0x11);
  this->replies = {empty_open(), open_reply(0x42, TRANSFER_STATUS_ACCEPTED, TRANSFER_BLOCK_SIZE + 2),
                   block_reply(0x42, 2, first), block_reply(0x42, 1, {0x22, 0x33}), block_reply(0x42, 0, {})};
  this->queue(HOT_WATER, 2 * TRANSFER_BLOCK_SIZE);
  this->run();

  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_TRUE(this->results[0].complete);
  ASSERT_EQ(this->results[0].data.size(), TRANSFER_BLOCK_SIZE + 2u);
  EXPECT_EQ(this->results[0].data.back(), 0x33);
  EXPECT_EQ(this->sent_bytes(2), (std::vector<uint8_t>{0x4A, 0x42, 0x00, 0x02}));
  EXPECT_EQ(this->sent_bytes(3), (std::vector<uint8_t>{0x4A, 0x42, 0x00, 0x01}));
}

TEST_F(AtlanticReaderTest, SendsTheOpenPairOnceMoreWhenTheDataOpenIsEmpty) {
  this->replies = {empty_open(),
                   empty_open(),
                   empty_open(),
                   open_reply(0x43, TRANSFER_STATUS_ADAPTED, 3),
                   block_reply(0x43, 1, {0x80, 0x2C, 0xBD}),
                   block_reply(0x43, 0, {})};
  this->queue();
  this->run();

  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_TRUE(this->results[0].complete);
  EXPECT_EQ(this->sent.size(), 6u);
}

TEST_F(AtlanticReaderTest, GivesUpAfterTheSecondEmptyDataOpenWithoutAClose) {
  this->replies = {empty_open(), empty_open(), empty_open(), empty_open()};
  this->queue();
  this->run();

  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_FALSE(this->results[0].complete);
  EXPECT_TRUE(this->results[0].data.empty());
  EXPECT_EQ(this->sent.size(), 4u) << "nothing was opened, so nothing is closed";
}

TEST_F(AtlanticReaderTest, GivesUpWhenTheHeatPumpNeverAnswers) {
  this->replies = {std::nullopt, std::nullopt, std::nullopt, std::nullopt};
  this->queue();
  this->run();

  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_FALSE(this->results[0].complete);
  EXPECT_EQ(this->sent.size(), 4u);
}

TEST_F(AtlanticReaderTest, ClosesAnEntryLongerThanAcceptedUnread) {
  this->replies = {empty_open(), open_reply(0x44, TRANSFER_STATUS_ACCEPTED, 40), block_reply(0x44, 0, {})};
  this->queue(HOT_WATER, TRANSFER_BLOCK_SIZE);
  this->run();

  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_FALSE(this->results[0].complete);
  ASSERT_EQ(this->sent.size(), 3u);
  EXPECT_EQ(this->sent_bytes(2), (std::vector<uint8_t>{0x4A, 0x44, 0x00, 0x00}));
}

TEST_F(AtlanticReaderTest, ClosesAfterABlockThatDoesNotAnswerTheChunkAskedFor) {
  this->replies = {empty_open(), open_reply(0x45, TRANSFER_STATUS_ACCEPTED, 3), block_reply(0x45, 7, {0x80, 0, 1}),
                   block_reply(0x45, 0, {})};
  this->queue();
  this->run();

  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_FALSE(this->results[0].complete);
  EXPECT_EQ(this->sent_bytes(3), (std::vector<uint8_t>{0x4A, 0x45, 0x00, 0x00}));
}

TEST_F(AtlanticReaderTest, WaitsWhileTheHubHoldsTheRadio) {
  this->held = true;
  this->queue();
  this->run(5);
  EXPECT_TRUE(this->sent.empty());
  EXPECT_EQ(this->delays.back(), AtlanticReader::BUSY_RETRY_MS);

  this->held = false;
  this->replies = {empty_open(), open_reply(0x46, TRANSFER_STATUS_ADAPTED, 3), block_reply(0x46, 1, {0x80, 0, 1}),
                   block_reply(0x46, 0, {})};
  this->run();
  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_TRUE(this->results[0].complete);
}

TEST_F(AtlanticReaderTest, RunsQueuedReadsOneAfterTheOtherInOrder) {
  this->replies = {empty_open(),
                   open_reply(0x47, TRANSFER_STATUS_ADAPTED, 3),
                   block_reply(0x47, 1, {0x80, 0x2C, 0xBD}),
                   block_reply(0x47, 0, {}),
                   empty_open(),
                   open_reply(0x48, TRANSFER_STATUS_ADAPTED, 3),
                   block_reply(0x48, 1, {0x80, 0x24, 0xAC}),
                   block_reply(0x48, 0, {})};
  this->queue(TRANSFER_OFFSET_ATLANTIC_HEATING_ENERGY);
  this->queue(HOT_WATER);
  this->run();

  ASSERT_EQ(this->results.size(), 2u);
  EXPECT_EQ(this->results[0].data, (std::vector<uint8_t>{0x80, 0x2C, 0xBD}));
  EXPECT_EQ(this->results[1].data, (std::vector<uint8_t>{0x80, 0x24, 0xAC}));
  EXPECT_EQ(this->sent_bytes(4)[7], 0x25) << "the second read opens its own offset";
}

TEST_F(AtlanticReaderTest, RefusesAReadBeyondTheQueueLimitAtOnce) {
  for (size_t i = 0; i <= AtlanticReader::QUEUE_MAX; i++)
    this->queue();
  EXPECT_TRUE(this->results.empty());
  this->queue();
  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_FALSE(this->results[0].complete);
}

TEST_F(AtlanticReaderTest, SkipsADeviceTheHubDoesNotKnow) {
  this->reader.queue("FFFFFF", HOT_WATER, TRANSFER_BLOCK_SIZE,
                     [this](bool complete, const uint8_t *, uint16_t) { this->results.push_back({complete, {}}); });
  this->run();
  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_FALSE(this->results[0].complete);
  EXPECT_TRUE(this->sent.empty());
}

TEST_F(AtlanticReaderTest, WaitsForAnotherControllerToGoQuietBeforeOpening) {
  esphome::test_clock::ManualClock clock(100000);
  // A TaHoma sharing the hub's id opens an entry of the heat pump.
  this->reader.on_frame(
      make_frame(HUB_ID, PUMP_ID, CMD_TRANSFER_OPEN, {0x08, 0x0C, 0x00, 0x10, 0x01, 0x00, 0xF0, 0, 0}));
  this->queue();
  this->run(3);
  EXPECT_TRUE(this->sent.empty());
  EXPECT_EQ(this->delays.back(), AtlanticReader::FOREIGN_RETRY_MS);

  esphome::test_clock::advance_ms(AtlanticReader::FOREIGN_HOLD_MS);
  this->replies = {empty_open(), open_reply(0x49, TRANSFER_STATUS_ADAPTED, 3), block_reply(0x49, 1, {0x80, 0, 1}),
                   block_reply(0x49, 0, {})};
  this->run();
  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_TRUE(this->results[0].complete);
}

TEST_F(AtlanticReaderTest, ClosesAndGivesUpWhenARoomThermostatWritesMidRead) {
  esphome::test_clock::ManualClock clock(100000);
  this->replies = {empty_open(), open_reply(0x4A, TRANSFER_STATUS_ADAPTED, 3), block_reply(0x4A, 0, {})};
  this->queue();
  this->run(2);  // prepare open, then data open accepted: the block request is next
  ASSERT_EQ(this->sent.size(), 2u);
  // A thermostat writes its room temperature to the heat pump (not addressed to the hub).
  this->reader.on_frame(
      make_frame(THERMOSTAT_ID, PUMP_ID, TRANSFER_WRITE_OPEN, {0x00, 0x0C, 0x00, 0x10, 0x01, 0x01, 0x8B, 0x00, 0x02}));
  this->run();

  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_FALSE(this->results[0].complete);
  ASSERT_EQ(this->sent.size(), 3u);
  EXPECT_EQ(this->sent_bytes(2), (std::vector<uint8_t>{0x4A, 0x4A, 0x00, 0x00})) << "the open transfer is closed";
}

TEST_F(AtlanticReaderTest, AnAnswerToTheHubIsNotAnotherController) {
  esphome::test_clock::ManualClock clock(100000);
  this->reader.on_frame(open_reply(0x41, TRANSFER_STATUS_ADAPTED, 3));
  // Nor is any frame outside the data transport.
  this->reader.on_frame(make_frame(HUB_ID, PUMP_ID, CMD_EXECUTE, {0x00}));
  this->replies = {empty_open(), open_reply(0x4B, TRANSFER_STATUS_ADAPTED, 3), block_reply(0x4B, 1, {0x80, 0, 1}),
                   block_reply(0x4B, 0, {})};
  this->queue();
  this->run();
  ASSERT_EQ(this->results.size(), 1u);
  EXPECT_TRUE(this->results[0].complete);
}

}  // namespace
