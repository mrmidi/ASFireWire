#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace ASFW::Async {

/**
 * \brief Zero-copy view of an AR packet for handler dispatch.
 *
 * Provides read-only access to packet header and payload without copying data.
 * All multi-byte fields are in BIG-ENDIAN (IEEE 1394 wire format).
 */
struct ARPacketView {
    std::span<const uint8_t> header;   ///< Packet header (12-16 bytes depending on tCode)
    std::span<const uint8_t> payload;  ///< Packet payload (0-N bytes depending on packet type)
    uint8_t tCode;                     ///< Transaction code (extracted from header first byte)
    uint16_t sourceID;                 ///< Source node ID (big-endian)
    uint16_t destID;                   ///< Destination node ID (big-endian)
    uint8_t tLabel;                    ///< Transaction label (6 bits)
};

/**
 * \brief Context type for packet routing (Request vs Response).
 */
enum class ARContextType : uint8_t {
    Request,   ///< Packet from AR Request context
    Response   ///< Packet from AR Response context
};

/**
 * \brief Packet handler callback type.
 *
 * Invoked by PacketRouter when packet with matching tCode is received.
 * Handler receives zero-copy view of packet data.
 *
 * \par Thread Safety
 * Handlers are invoked from interrupt context. Must complete quickly and
 * avoid blocking operations.
 */
using PacketHandler = std::function<void(const ARPacketView&)>;

/**
 * \brief Central dispatcher for AR (Asynchronous Receive) packets.
 *
 * Routes received packets to registered handlers based on tCode and context type.
 * Supports registration of separate handlers for request and response packets.
 *
 * \par OHCI Specification References
 * - §8.4.2: AR DMA packet stream format
 * - §8.7: AR packet formats (Figures 8-7 through 8-14)
 *
 * \par IEEE 1394 Transaction Codes
 * Per IEEE 1394-1995 §6.2, Table 6-1:
 * - 0x0: Write request (quadlet)
 * - 0x1: Write request (block)
 * - 0x2: Write response
 * - 0x4: Read request (quadlet)
 * - 0x5: Read request (block)
 * - 0x6: Read response (quadlet)
 * - 0x7: Read response (block)
 * - 0x8: Cycle start packet
 * - 0x9: Lock request
 * - 0xA: Isochronous block / Async stream
 * - 0xB: Lock response
 * - 0xE: PHY packet
 *
 * \par Design Rationale
 * - **Zero-copy**: Uses std::span to avoid copying packet data
 * - **Functional handlers**: std::function allows lambdas and captures
 * - **Separate request/response tables**: Different tCode space for each context
 * - **Single-threaded**: No locking (caller must serialize)
 *
 * \par Apple Pattern
 * Similar to AppleFWOHCI packet dispatch:
 * - processPacket() extracts tCode and routes to handler
 * - Separate paths for requests vs responses
 * - Handlers invoked synchronously from interrupt context
 *
 * \par Linux Pattern
 * See drivers/firewire/ohci.c handle_ar_packet():
 * - Switch on tCode (lines 1680-1710)
 * - Dispatches to fw_core_handle_request() or fw_core_handle_response()
 * - Zero-copy packet forwarding to core layer
 *
 * \par Usage Example
 * \code
 * PacketRouter router;
 *
 * // Register quadlet read request handler
 * router.RegisterRequestHandler(0x4, [](const ARPacketView& pkt) {
 *     // Extract destination offset from header
 *     uint64_t offset = ExtractOffset(pkt.header);
 *     // Service request...
 * });
 *
 * // Register read response handler
 * router.RegisterResponseHandler(0x6, [](const ARPacketView& pkt) {
 *     // Match to pending request by tLabel
 *     // Complete transaction...
 * });
 *
 * // Route packet from AR Request buffer
 * router.RoutePacket(ARContextType::Request, std::span<const uint8_t>(bufferData, bufferSize));
 * \endcode
 *
 * \warning Handlers are invoked synchronously from RoutePacket(). They must
 *          complete quickly to avoid blocking AR interrupt processing.
 */
class PacketRouter {
public:
    PacketRouter() = default;
    ~PacketRouter() = default;

    /**
     * \brief Register handler for AR Request packets with specified tCode.
     *
     * \param tCode Transaction code (0x0-0xF)
     * \param handler Callback to invoke when packet received
     *
     * \par Usage
     * Register handlers for incoming requests that need servicing:
     * - 0x0/0x1: Write requests (handle CSR/config ROM writes)
     * - 0x4/0x5: Read requests (handle CSR/config ROM reads)
     * - 0x9: Lock requests (handle atomic operations)
     * - 0xE: PHY packets (handle link-on, self-ID during bus reset)
     *
     * \warning Overwrites any previously registered handler for this tCode.
     */
    void RegisterRequestHandler(uint8_t tCode, PacketHandler handler);

