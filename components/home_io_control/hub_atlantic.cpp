#include "hub_internal.h"

#include <utility>

namespace esphome {
namespace home_io_control {

// Built here, not in the reader, because set_timeout() and send_and_receive_() are protected hub members.
// The lambdas capture `this`: the reader is a member of the hub, so it never outlives it.
AtlanticReaderHub IOHomeControlComponent::make_atlantic_reader_hub_() {
  AtlanticReaderHub hub;
  hub.set_timeout = [this](const char *name, uint32_t delay_ms, std::function<void()> callback) {
    this->set_timeout(name, delay_ms, std::move(callback));
  };
  // The conditions under which loop() keeps the radio from a background poll (see defer_background_poll_()).
  hub.radio_held = [this]() {
    return this->busy_ || this->radio_test_mode_ || this->key_extraction_awaiting_reply_() ||
           this->oneway_receiver_.defers_background_poll(true, millis());
  };
  hub.find_device = [this](const std::string &device_id) -> const IoDevice * { return this->get_device(device_id); };
  // The heat pump answers on the two-way channel (CH2), like the TaHoma's reads of it.
  hub.exchange = [this](const IoFrame &request, IoFrame &reply) {
    return this->send_and_receive_(request, reply, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;
  };
  return hub;
}

}  // namespace home_io_control
}  // namespace esphome
