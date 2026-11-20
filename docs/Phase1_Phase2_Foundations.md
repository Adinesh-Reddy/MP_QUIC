# MP-QUIC Comprehensive Technical Reference: Phase 1 & Phase 2

This document provides a line-by-line, file-by-file technical reference for **Phase 1 (Linux UDP Sockets & epoll Reactor)** and **Phase 2 (Packet Header & Frame Wire Formats)** in the MP-QUIC codebase. It combines the source code implementation details, internal data structures, POSIX system call mechanics, and all architectural principles and design patterns.

---

## Table of Contents
1. [Architectural Overview & Layering](#1-architectural-overview--layering)
2. [Phase 1: Operating System & Socket Layer](#2-phase-1-operating-system--socket-layer)
   - 2.1 [`include/sockets/udp_socket.hpp` & `src/sockets/udp_socket.cpp`](#21-includesocketsudp_sockethpp--srcsocketsudp_socketcpp)
   - 2.2 [`include/sockets/socket_manager.hpp` & `src/sockets/socket_manager.cpp`](#22-includesocketssocket_managerhpp--srcsocketssocket_managercpp)
   - 2.3 Deep Dive: Blocking vs. Non-Blocking I/O & Multipath Deadlocks
   - 2.4 Deep Dive: The `epoll` Reactor & Socket Draining Mechanics
   - 2.5 Deep Dive: The Callback Decoupling Architecture
   - 2.6 Integration Verification: [`tests/integration/test_packet_socket.cpp`](#26-integration-verification-test_packet_socketcpp)
3. [Phase 2: Binary Wire Protocol & Framing](#3-phase-2-binary-wire-protocol--framing)
   - 3.1 [`include/packet/packet.hpp` & `src/packet/packet.cpp`](#31-includepacketpackethpp--srcpacketpacketcpp)
   - 3.2 [`include/packet/frame.hpp` & `src/packet/frame.cpp`](#32-includepacketframehpp--srcpacketframecpp)
   - 3.3 Deep Dive: The 18-Byte Public Header & Fast Header Peeking
   - 3.4 Deep Dive: Packet Sequence Numbers vs. Stream Byte Offsets
   - 3.5 Deep Dive: Zero-Handshake Connection Establishment (0-RTT)
   - 3.6 Deep Dive: Dynamic Path Discovery & The Odd/Even ID Rule
   - 3.7 Deep Dive: The 7 Frame Specifications & Wire Encodings
   - 3.8 Deep Dive: Variable-Length Integer (Varint) Compression
   - 3.9 Deep Dive: SACK Ranges & Interval Merging
   - 3.10 Deep Dive: The 6 Receiver ACK Decision Rules
   - 3.11 Deep Dive: Infinite ACK Loop Prevention
   - 3.12 Deep Dive: Retransmittability & Piggybacked Frames
   - 3.13 Deep Dive: Cross-Path Acknowledgments
   - 3.14 Deep Dive: Dual-Level Flow Control & `WINDOW_UPDATE` Frames
   - 3.15 Unit Test Verification: [`tests/unit/test_packet.cpp`](#315-unit-test-verification-test_packetcpp)

---

## 1. Architectural Overview & Layering

MP-QUIC Mini decouples the physical transmission medium from logical streams. Phases 1 and 2 establish the bottom two layers of the stack:

```
┌────────────────────────────────────────────────────────────────────────┐
│                          APPLICATION LAYER                             │
│                  (mpq_server, mpq_client, mpq_reqres)                  │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Application writes & callbacks
┌───────────────────────────────────▼────────────────────────────────────┐
│                    PHASE 2: PACKET & FRAME LAYER                       │
│    - Packet: 18-Byte Header (flags, CID, path_id, packet_number)       │
│    - Frame: Tagged union (STREAM, ACK, WINDOW_UPDATE, PING, etc.)      │
│    - Big-endian codec & 2-bit prefixed varint compression              │
│    - Selective Acknowledgment (SACK) range tracking                    │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Serialized byte buffers (≤ 1350 B)
┌───────────────────────────────────▼────────────────────────────────────┐
│                    PHASE 1: SOCKET & REACTOR LAYER                     │
│    - UDPSocket: POSIX non-blocking datagram sockets (SOCK_NONBLOCK)    │
│    - SocketManager: Linux epoll event loop (epoll_wait)               │
│    - Socket draining loop (EAGAIN handling)                            │
│    - Integrated timer ticks (on_tick heartbeat)                        │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ POSIX syscalls (sendto / recvfrom)
┌───────────────────────────────────▼────────────────────────────────────┐
│                         LINUX KERNEL & NICs                            │
│        (4 MiB SO_RCVBUF / SO_SNDBUF, Path 0 Wi-Fi, Path 1 LTE)         │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Phase 1: Operating System & Socket Layer

### 2.1 `include/sockets/udp_socket.hpp` & `src/sockets/udp_socket.cpp`

This class wraps raw Linux POSIX UDP socket operations in RAII C++ semantics.

#### Header Definition ([`include/sockets/udp_socket.hpp`](file:///home/adinesh-reddy/MP_QUIC/include/sockets/udp_socket.hpp))
```cpp
class UDPSocket
{
private:
    int fd = -1; // Underlying Linux file descriptor

public:
    UDPSocket();
    ~UDPSocket();

    // Prevent copying (sockets own unique kernel resources)
    UDPSocket(const UDPSocket&) = delete;
    UDPSocket& operator=(const UDPSocket&) = delete;

    ssize_t sendTo(const uint8_t *data, size_t length, const string &dest_ip, uint16_t dest_port);
    ssize_t recvFrom(uint8_t *buffer, size_t max_length, string &src_ip, uint16_t &src_port);

    bool bind(const string& ip, uint16_t port);
    uint16_t get_local_port();
    bool set_buffer_size(int bytes);
    bool isValid() const;
    void close();
    int get_fd();
};
```

#### Detailed Method Breakdown ([`src/sockets/udp_socket.cpp`](file:///home/adinesh-reddy/MP_QUIC/src/sockets/udp_socket.cpp))

1. **`UDPSocket::UDPSocket()` (Lines 11–17):**
   ```cpp
   fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
   ```
   * Creates an IPv4 (`AF_INET`) datagram (`SOCK_DGRAM`) socket.
   * Atomically sets `SOCK_NONBLOCK` during creation to prevent blocking.

2. **`UDPSocket::bind(const string &ip, uint16_t port)` (Lines 24–45):**
   * Configures a `sockaddr_in` struct with `sin_family = AF_INET` and converts the port to network byte order using `htons(port)`.
   * Calls `inet_pton(AF_INET, ip.c_str(), &addr.sin_addr)` to parse IPv4 dotted-decimal strings into binary.
   * Invokes `::bind(fd, (struct sockaddr *)&addr, sizeof(addr))`.
   * If `port == 0`, the Linux kernel automatically binds to an available ephemeral port.

3. **`UDPSocket::sendTo(...)` (Lines 47–66):**
   * Converts destination IP string to binary via `inet_pton` and port via `htons`.
   * Calls POSIX `sendto(fd, data, length, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr))`.
   * Returns bytes sent or `-1` on error.

4. **`UDPSocket::recvFrom(...)` (Lines 68–84):**
   * Calls POSIX `recvfrom(fd, buffer, max_length, 0, (struct sockaddr *)&src_addr, &addr_len)`.
   * If bytes $> 0$, extracts sender IP using `inet_ntop(AF_INET, &src_addr.sin_addr, ...)` and sender port using `ntohs(src_addr.sin_port)`.
   * Returns bytes read, or `-1` if empty/error.

5. **`UDPSocket::set_buffer_size(int bytes)` (Lines 99–122):**
   * Invokes `setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes))` and `SO_SNDBUF`.
   * Reads back actual allocated size with `getsockopt`. On Linux, the kernel doubles the requested size for internal bookkeeping headers, clamped by `/proc/sys/net/core/rmem_max` and `wmem_max`. Emits a warning if capped.

6. **`UDPSocket::get_local_port()` (Lines 86–97):**
   * Calls `getsockname()` to retrieve the port assigned by the kernel when bound to port 0.

7. **`UDPSocket::~UDPSocket()` & `close()` (Lines 19–22, 134–140):**
   * Closes `fd` via `::close(fd)` and resets to `-1`.

---

### 2.2 `include/sockets/socket_manager.hpp` & `src/sockets/socket_manager.cpp`

This class implements the single-threaded **Reactor / Event Loop** engine.

#### Header Definition ([`include/sockets/socket_manager.hpp`](file:///home/adinesh-reddy/MP_QUIC/include/sockets/socket_manager.hpp))
```cpp
typedef function<void(int fd, const uint8_t *data, size_t len, string ip, uint16_t port)> RecvCallback;
typedef function<void()> TickCallback;

class SocketManager
{
public:
    SocketManager();
    ~SocketManager();

    bool add_socket(UDPSocket *sock);
    void on_receive(RecvCallback cb);
    void on_tick(TickCallback cb);
    void run(int tick_ms = 10);
    void stop();

private:
    int epoll_fd = -1;
    bool running = false;
    unordered_map<int, UDPSocket *> sockets;
    RecvCallback callback = nullptr;
    TickCallback tick_callback = nullptr;
};
```

#### Detailed Method Breakdown ([`src/sockets/socket_manager.cpp`](file:///home/adinesh-reddy/MP_QUIC/src/sockets/socket_manager.cpp))

1. **`SocketManager::SocketManager()` (Lines 9–15):**
   * Initializes Linux epoll instance: `epoll_fd = epoll_create1(0)`.
2. **`SocketManager::add_socket(UDPSocket *sock)` (Lines 26–43):**
   * Sets up `epoll_event` with `event.events = EPOLLIN` (monitor readability) and `event.data.fd = sock->get_fd()`.
   * Registers socket via `epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock->get_fd(), &event)`.
   * Stores socket pointer in `unordered_map<int, UDPSocket *> sockets`.
3. **`SocketManager::run(int tick_ms)` (Lines 55–102):**
   * Allocates `epoll_event events[16]` on stack and a 64 KiB read buffer (`buffer[65536]`).
   * Enters main loop:
     1. `epoll_wait(epoll_fd, events, 16, tick_ms)` blocks with zero CPU usage until data is ready or `tick_ms` expires.
     2. Handles `EINTR` interrupts by continuing loop.
     3. For each ready `fd`, drains the socket via `while (true) { sock->recvFrom(...) }` until `received <= 0`.
     4. Dispatches each datagram via `callback(...)`.
     5. Calls `tick_callback()` on every iteration to drive protocol timers.
4. **`SocketManager::stop()` (Lines 104–107):**
   * Sets `running = false`, cleanly terminating the `while (running)` loop.

---

### 2.3 Deep Dive: Blocking vs. Non-Blocking I/O & Multipath Deadlocks

In single-path TCP, blocking I/O is manageable because there is only one socket. In Multipath transport, blocking I/O causes fatal deadlocks:

```
┌────────────────────────────────────────────────────────────────────────┐
│                        THE MULTIPATH DEADLOCK                          │
├────────────────────────────────────────────────────────────────────────┤
│ 1. Application has 2 interfaces: Wi-Fi (Path 0) and LTE (Path 1).      │
│ 2. Single thread calls blocking recvfrom() on Path 0 (Wi-Fi).          │
│ 3. Wi-Fi path is currently idle (no incoming packets).                 │
│ 4. THREAD BLOCKS IN KERNEL SLEEP on Path 0!                            │
│ 5. Meanwhile, 20 packets arrive on Path 1 (LTE) -> UNREAD / STALLED!   │
│ 6. Meanwhile, Path 0's RTO timer expires -> UNNOTICED / NO RESEND!     │
└────────────────────────────────────────────────────────────────────────┘
```

**The Non-Blocking Solution (`SOCK_NONBLOCK`):**
Non-blocking sockets guarantee that `recvfrom()` never blocks. If the kernel queue is empty, it returns `-1` immediately. Combined with `epoll`, the thread only wakes when packets are actually present.

---

### 2.4 Deep Dive: The `epoll` Reactor & Socket Draining Mechanics

```cpp
while (running) {
    int num_events = epoll_wait(epoll_fd, events, 16, tick_ms);

    for (int i = 0; i < num_events; i++) {
        int ready_fd = events[i].data.fd;
        UDPSocket *sock = sockets[ready_fd];

        // Socket Draining Loop:
        while (true) {
            ssize_t received = sock->recvFrom(buffer, sizeof(buffer), src_ip, src_port);
            if (received <= 0) break; // EAGAIN: queue is now empty
            callback(ready_fd, buffer, received, src_ip, src_port);
        }
    }

    if (tick_callback != nullptr) {
        tick_callback(); // Runs loss timers, delayed ACKs, send_pending()
    }
}
```

#### Why Socket Draining is Mandatory:
`epoll_wait` uses level-triggered polling. When a socket receives a burst of 10 packets, `epoll_wait` wakes up once. If the code only called `recvFrom()` once, the remaining 9 packets would remain stuck in the socket buffer until future `epoll_wait` turns, introducing artificial latency and jitter.
The inner `while (true)` loop completely empties the socket queue until `recvFrom()` returns `-1` with `EAGAIN`.

---

### 2.5 Deep Dive: The Callback Decoupling Architecture

`SocketManager` acts as a generic event dispatcher. By using `std::function`:
```cpp
manager.on_receive([&](int fd, const uint8_t *data, size_t len, string ip, uint16_t port) {
    client.handle_datagram(fd, data, len, ip, port);
});
manager.on_tick([&]() {
    client.on_tick();
});
```
* `SocketManager` has no dependencies on QUIC headers, packet parsing, or state.
* `Client` and `Server` plug into the network layer without modifying socket code.

---

### 2.6 Integration Verification: `test_packet_socket.cpp`

File: [`tests/integration/test_packet_socket.cpp`](file:///home/adinesh-reddy/MP_QUIC/tests/integration/test_packet_socket.cpp)
* **What it tests:** Creates two real `UDPSocket` instances on loopback (`127.0.0.1:19001` and `127.0.0.1:19002`), registers the receiver with `SocketManager`, launches `SocketManager::run(10)` in a background `std::thread`, and transmits an MP-QUIC packet containing a `FRAME_STREAM` payload.
* **Assertions verified:**
  1. `manager.add_socket(&receiver_sock)` succeeds.
  2. Packet deserializes cleanly on arrival: `pkt.get_connection_id() == 1001`, `pkt.get_path_id() == 1`, `pkt.get_packet_num() == 42`.
  3. Frame payload matches `"End-to-end packet transmission test"`.
  4. `tick_count.load() > 0` verifies that timer ticks fire periodically.

---

## 3. Phase 2: Binary Wire Protocol & Framing

### 3.1 `include/packet/packet.hpp` & `src/packet/packet.cpp`

Defines the binary envelope, header packing, and packet-level serialization.

#### Header Definition ([`include/packet/packet.hpp`](file:///home/adinesh-reddy/MP_QUIC/include/packet/packet.hpp))
```cpp
const uint8_t FLAG_NONE = 0x00;
const size_t MAX_PACKET_SIZE = 1350; // Caps UDP payload to fit inside 1500 MTU

#pragma pack(push, 1)
struct PacketHeader
{
    uint8_t  flags;
    uint64_t connection_id;
    uint8_t  path_id;
    uint64_t packet_number;
};
#pragma pack(pop)

class Packet
{
public:
    static constexpr size_t HEADER_SIZE = 18;

    Packet();
    Packet(uint8_t flags, uint64_t connection_id, uint8_t path_id, uint64_t packet_number);

    // Getters & Setters
    uint8_t get_flags() const;
    uint64_t get_connection_id() const;
    uint8_t get_path_id() const;
    uint64_t get_packet_number() const;

    const vector<Frame> &get_frames() const;
    void add_frame(const Frame &frame);

    size_t size() const;
    bool is_retransmittable() const;

    vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t *buffer, size_t length, Packet &out_packet);
    static bool peek_header(const uint8_t *buffer, size_t length, uint64_t &connection_id, uint8_t &path_id);
    static uint64_t current_time_us();
};
```

#### Detailed Method Breakdown ([`src/packet/packet.cpp`](file:///home/adinesh-reddy/MP_QUIC/src/packet/packet.cpp))

1. **`Packet::size()` (Lines 95–102):**
   * Returns `18 + sum(frame.size())`.
2. **`Packet::is_retransmittable()` (Lines 104–111):**
   * Loops over `frames`. Returns `true` if any frame returns `frame.is_retransmittable() == true`. Returns `false` only if all frames are ACKs.
3. **`Packet::serialize()` (Lines 113–139):**
   * Enforces `total_size <= MAX_PACKET_SIZE` (1350 bytes); throws `std::length_error` otherwise.
   * Writes byte 0: `flags`.
   * Converts `connection_id` to network byte order using `htobe64()` and writes 8 bytes.
   * Writes byte 9: `path_id`.
   * Converts `packet_number` to network byte order using `htobe64()` and writes 8 bytes.
   * Calls `frame.serialize(buffer)` for every frame.
4. **`Packet::deserialize()` (Lines 141–178):**
   * Rejects buffers smaller than 18 bytes.
   * Reads `raw_cid` and `raw_pnum`, converting from network order with `be64toh()`.
   * Loops from `pos = 18` calling `Frame::parse(buffer + pos, length - pos, consumed, frame)`.
   * Rejects packets with 0 valid frames.
5. **`Packet::peek_header()` (Lines 180–191):**
   * Extracts `connection_id` and `path_id` from the first 10 bytes without parsing the rest of the packet.

---

### 3.2 `include/packet/frame.hpp` & `src/packet/frame.cpp`

Implements the 7 self-delimiting frame types and varint codecs.

#### Header Definition ([`include/packet/frame.hpp`](file:///home/adinesh-reddy/MP_QUIC/include/packet/frame.hpp))
```cpp
const uint8_t FRAME_STREAM           = 0x01;
const uint8_t FRAME_ACK              = 0x02;
const uint8_t FRAME_CONNECTION_CLOSE = 0x03;
const uint8_t FRAME_WINDOW_UPDATE    = 0x04;
const uint8_t FRAME_PING             = 0x07;
const uint8_t FRAME_ADD_ADDRESS      = 0x10;
const uint8_t FRAME_PATHS            = 0x12;

struct StreamFrame {
    uint32_t stream_id = 0;
    uint64_t offset = 0;
    bool fin = false;
    vector<uint8_t> data;
};

struct AckRange {
    uint64_t first;
    uint64_t last;
};

struct AckFrame {
    uint8_t path_id = 0;
    uint64_t largest_acked = 0;
    uint32_t ack_delay_us = 0;
    vector<AckRange> ranges;
    uint64_t lowest_acked() const;
    bool acks_packet(uint64_t packet_number) const;
};

struct WindowUpdateFrame {
    uint32_t stream_id = 0;
    uint64_t byte_offset = 0;
};

struct PathInfo {
    uint8_t path_id = 0;
    uint32_t rtt_us = 0;
};

struct PathsFrame {
    vector<PathInfo> paths;
};

struct NetworkAddress {
    string ip;
    uint16_t port = 0;
};

struct AddAddressFrame {
    vector<NetworkAddress> addresses;
};

struct ConnectionCloseFrame {
    uint32_t error_code = 0;
    string reason;
};
```

---

### 3.3 Deep Dive: The 18-Byte Public Header & Fast Header Peeking

The 18-byte public header is packed with `#pragma pack(push, 1)`:
* **Byte 0:** `flags` (reserved, `0x00`).
* **Bytes 1–8:** `connection_id` (64-bit integer, big-endian).
* **Byte 9:** `path_id` (8-bit integer, selects path sequence space).
* **Bytes 10–17:** `packet_number` (64-bit integer, big-endian).

#### Fast Header Peeking:
When a packet arrives at `Server::handle_datagram`:
```cpp
uint64_t connection_id = 0;
uint8_t path_id = 0;
if (!Packet::peek_header(data, length, connection_id, path_id)) return;
```
The server reads `connection_id` and `path_id` directly from bytes 1–10. It can immediately route the datagram to the correct `Connection` object in a hash map without paying the CPU cost of deserializing frames.

---

### 3.4 Deep Dive: Packet Sequence Numbers vs. Stream Byte Offsets

```
Packet Sequence Space (Per Path):
Path 0:  [Packet #1]  [Packet #2]  [Packet #3]  [Packet #4] ... (Strictly monotonic)
Path 1:  [Packet #1]  [Packet #2]  [Packet #3] ...              (Strictly monotonic)

Stream Offset Space (Connection-Wide):
Stream 1: [Byte 0 ... 1315]  [Byte 1316 ... 2631]  [Byte 2632 ... 3947] ...
```

* **Packet Number:** Uniquely identifies a physical datagram on a specific path. It is never reused.
* **Stream Offset:** Uniquely identifies the byte position in the application payload.
* **Retransmission:** If Packet #3 on Path 0 carrying stream bytes `[2632..3947]` is lost, those stream bytes are repacked into **Packet #12 on Path 1**. When the receiver ACKs Packet #12, the sender gets an unambiguous RTT measurement for Path 1, and the receiver slots the bytes into offset 2632.

---

### 3.5 Deep Dive: Zero-Handshake Connection Establishment (0-RTT)

MP-QUIC Mini eliminates the 3-way handshake delay:
1. **Client:** Generates a random 64-bit `connection_id`. Binds local sockets. Creates Path 0.
2. **First Packet:** Client writes data (`"GET file.bin"`). This data is packed into **Packet #1 on Path 0**.
3. **Server Acceptance:** Server receives Packet #1. It checks `connections.find(connection_id)`. If unknown and `path_id == 0`, it instantiates a new `Connection` and `PathManager`, creates Path 0, queues an `ADD_ADDRESS` frame, and immediately delivers the stream data to the server application.

---

### 3.6 Deep Dive: Dynamic Path Discovery & The Odd/Even ID Rule

#### The Collision Problem:
If both client and server could choose arbitrary Path IDs, both might pick ID `1` simultaneously, causing state corruption.

#### The Protocol Convention:
* **Server-Initiated:** Even numbers ($0, 2, 4 \dots$). The initial path is always **Path 0**.
* **Client-Initiated:** Odd numbers ($1, 3, 5 \dots$).

#### Server Validation:
In [`src/path/path_manager.cpp`](file:///home/adinesh-reddy/MP_QUIC/src/path/path_manager.cpp#L122-L125):
```cpp
if (path_id != 0 && path_id % 2 == 0) {
    return nullptr; // Reject invalid even path ID!
}
```

---

### 3.7 Deep Dive: The 7 Frame Specifications & Wire Encodings

1. **`FRAME_STREAM` (`0x01`):**
   * Format: `type (1B) | stream_id (4B) | offset (8B) | fin (1B) | length (2B) | payload (LB)`
   * Rejects `fin > 1` or `length == 0 && fin == 0`.
2. **`FRAME_ACK` (`0x02`):**
   * Format: `type (1B) | path_id (1B) | largest_acked (8B) | ack_delay_us (4B) | num_ranges (2B) | varint ranges...`
   * Rejects `num_ranges == 0 || num_ranges > 256` or invalid range spans.
3. **`FRAME_WINDOW_UPDATE` (`0x04`):**
   * Format: `type (1B) | stream_id (4B) | byte_offset (8B)`
   * Advances receive window limit.
4. **`FRAME_PING` (`0x07`):**
   * Format: `type (1B)`. Single byte liveness probe.
5. **`FRAME_ADD_ADDRESS` (`0x10`):**
   * Format: `type (1B) | count (1B) | [IPv4 (4B) + Port (2B)] * count`
   * Advertises listening network endpoints.
6. **`FRAME_PATHS` (`0x12`):**
   * Format: `type (1B) | count (1B) | [path_id (1B) + rtt_us (4B)] * count`
   * `rtt_us = 0xFFFFFFFF` signals a potentially failed path.
7. **`FRAME_CONNECTION_CLOSE` (`0x03`):**
   * Format: `type (1B) | error_code (4B) | reason_len (2B) | reason_str`
   * Error codes: `0 = CLOSE_NO_ERROR`, `1 = CLOSE_PROTOCOL_ERROR`, `2 = CLOSE_IDLE_TIMEOUT`.

---

### 3.8 Deep Dive: Variable-Length Integer (Varint) Compression

Implemented in [`src/packet/frame.cpp` (lines 78–126)](file:///home/adinesh-reddy/MP_QUIC/src/packet/frame.cpp#L78-L126):

```cpp
static bool get_varint(const uint8_t *buffer, size_t length, size_t &pos, uint64_t &value)
{
    if (length - pos < 1) return false;

    // Top 2 bits determine byte length: 1, 2, 4, or 8 bytes
    size_t size = static_cast<size_t>(1) << (buffer[pos] >> 6);
    if (length - pos < size) return false;

    value = buffer[pos] & 0x3F;
    for (size_t i = 1; i < size; i++) {
        value = (value << 8) | buffer[pos + i];
    }
    pos += size;
    return true;
}
```

```
Value Range           Encoded Bytes    Top 2 Bits (Prefix)
0 to 63               1 Byte           00 (0x00)
64 to 16,383          2 Bytes          01 (0x40)
16,384 to 1,073,741,823 4 Bytes        10 (0x80)
1,073,741,824 to 2^62-1 8 Bytes        11 (0xC0)
```

---

### 3.9 Deep Dive: SACK Ranges & Interval Merging

`AckFrame` carries `ranges` sorted in descending order:
* `ranges[0].last == largest_acked`.
* First range encoded as: `first_range_len = last - first`.
* Subsequent ranges encoded as:
  - `gap = previous.first - current.last - 1`
  - `range_len = current.last - current.first`

#### Automatic Range Merging:
Inside `ReceivedPacketHandler::remember()`:
* If received packet $P$ is adjacent to both a range below and above:
  ```cpp
  if (joins_below && joins_above) {
      below->second = above->second;
      received_ranges.erase(above);
  }
  ```
* If ranges exceed `MAX_ACK_RANGES = 256`, the oldest range is dropped.

---

### 3.10 Deep Dive: The 6 Receiver ACK Decision Rules

Inside `ReceivedPacketHandler::decide_ack()`:
1. **Rule 1 (First Packet):** `!sent_an_ack` $\rightarrow$ `ack_now = true`.
2. **Rule 2 (Gap Opened):** `ranges.rbegin()->first > highest_in_last_ack` $\rightarrow$ `ack_now = true`.
3. **Rule 3 (Gap Filled):** `packet_number < highest_in_last_ack` $\rightarrow$ `ack_now = true`.
4. **Rule 4 (2-Packet Threshold):** `unacked_data_packets >= 2` $\rightarrow$ `ack_now = true`.
5. **Rule 5 (Delayed ACK Timer):** Single data packet $\rightarrow$ arms `delayed_ack_deadline_us = now + 25000` (25 ms).
6. **Rule 6 (Pure ACK):** Packet contains only ACK frame $\rightarrow$ `needs_ack = false` $\rightarrow$ no ACK generated.

---

### 3.11 Deep Dive: Infinite ACK Loop Prevention

If Host A ACKs Host B, and Host B ACKs Host A's ACK, an infinite packet storm results.
* In MP-QUIC, `Frame::is_retransmittable()` returns `false` for `FRAME_ACK`.
* When a pure ACK packet arrives, `packet.is_retransmittable()` is `false`.
* `ReceivedPacketHandler::decide_ack()` is called with `needs_ack = false`.
* No ACK is ever scheduled in response to an ACK!

---

### 3.12 Deep Dive: Retransmittability & Piggybacked Frames

When an application sends data while also having an ACK ready:
* The packet contains **`[FRAME_ACK + FRAME_STREAM]`**.
* `packet.is_retransmittable()` evaluates to **`true`** because of `FRAME_STREAM`.
* The receiver will acknowledge this packet.
* In `SentPacketHandler::sent_packet()`:
  ```cpp
  for (const auto &frame : frames) {
      if (frame.is_retransmittable()) {
          entry.frames.push_back(frame); // Saves STREAM, DROPS ACK!
      }
  }
  ```
* If lost, only the `STREAM` frame is retransmitted. The old ACK is discarded, and a fresh up-to-date ACK is attached to the new packet.

---

### 3.13 Deep Dive: Cross-Path Acknowledgments

`AckFrame` explicitly identifies `path_id`.
In `Connection::dispatch_frame()`:
```cpp
Path *acked_path = get_path(frame.ack.path_id);
if (acked_path != nullptr) {
    uint64_t with_pn = (frame.ack.path_id == packet.get_path_id()) ? packet.get_packet_number() : 0;
    acked_path->get_sent_packet_handler()->received_ack(frame.ack, with_pn, now_us);
}
```
If Path 0 (Wi-Fi) is dropping uplink packets, Path 1 (Cellular) can carry the ACK for Path 0, allowing the connection to survive asymmetric link failures.

---

### 3.14 Deep Dive: Dual-Level Flow Control & `WINDOW_UPDATE` Frames

Flow control protects receiver memory:
* **Stream Window (`stream_id > 0`):** Initial $32\text{ KiB}$, Max $16\text{ MiB}$.
* **Connection Window (`stream_id == 0`):** Initial $48\text{ KiB}$, Max $24\text{ MiB}$ ($1.5\times$ stream window).

#### Triggering Updates:
When `advertised_limit - read_bytes < window_step / 2`:
* If updates occur within $2 \times \text{RTT}$, `autotune_step()` doubles `window_step`.
* Emits `FRAME_WINDOW_UPDATE(stream_id, new_offset)`.
* Queued onto **all active paths** so loss on one path cannot stall the sender.

---

### 3.15 Unit Test Verification: `test_packet.cpp`

File: [`tests/unit/test_packet.cpp`](file:///home/adinesh-reddy/MP_QUIC/tests/unit/test_packet.cpp)
This test suite runs 11 rigorous verification suites:
1. `test_header_size_and_layout`: Asserts `sizeof(PacketHeader) == 18` and verifies big-endian byte offsets.
2. `test_stream_frame_round_trip`: Verifies stream serialization, offset tracking, and empty-payload FIN validation.
3. `test_ack_frame_round_trip`: Verifies multi-range SACK packing, gap calculation, and round-trip decoding.
4. `test_ack_frame_256_ranges`: Tests packing exactly 256 disjoint ranges into varints and verifies dropping range #257.
5. `test_paths_frame_round_trip`: Verifies smoothed RTT reporting and `0xFFFFFFFF` failure sentinels.
6. `test_add_address_frame_round_trip`: Tests IPv4 binary serialization and rejection of invalid IP strings.
7. `test_connection_close_frame_round_trip`: Tests error codes and truncating strings $> 200$ bytes.
8. `test_multiple_frames_in_one_packet`: Tests packing 5 diverse frames into a single packet.
9. `test_flags_manipulation`: Tests bitwise flag manipulation.
10. `test_packet_size_limit`: Asserts that packets up to 1350 bytes serialize cleanly, while 1351 bytes throws `std::length_error`.
11. `test_malformed_packets`: Verifies rejection of truncated headers, bad varints, and invalid ranges.