    /**
     * \brief Register handler for AR Response packets with specified tCode.
     *
     * \param tCode Transaction code (0x2, 0x6, 0x7, 0xB)
     * \param handler Callback to invoke when packet received
     *
     * \par Usage
     * Register handlers for responses to local AT requests:
     * - 0x2: Write response (complete pending write)
     * - 0x6: Read response quadlet (extract data, complete read)
     * - 0x7: Read response block (extract data, complete read)
     * - 0xB: Lock response (extract data, complete lock)
     *
     * \warning Overwrites any previously registered handler for this tCode.
     */
    void RegisterResponseHandler(uint8_t tCode, PacketHandler handler);

    /**
     * \brief Route packet from AR buffer to registered handler.
     *
     * Parses packet buffer, extracts packets one-by-one, and dispatches each
     * packet to its registered handler based on tCode and context type.
     *
     * \param contextType AR Request or AR Response context
     * \param packetData Buffer containing packet stream (may have multiple packets)
     * \param packetSize Size of buffer in bytes
     *
     * \par Implementation
     * 1. Use ARPacketParser::ParseNext() to extract packets from buffer
     * 2. For each packet:
     *    a. Extract tCode from header first byte (bits[7:4])
     *    b. Build ARPacketView with zero-copy spans
     *    c. Lookup handler in requestHandlers_ or responseHandlers_
     *    d. Invoke handler(view) if registered, else log warning
     * 3. Continue until buffer exhausted
     *
     * \par OHCI §8.4.2
     * "AR buffers contain a stream of packets. Each packet consists of:
     *  - Packet header (variable length based on tCode)
     *  - Packet data (optional, based on tCode and data_length)
     *  - 4-byte trailer (xferStatus | timeStamp)
     * Software must parse the stream to extract individual packets."
     *
     * \par Thread Safety
     * Not thread-safe. Caller must serialize RoutePacket() calls (typically
     * invoked from single interrupt handler thread).
     *
     * \par Phase 2.2
     * Signature updated to use std::span for type-safe buffer access.
     */
    void RoutePacket(ARContextType contextType, std::span<const uint8_t> packetData);

    /**
     * \brief Clear all registered handlers.
     *
     * Removes all request and response handlers. Useful for shutdown/reset.
     */
    void ClearAllHandlers();

    PacketRouter(const PacketRouter&) = delete;
    PacketRouter& operator=(const PacketRouter&) = delete;

private:
    /// Registered handlers for AR Request packets, indexed by tCode
    std::array<PacketHandler, 16> requestHandlers_;

    /// Registered handlers for AR Response packets, indexed by tCode
    std::array<PacketHandler, 16> responseHandlers_;

    /**
     * \brief Extract tCode from packet header first byte (Phase 2.2: std::span).
     *
     * \param header Packet header bytes (big-endian)
     * \return tCode value (4 bits, range 0-15)
     *
     * \par IEEE 1394 Wire Format
     * Per IEEE 1394-1995 §6.2, control quadlet format:
     * - Byte 0: destID[15:10]
     * - Byte 1: destID[9:2]
     * - Byte 2: destID[1:0] | tl[7:2]
     * - Byte 3: tl[1:0] | rt[1:0] | tCode[3:0]
     *
     * tCode is in bits[3:0] of byte 3 (fourth byte).
     */
    static uint8_t ExtractTCode(std::span<const uint8_t> header) noexcept;

    /**
     * \brief Extract source ID from packet header (Phase 2.2: std::span).
     *
     * \param header Packet header bytes (big-endian)
     * \return Source node ID (16 bits, big-endian)
     *
     * \par IEEE 1394 Wire Format
     * sourceID is at bytes [4-5] of async packet header (OHCI Figure 8-7).
     */
    static uint16_t ExtractSourceID(std::span<const uint8_t> header) noexcept;

    /**
     * \brief Extract destination ID from packet header (Phase 2.2: std::span).
     *
     * \param header Packet header bytes (big-endian)
     * \return Destination node ID (16 bits, big-endian)
     *
     * \par IEEE 1394 Wire Format
     * destinationID is at bytes [0-1] of async packet header.
     */
    static uint16_t ExtractDestID(std::span<const uint8_t> header) noexcept;

    /**
     * \brief Extract transaction label from packet header (Phase 2.2: std::span).
     *
     * \param header Packet header bytes (big-endian)
     * \return tLabel value (6 bits, range 0-63)
     *
     * \par IEEE 1394 Wire Format
     * tLabel is split across bytes 2-3:
     * - Byte 2 bits[7:2]: tLabel[5:0] upper bits
     * - Byte 3 bits[7:6]: tLabel[1:0] lower bits
     */
    static uint8_t ExtractTLabel(std::span<const uint8_t> header) noexcept;
};

} // namespace ASFW::Async
