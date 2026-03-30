# MazeWar: A Multi-Threaded Network Game Server in C

## Overview

**MazeWar** is a real-time multiplayer network game server built from scratch in C using POSIX sockets and threading. The server supports multiple concurrent players navigating a 2D maze, firing lasers, rotating, moving, and chatting in real time. Each connected client is serviced by its own thread, and the server architecture supports message passing, player coordination, and dynamic view updates.

The project emphasizes low-level network programming, thread-safe shared state management, and concurrent system design. All components—including the networking protocol, client registry, maze logic, and player state—were implemented manually using POSIX-compliant system calls, synchronization primitives, and custom protocol definitions.

---

## Features

- 🧠 **Multi-threaded Server Core:** Each client connection spawns a dedicated service thread using POSIX threads.
- 🕹️ **Live Avatar Management:** Players control avatars in a shared 2D maze. Movements and actions are broadcast to all clients.
- 🔫 **Laser Combat Mechanics:** Players can fire lasers in the direction of gaze to eliminate opponents temporarily.
- 🔄 **Real-time View Updates:** Each client receives incremental or full screen refreshes based on their avatar’s state and events.
- 💬 **In-game Chat:** Players can send messages visible to all currently connected users.
- 📶 **Custom Protocol Stack:** All communication follows a self-defined packet-based protocol layered over TCP.
- 🔐 **Thread Safety:** Mutexes and semaphores ensure consistent shared state and clean termination.
- 🚦 **Client Registry:** Tracks active connections, supports graceful shutdown, and ensures cleanup of orphaned threads.
- 💥 **Signal-Driven Interaction:** Signals like `SIGUSR1` and `SIGHUP` trigger in-game actions and server control.

---

## Modules

- `main.c`: Initializes the server, handles command-line options, sets up signals, and starts the accept loop.
- `protocol.c`: Implements sending and receiving of structured packets over sockets.
- `client_registry.c`: Tracks active clients using semaphores and handles cleanup on shutdown.
- `maze.c`: Manages the internal maze layout, avatar positions, and collision logic.
- `player.c`: Manages individual player state, handles view rendering, laser hits, scoring, and synchronization.

---

## Gameplay Summary

- Players control avatars using arrow keys and the Escape key (fire).
- Movement is restricted by maze walls and other avatars.
- Hitting another player with a laser temporarily removes them from the maze and increments your score.
- Chat messages and scoreboards are updated in real-time.
- When avatars come into view, visual updates are immediately sent to the affected clients.
