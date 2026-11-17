#pragma once
/// @file atlantic_reader.h
/// @brief Queued, non-blocking reads of Atlantic heat pump entries over the data transport.
/// @ingroup hioc_hub

#include "device_registry.h"
#include "hub_hooks.h"
#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace esphome {
namespace home_io_control {

/// Called when a queued Atlantic read ends: @p complete is true when every announced block was read;
/// @p data / @p len hold what was read (possibly nothing). The buffer is only valid during the call.
using AtlanticReadDone = std::function<void(bool complete, const uint8_t *data, uint16_t len)>;

/// @brief The calls AtlanticReader reaches the hub through, injected by the hub
/// (IOHomeControlComponent::make_atlantic_reader_hub_()).
struct AtlanticReaderHub {
  NamedTimeoutFn set_timeout;        ///< The hub's set_timeout(), which is protected.
  std::function<bool()> radio_held;  ///< True while loop() keeps the radio (busy, test mode, ...).
  std::function<const IoDevice *(const std::string &device_id)> find_device;  ///< Registry lookup.
  /// One request/answer exchange on the two-way channel; false unless the device answered.
  std::function<bool(const IoFrame &request, IoFrame &reply)> exchange;
};

/// @brief Reads entries of an Atlantic heat pump, one at a time, without blocking the loop.
///
/// Each read opens the entry in TRANSFER_OBJECT_ATLANTIC_PREPARE, then in TRANSFER_OBJECT_ATLANTIC_DATA,
/// as a TaHoma does, reads its blocks and closes the transfer. Read-only: create_transfer_open() only opens
/// the offsets of TRANSFER_OFFSETS_ATLANTIC. Each step is one exchange; the TRANSFER_BLOCK_GAP_MS gap
/// between two requests is a timeout, so the loop runs in between. A step waits while the hub keeps the
/// radio, and while another controller uses the data transport (on_frame()). A data open answered empty
/// is re-sent once.
class AtlanticReader {
 public:
  /// Waiting reads allowed behind the one in progress: a full scan of TRANSFER_OFFSETS_ATLANTIC plus the
  /// periodic reads that join it.
  static constexpr size_t QUEUE_MAX = 80;
  /// Quiet time required after the last transfer frame of another controller. A TaHoma app session
  /// reads ~35 entries with gaps under a second; its 10-min polls take a few seconds.
  static constexpr uint32_t FOREIGN_HOLD_MS = 15000;
  /// Re-check delay of a read waiting for that quiet time.
  static constexpr uint32_t FOREIGN_RETRY_MS = 1000;
  /// Re-check delay of a step that found the radio held by the hub.
  static constexpr uint32_t BUSY_RETRY_MS = 100;

  AtlanticReader(const uint8_t *node_id, AtlanticReaderHub hub) : node_id_(node_id), hub_(std::move(hub)) {}

  /// @brief Queue a read of one entry of the heat pump's data object.
  /// @param device_id Hexadecimal node ID string of the heat pump, already in the registry.
  /// @param offset Entry offset, one of TRANSFER_OFFSETS_ATLANTIC.
  /// @param max Longest answer accepted; an entry announced longer is closed unread.
  /// @param done Called once, from the loop, when the read ends (also on refusal or a full queue).
  void queue(const std::string &device_id, uint16_t offset, uint16_t max, AtlanticReadDone done);
  /// @brief Note a received frame: a data-transport frame (0x46-0x4B) sent under this hub's id, or not
  /// addressed to it, is another controller's, and holds the reads for FOREIGN_HOLD_MS.
  void on_frame(const IoFrame &frame);

 protected:
  struct Read {
    std::string device_id;
    uint16_t offset{0};
    uint16_t max{0};
    AtlanticReadDone done;
  };

  [[nodiscard]] bool foreign_active_() const;
  /// Run @p step after @p delay_ms; while the hub holds the radio, retry it a little later.
  void after_(uint32_t delay_ms, std::function<void()> step);
  void start_next_();
  /// Open the front read's entry: in the prepare object when @p prepare, else in the data object.
  void open_step_(bool prepare);
  void block_step_();
  /// Close the transfer (when @p close), report to the read's callback and start the next read.
  void finish_(bool close, bool complete);

  const uint8_t *node_id_;
  AtlanticReaderHub hub_;
  std::vector<Read> reads_;  ///< Waiting reads, oldest first; the front one is in progress while reading_.
  bool reading_{false};
  TransferOpenReply open_{};   ///< Answer to the open of the read in progress.
  std::vector<uint8_t> data_;  ///< Bytes read so far.
  uint16_t chunk_{0};          ///< Next block to request (counting down to 1).
  bool retried_{false};        ///< The data open was already re-sent.
  uint32_t foreign_ms_{0};     ///< millis() of the last foreign transfer frame; 0 = none yet.
};

}  // namespace home_io_control
}  // namespace esphome
