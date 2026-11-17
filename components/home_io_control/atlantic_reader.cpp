#include "atlantic_reader.h"

#include "proto_commands.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstring>
#include <utility>

namespace esphome {
namespace home_io_control {

namespace {
const char *const TAG = "home_io_control.atlantic";
/// Name of the one timeout the read chain uses (a single chain: a step schedules only the next).
const char *const TIMEOUT_NAME = "atlantic_read";
}  // namespace

void AtlanticReader::on_frame(const IoFrame &frame) {
  if (frame.cmd < CMD_TRANSFER_OPEN || frame.cmd > CMD_UNKNOWN4A_RESP)
    return;
  // Ours never come back here (the radio does not hear its own transmissions, and the answers to our own
  // requests are taken by the exchange). So a request sent under our id, or any frame not addressed to us,
  // belongs to another controller: a TaHoma sharing our id, or any other one, thermostats included.
  if (memcmp(frame.src, this->node_id_, NODE_ID_SIZE) != 0 && memcmp(frame.dst, this->node_id_, NODE_ID_SIZE) == 0)
    return;
  if (!this->foreign_active_() && this->reading_)
    ESP_LOGI(TAG, "another controller is using the data transport: reads wait");
  this->foreign_ms_ = millis();
  if (this->foreign_ms_ == 0)
    this->foreign_ms_ = 1;
}

bool AtlanticReader::foreign_active_() const {
  return this->foreign_ms_ != 0 && millis() - this->foreign_ms_ < FOREIGN_HOLD_MS;
}

void AtlanticReader::queue(const std::string &device_id, uint16_t offset, uint16_t max, AtlanticReadDone done) {
  if (this->reads_.size() > QUEUE_MAX) {
    ESP_LOGW(TAG, "%s: read queue full, offset 0x%04X skipped", device_id.c_str(), static_cast<unsigned>(offset));
    if (done)
      done(false, nullptr, 0);
    return;
  }
  this->reads_.push_back(Read{device_id, offset, max, std::move(done)});
  if (!this->reading_)
    this->start_next_();
}

void AtlanticReader::after_(uint32_t delay_ms, std::function<void()> step) {
  this->hub_.set_timeout(TIMEOUT_NAME, delay_ms, [this, step = std::move(step)]() mutable {
    if (this->hub_.radio_held()) {
      this->after_(BUSY_RETRY_MS, std::move(step));
      return;
    }
    step();
  });
}

void AtlanticReader::start_next_() {
  if (this->reads_.empty()) {
    this->reading_ = false;
    return;
  }
  this->reading_ = true;
  this->data_.clear();
  this->open_ = TransferOpenReply{};
  this->chunk_ = 0;
  this->retried_ = false;
  // Start on the next loop pass, never inside the caller (a sensor update or another read's callback).
  this->after_(0, [this]() { this->open_step_(true); });
}

void AtlanticReader::open_step_(bool prepare) {
  // No transfer of ours is open yet: wait until the other controller has gone quiet,
  // then start over from the prepare open.
  if (this->foreign_active_()) {
    this->after_(FOREIGN_RETRY_MS, [this]() { this->open_step_(true); });
    return;
  }
  const Read &read = this->reads_.front();
  const IoDevice *dev = this->hub_.find_device(read.device_id);
  if (dev == nullptr) {
    ESP_LOGW(TAG, "%s: not registered, read skipped", read.device_id.c_str());
    this->finish_(false, false);
    return;
  }
  // A TaHoma first opens the entry in the "prepare" object, which the heat pump answers empty, then in
  // the data object, which it answers with the data. Send the same pair; should the first open
  // already carry the data, read it from there.
  const TransferObject target{TRANSFER_OBJECT_CLASS_ATLANTIC,
                              prepare ? TRANSFER_OBJECT_ATLANTIC_PREPARE : TRANSFER_OBJECT_ATLANTIC_DATA, read.offset};
  IoFrame request;
  IoFrame reply;
  if (!create_transfer_open(request, this->node_id_, dev->node_id, target, dev->low_power)) {
    ESP_LOGE(TAG, "%s: offset 0x%04X is not readable", read.device_id.c_str(), static_cast<unsigned>(read.offset));
    this->finish_(false, false);
    return;
  }
  TransferOpenReply open;
  const bool answered = this->hub_.exchange(request, reply) && reply.cmd == CMD_TRANSFER_OPEN_RESP &&
                        decode_transfer_open_reply(reply, open);
  const bool has_data = answered && open.ok() && open.length > 0;

  if (!has_data) {
    if (prepare) {
      this->after_(TRANSFER_BLOCK_GAP_MS, [this]() { this->open_step_(false); });
      return;
    }
    // The heat pump often answers a data open empty (about one in three on air): try the pair once more.
    if (!this->retried_) {
      this->retried_ = true;
      this->after_(TRANSFER_BLOCK_GAP_MS, [this]() { this->open_step_(true); });
      return;
    }
    if (!answered) {
      ESP_LOGW(TAG, "%s: no well-formed answer to the open of offset 0x%04X", read.device_id.c_str(),
               static_cast<unsigned>(read.offset));
    } else {
      ESP_LOGW(TAG, "%s: open of offset 0x%04X refused (status %u, length %u)", read.device_id.c_str(),
               static_cast<unsigned>(read.offset), static_cast<unsigned>(open.status),
               static_cast<unsigned>(open.length));
    }
    this->finish_(false, false);
    return;
  }

  this->open_ = open;
  if (open.length > read.max) {
    ESP_LOGW(TAG, "%s: offset 0x%04X announces %u bytes, more than the %u expected", read.device_id.c_str(),
             static_cast<unsigned>(read.offset), static_cast<unsigned>(open.length), static_cast<unsigned>(read.max));
    this->finish_(true, false);
    return;
  }
  this->data_.reserve(open.length);
  this->chunk_ = transfer_chunk_count(open.length);
  this->after_(TRANSFER_BLOCK_GAP_MS, [this]() { this->block_step_(); });
}

void AtlanticReader::block_step_() {
  const Read &read = this->reads_.front();
  // The other controller started a transfer while ours is open: free the heat pump at once (close)
  // rather than interleave with it. The read fails; a periodic one is simply retried next time.
  if (this->foreign_active_()) {
    ESP_LOGW(TAG, "%s: offset 0x%04X: another controller is reading, read abandoned", read.device_id.c_str(),
             static_cast<unsigned>(read.offset));
    this->finish_(true, false);
    return;
  }
  const IoDevice *dev = this->hub_.find_device(read.device_id);
  if (dev == nullptr) {
    this->finish_(false, false);
    return;
  }
  IoFrame request;
  IoFrame reply;
  const uint8_t *block = nullptr;
  uint8_t block_len = 0;
  const uint8_t handle = this->open_.handle;
  if (!create_transfer_read(request, this->node_id_, dev->node_id, handle, this->chunk_, dev->low_power) ||
      !this->hub_.exchange(request, reply) || !decode_transfer_block(reply, handle, this->chunk_, block, block_len) ||
      this->data_.size() + block_len > read.max) {
    ESP_LOGW(TAG, "%s: offset 0x%04X: incomplete answer (%u of %u bytes)", read.device_id.c_str(),
             static_cast<unsigned>(read.offset), static_cast<unsigned>(this->data_.size()),
             static_cast<unsigned>(this->open_.length));
    this->finish_(true, false);
    return;
  }
  this->data_.insert(this->data_.end(), block, block + block_len);
  this->chunk_--;
  if (this->chunk_ >= 1) {
    this->after_(TRANSFER_BLOCK_GAP_MS, [this]() { this->block_step_(); });
    return;
  }
  this->finish_(true, true);
}

void AtlanticReader::finish_(bool close, bool complete) {
  const auto report = [this, complete]() {
    const Read read = std::move(this->reads_.front());
    this->reads_.erase(this->reads_.begin());
    // Copy out before the callback: it may queue another read, which clears data_.
    const std::vector<uint8_t> data = std::move(this->data_);
    this->data_.clear();
    if (read.done)
      read.done(complete, data.data(), static_cast<uint16_t>(data.size()));
    this->start_next_();
  };
  if (!close) {
    report();
    return;
  }
  // The device holds the handle until the transfer is closed, so every path past an accepted open
  // closes it, one gap after the last answer.
  this->after_(TRANSFER_BLOCK_GAP_MS, [this, report]() {
    const IoDevice *dev = this->hub_.find_device(this->reads_.front().device_id);
    IoFrame request;
    IoFrame reply;
    if (dev != nullptr && create_transfer_read(request, this->node_id_, dev->node_id, this->open_.handle,
                                               TRANSFER_CHUNK_CLOSE, dev->low_power))
      this->hub_.exchange(request, reply);
    report();
  });
}

}  // namespace home_io_control
}  // namespace esphome
