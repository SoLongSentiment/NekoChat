# NekoChat - P2P Chat with Voice and Screen Sharing

NekoChat is a peer-to-peer chat application for Windows that enables direct voice calls, screen sharing, and text messaging between users without requiring a central media server. It uses ICE (RFC 5245) with STUN for NAT traversal, GDI+ for screen capture, Windows Wave API for low-latency audio, and a custom OpenGL graphical interface.

## Features

*   **Direct P2P connections** – No media relay (STUN only for external address discovery).
*   **Text chat** – Simple XOR-obfuscated messages sent directly between peers.
*   **Voice communication** – Real-time 16-bit 44.1 kHz mono audio with:
    *   Automatic mixing of multiple simultaneous talkers.
    *   Basic acoustic echo cancellation (correlation-based).
    *   Per-peer volume control (0–200%).
*   **Screen sharing** – Capture the whole desktop, scale, encode to JPEG, and stream in chunks.
    *   Adjustable FPS (1–60), JPEG quality (20–95), and capture scale (20–100%).
    *   Separate viewer window to watch remote screens.
*   **ICE connectivity checks** – Host and server-reflexive candidates (STUN) are gathered; pairs are tested and the best working one is used.
*   **Signalling server** – Lightweight TCP server for peer discovery, exchange of ICE candidates, and relayed messages.
*   **OpenGL GUI** – Pure OpenGL interface with tabs for connection logs, voice mixer, and screen sharing controls. No extra UI libraries.

## Architecture Overview

NekoChat consists of several key components:

*   **IceAgent** – Implements ICE connectivity checks (RFC 5245). Manages local/remote candidates, sends STUN Binding Requests, tracks timeouts/retries, and selects the active candidate pair.
*   **CandidateGatherer** – Enumerates local IPv4 addresses (host candidates) and queries a STUN server to obtain a server-reflexive address.
*   **StunClient** – Helper for building/parsing STUN messages.
*   **AudioEngine** – Captures microphone input (waveIn), mixes received audio frames from different peers, plays mixed output (waveOut), and applies basic echo suppression.
*   **ScreenShareEngine** – Captures the desktop using BitBlt, resizes, compresses to JPEG via GDI+, and splits the image into chunks for UDP transmission.
*   **SignallingClient / SignallingServer** – TCP-based signalling for registration, user listing, sending ICE candidates, and text relay.
*   **P2PClient** – The core class that ties everything together. Manages one IceAgent per peer, routes incoming packets (text, audio, screen), and handles reconnections.
*   **GUI (P2PChatOpenGL.cpp)** – A custom OpenGL window with input fields, buttons, tabs, and a separate viewer window for streams.

## Dependencies

*   Windows SDK 
*   CMake 3.16 or newer
*   A C++17 compiler 
*   OpenGL 
*   GDI+ 

## Usage

### 1. Start the signalling server NekoChat_server.exe
Run the server on a machine with a public IP (or local network for testing). It listens on TCP port 27015 by default.

### 2. Start clients NekoChat_client.exe OR NekoChat_cmd_client.exe
Each client must know the server's IP address. Before building, replace the placeholder `"SERVER IP!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"` in `P2PChat.cpp` and `P2PChatOpenGL.cpp` with the actual server IP.

### Commands:


| Command | Description |
| :--- | :--- |
| `register <id>` | Register your ID with the signalling server. |
| `offer <peer_id>` | Send an offer to another user to start P2P connection. |
| `msg <peer_id> <text>` | Send a direct text message over the P2P link. |
| `smsg <peer_id> <text>` | Send a message relayed through the signalling server. |
| `list` | Request the list of online users from the server. |
| `voice on <peer_id>` | Start sending microphone audio to a specific peer. |
| `brdvoice on` | Broadcast your voice to all connected peers. |
| `voice off` | Stop sending audio. |
| `volume <peer_id> <0-200>` | Adjust playback volume for a peer (100 = normal). |
| `stream on <peer_id\|*>` | Start screen sharing to a single peer or broadcast (*). |
| `stream off` | Stop screen sharing. |
| `stream fps <n>` | Set screen capture FPS (1–60). |
| `stream quality <n>` | Set JPEG quality (20–95). |
| `stream scale <n>` | Set capture scale in percent (20–100). |
| `reconnect <peer_id>` | Force a reconnect attempt. |

### GUI client
Run `NekoChat_client.exe`. The OpenGL window will appear. Enter your ID in the Connect screen and press **Connect**. Once registered, you can use the tabs and buttons to perform most actions without typing commands (though the command input at the bottom still accepts them).

* **Connection tab** – Shows important connection events.
* **Logs tab** – Full debug output.
* **Voice tab** – Adjust echo suppression parameters and per-peer volume.
* **Screen tab** – Start/stop screen sharing, choose target, adjust streaming parameters, and open a viewer window to watch remote streams.

## Configuration
The only hard-coded configuration is the signalling server IP. You must replace the placeholder in the source files before compiling. If you want to change the server port, edit `DEFAULT_PORT` in `SignallingServer.hpp` and the port number in `P2PChat.cpp` and gui version (both occurrences of 27015).
