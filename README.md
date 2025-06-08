# myprotocol

A simple client-server sample using the Microsoft MsQuic API. 

---

## Table of Contents

* [Overview](#overview)
* [Dependencies](#dependencies)
* [Certificate Generation](#certificate-generation)
* [Building](#building)
* [Running](#running)
* [Project Structure](#project-structure)
* [License](#license)

---

## Overview

This repository contains:

* **`gameprotocol.cpp`**: Implementation of a minimal client and server using MsQuic.
* **`protocol.hpp`**: Shared protocol definitions (PDU, message types).
* **`CMakeLists.txt`**: CMake build script.

The client:

1. Opens a QUIC connection to the server.
2. Creates a bidirectional stream.
3. Sends messages 1 through 10 (state updates).
4. Closes its send direction and awaits the server’s FIN.

The server:

1. Listens on UDP port 4567.
2. Accepts QUIC connections and streams.
3. Echoes each received counter back to the client.
4. Shuts down its send direction on receiving message 10.

---

## Dependencies

* **CMake** ≥ 3.18
* **A C++17 toolchain** (GCC/Clang)
* **MsQuic** library installed and discoverable via CMake `find_package(msquic CONFIG REQUIRED)`.
* On Linux: `pthread` and `dl` (loaded automatically by the CMakeLists).
* **OpenSSL** (for certificate generation) or use the instructions below.

---

## Certificate Generation

### Self-signed certificate (Linux/OpenSSL)

```bash
# Generate a 2048-bit RSA key and self-signed cert
openssl req -nodes -new -x509 -keyout server.key -out server.crt \
  -subj "/CN=localhost" -days 365
```

Copy `server.crt` and `server.key` to a known location (e.g. `/tmp/`).

---

## Building

```bash
# From the root of the repository:
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
```

This produces the executable `myprotocol` in `build/` (or `out/`, depending on your CMake generator).

---

## Running

### Server

Open a terminal and run:

```bash
./myprotocol \
  -server \
  -cert_file:/tmp/server.crt \
  -key_file:/tmp/server.key
```

* **`-server`**: run in server mode
* **`-cert_file`**: path to TLS certificate
* **`-key_file`**: path to private key

### Client

In a second terminal, run:

```bash
./myprotocol \
  -client \
  -unsecure \
  -target:127.0.0.1
```

* **`-client`**: run in client mode
* **`-unsecure`**: skip certificate validation (for testing)
* **`-target`**: server address (IPv4 or hostname)

**Expected output**:

```text
[conn][0x...] Connected
[cli][0x...] Starting stream...
[cli][0x...] Sent 1
[strm][0x...] Data sent
[conn][0x...] Resumption ticket received (... bytes)
[strm][0x...] All done
[conn][0x...] All done
```

---

## Project Structure

```text
.
├── CMakeLists.txt        # Build script
├── protocol.hpp          # Shared PDU/message definitions
└── gameprotocol.cpp      # Client & server implementation
```

---

## License

Licensed under the MIT License. See [LICENSE](LICENSE) for details.
