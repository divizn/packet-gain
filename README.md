# packet-gain

A multipath UDP relay in C++20 for low-latency traffic such as online games.

The client sends every packet down several paths at once and the exit node
forwards the first copy to arrive, so a packet is only lost if every path
drops it. With independent loss `p` on each of `k` paths, loss falls from `p`
to about `p^k`. This is how game network optimisers like ExitLag and NoPing
handle packet loss; their other half, a relay network chosen for better
routes, is not built yet.

```
app ──> client ──┬── path 0 ──┐
                 ├── path 1 ──┼──> exit ──> target
                 └── path 2 ──┘
```

Replies come back the same way: the exit sends each one over every path the
client has used and the client delivers the first copy.

```
$ pgain genkey > key
$ pgain exit key '[::]:7100' game.example.com:27015
$ pgain client key 127.0.0.1:7000 relay-a:7100 relay-b:7100
```

The game (or anything else speaking UDP) then talks to `127.0.0.1:7000`.
Each exit forwards to one fixed target, so it cannot be used as an open
proxy.

## Measured

On an i7-8650U laptop, Linux 6.8, Release build. Loss and delay are
injected in userspace by `bench/impair.cpp`, a UDP proxy placed in front of
each path (see [Testing without root](#testing-without-root)).

`pgain-bench loss --seconds 20 --rate 128`: 10% loss in each direction on
every path, 10 ms delay, 2 ms jitter, 128 packets/s, 64-byte payloads.

| setup | round-trip loss | theory | p50 RTT | p99 RTT |
|---|---|---|---|---|
| direct | 19.06% | 19.00% | 23.4 ms | 27.2 ms |
| 1 path | 18.32% | 19.00% | 23.6 ms | 27.3 ms |
| 2 paths | 1.52% | 1.99% | 23.3 ms | 27.1 ms |
| 3 paths | 0.20% | 0.20% | 22.6 ms | 28.3 ms |

Theory is `1 - (1 - p^k)^2`, loss on the way out or the way back.

`pgain-bench throughput --seconds 5 --paths 1`: one sender sending 64-byte
packets as fast as it can.

| | delivered packets per CPU-second | user | kernel |
|---|---|---|---|
| client | 63,271 | 1.01 s | 3.43 s (77%) |
| exit | 78,555 | 0.73 s | 2.84 s (79%) |

Most of the time is the kernel's loopback UDP stack, split per thread with
`getrusage(RUSAGE_THREAD)`. The exit's own code costs about 2.6 µs per
packet, of which HMAC-SHA256 is 1.4 µs (timed separately over a million
64-byte packets). Only 43% of offered packets were delivered: the sender
outruns the receive buffers on purpose, to find the ceiling.

With no injected loss or delay, the tunnel adds 0.2 to 0.5 ms to the p50
round trip on loopback. p99 is not worth quoting from these runs: it was
5 ms on the direct path too, which is scheduler noise from seven threads on a
laptop.

## How it works

- **Wire format:** a 16-byte header, then the payload, then a 16-byte tag.

  ```
  0        1        2        3        4                8               16
  | version | type   | path   | 0      | session (u32) | seq (u64)      |
  ```

  Integers are big-endian. `session` is chosen at random by the client.
  `seq` counts packets, so every copy of a packet carries the same one; the
  path byte differs.
- **Dedup:** a 1024-entry sliding bitmap per direction, the same shape as the
  anti-replay window in IPsec and WireGuard. Out-of-order packets inside the
  window are accepted once; older ones are dropped. There is no reorder
  buffer, since holding a packet back to reorder it adds the latency this
  exists to remove.
- **Authentication:** HMAC-SHA256 truncated to 128 bits over header and
  payload, with a pre-shared key. The exit checks it before creating a
  session, so forged packets cannot create sessions or reach the target.
  Payloads are not encrypted; games encrypt their own traffic, and doing it
  here safely needs a handshake (see [Not supported](#not-supported)).
- **Return paths:** the exit learns each path's return address from the
  packets arriving on it, but only from a packet newer than any seen on that
  path. Without that, anyone who captured a packet could replay it from
  their own address and receive the session's replies.
- **Batching:** `recvmmsg`/`sendmmsg` on `epoll`, 32 datagrams per call.
  Sends use scatter-gather (header, payload, tag as separate `iovec`s), so a
  payload is never copied, however many paths it goes down. The exit's
  sends to the target are batched per session and flushed when the session
  changes within a batch.
- **Addresses:** IPv4 and IPv6. Listeners on `[::]` are dual-stack, and IPv6
  literals take brackets (`[::1]:7000`).

## Testing

- **Dedup harness:** 200 seeded runs of 5000 packets over 1 to 3 simulated
  paths with loss, duplication and reordering, half of them reordering
  further than the window. Every decision is compared with a reference model
  built on `std::set`.
- **Loopback tunnel tests:** real sockets, covering 1, 2 and 4 paths, IPv4,
  IPv6 and dual-stack, two clients interleaved in one batch, a wrong key,
  every byte of a packet flipped in turn, a replay from a new address, and a
  fake exit sending a forged reply.
- **Loss test:** traffic through the impairer at 20% loss and 5%
  duplication; one-way loss has to land within four standard deviations of
  `p^k` for 1, 2 and 3 paths (measured 20.6%, 3.9%, 0.77%).

Each test was checked by breaking the code it covers and watching it fail:
not clearing reused window slots, an off-by-one at the window edge, either
side skipping dedup or MAC checks, a payload offset off by one, paths
numbering packets separately, learning return addresses from replays,
dropping the batch flush, `IPV6_V6ONLY`, wrong address lengths, and a client
sending only one copy. The harness missed one of these (a jump past the
whole window not clearing it); a unit test catches it.

### Testing without root

`tc netem` needs root, and so does running it in a network namespace on
Ubuntu 24.04, where `kernel.apparmor_restrict_unprivileged_userns` blocks
unprivileged user namespaces. The impairer does the same job in userspace:
seeded loss, duplication, delay and jitter, in both directions. That keeps
the tests deterministic and runnable in CI.

### Receive buffer overflow

An early version of the tunnel test lost a third of its packets with four
paths. `UdpRcvbufErrors` in `nstat` went up by 484 during the run: the test
sent 200 packets on each path in one burst, 800 datagrams into the exit's
208 KB default receive buffer. Game traffic runs at 60 to 128 packets/s, so
the test now paces itself; the throughput benchmark is where overflow is
measured.

## Build and test

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --timeout 60
```

Needs OpenSSL 3. `-DPGAIN_SANITIZE=ON` builds with ASan and UBSan; CI runs
both builds on Ubuntu 24.04. For benchmarks, use a Release build:

```
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release
./build-release/pgain-bench loss
./build-release/pgain-bench throughput
```

## Not supported

- Relay mesh with latency probing and route selection
- Capturing a game's traffic by process (WinDivert on Windows, a TUN device
  on Linux); the app has to be pointed at the client
- Encryption, and replay protection across sessions: once a session expires
  after 60 s idle, replaying its old packets opens a new one and forwards
  each of them once. Both need a handshake.
- More than one target per exit
- Benchmarks over real links between machines
