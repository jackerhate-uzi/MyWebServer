# MyWebServer

A high-performance C++ Web Server based on Linux Epoll.

## Milestone 1: Echo Server

Currently implements a basic TCP echo server using Epoll (Level Trigger).



## How to Build

```bash
mkdir build && cd build
cmake ..
make
```

## How to Run

```bash
./server
```

The server listens on port 9006.

## How to Test

You can test it using `nc`or`telnet` from the same machine or any device in the LAN.

```bash
nc <server_ip> 9906
#Type anything and press Enter to see the echo.
```



