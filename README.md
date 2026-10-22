# packet-gain

A multipath UDP relay in C++20 for low-latency traffic such as online games.

The client knows several paths to an exit node, each either direct or
through a chain of relays. It probes every path for latency, jitter and
loss, sends each packet down the best `k` of them, and the exit forwards the
first copy to arrive. A packet is only lost if every path it took drops it,
so with independent loss `p` on each path, loss falls from `p` to about
`p^k`. When the best path goes dark, the client moves to the next best one
in about 165 ms. This is how game network optimisers like ExitLag and NoPing
deal with bad routes and packet loss.

```
app ──> client ──┬── relay ── relay ──┐
                 ├── relay ───────────┼──> exit ──> target
                 └────────────────────┘
```

Replies come back the same way: the exit sends each one over every path the
client has used in the last second, and the client delivers the first copy.

```
$ pgain genkey > key
$ pgain exit key '[::]:7100' game.example.com:27015
$ pgain relay key '[::]:7200' exit.example.com:7100
$ pgain client key 127.0.0.1:7000 --send 2 relay-a:7200 relay-b:7200 exit.example.com:7100
```

The game (or anything else speaking UDP) then talks to `127.0.0.1:7000`.
Each exit forwards to one fixed target and each relay to one fixed next hop,
so neither can be used as an open proxy.

## Measured

On an i7-8650U laptop, Linux 6.8, Release build. Loss and delay are
injected in userspace by `bench/impair.cpp`, a UDP proxy placed in front of
each path (see [Testing without root](#testing-without-root)).

### Loss

`pgain-bench loss --seconds 20 --rate 128`: 10% loss in each direction on
every path, 10 ms delay, 2 ms jitter, 128 packets/s, 64-byte payloads,
sending on every path.

| setup | round-trip loss | theory | p50 RTT | p99 RTT |
|---|---|---|---|---|
| direct | 19.06% | 19.00% | 23.4 ms | 25.7 ms |
| 1 path | 18.40% | 19.00% | 23.6 ms | 25.9 ms |
| 2 paths | 2.38% | 1.99% | 23.2 ms | 28.4 ms |
| 3 paths | 0.27% | 0.20% | 22.6 ms | 27.2 ms |

Theory is `1 - (1 - p^k)^2`, loss on the way out or the way back.

### Failover

`pgain-bench failover --delay-ms 5 --rate 1000 --seconds 4 --runs 5`: three
paths at 5, 10 and 15 ms one way, sending on one. Halfway through, path 0
starts dropping everything.

| run | packets lost | longest outage | path after |
|---|---|---|---|
| 0 | 166 | 166 ms | 1 |
| 1 | 166 | 166 ms | 1 |
| 2 | 165 | 165 ms | 1 |
| 3 | 166 | 166 ms | 1 |
| 4 | 167 | 167 ms | 1 |

A path is declared dead after three missed probe intervals (150 ms); the
rest is the round trip on the new path.

### Relay hops

`pgain-bench relay --rate 500 --seconds 10`: one path through 0 to 3
relays, no injected delay, two runs.

| relays | p50 RTT, run 1 | p50 RTT, run 2 |
|---|---|---|
| 0 | 286 µs | 246 µs |
| 1 | 371 µs | 368 µs |
| 2 | 438 µs | 450 µs |
| 3 | 455 µs | 463 µs |

Each relay adds roughly 10 to 120 µs to the round trip on loopback, which
crosses it twice. p99 is not worth quoting from these runs: it was 1.8 to
2.4 ms with no relays at all, which is scheduler noise from several threads
on a laptop.

### Throughput

`pgain-bench throughput --seconds 5 --paths 1`: one sender sending 64-byte
packets as fast as it can. Four runs:

| | delivered packets per CPU-second | time in the kernel |
|---|---|---|
| client | 82,198 to 97,599 | 76 to 78% |
| exit | 88,633 to 100,567 | 75 to 80% |

Most of the time is the kernel's loopback UDP stack, split per thread with
`getrusage(RUSAGE_THREAD)`. The exit's own code costs about 2 to 2.6 µs per
packet, of which HMAC-SHA256 is 1.4 µs (timed separately over a million
64-byte packets). About half of the offered packets were delivered: the
sender outruns the receive buffers on purpose, to find the ceiling.

## How it works

- **Wire format:** a 16-byte header, then the payload, then a 16-byte tag.

  ```
  0        1        2        3        4                8               16
  | version | type   | path   | 0      | session (u32) | seq (u64)      |
  ```

  Integers are big-endian. `type` is data, probe or probe reply. `session`
  is chosen at random by the client. `seq` counts packets, so every copy of
  a packet carries the same one; the path byte differs.
- **Dedup:** a 1024-entry sliding bitmap per direction, the same shape as the
  anti-replay window in IPsec and WireGuard. Out-of-order packets inside the
  window are accepted once; older ones are dropped. There is no reorder
  buffer, since holding a packet back to reorder it adds the latency this
  exists to remove.
- **Probing:** every 50 ms the client sends a probe carrying a timestamp
  down each path, and the exit sends it straight back. Smoothed RTT and RTT
  variance follow RFC 6298; loss is the share of the last 54 probes with no
  reply (the 10 newest are left out as still in flight). A path with no
  reply for three intervals plus four RTT variances is dead.
- **Path selection:** live paths are ranked by `srtt + 2 * rttvar +
  200 ms * loss`, and data goes down the best `k`. The paths in use get a
  10% plus 1 ms head start, so near-equal paths don't flap. If fewer than
  `k` paths are alive, dead ones fill the gap; if none are, every path is
  used.
- **Relays:** forward packets to a fixed next hop, with one upstream socket
  per source so replies find their way back. Relays check the tag before
  opening a flow. Probes pass through like any other packet, so a path's RTT
  covers every hop.
- **Authentication:** HMAC-SHA256 truncated to 128 bits over header and
  payload, with a pre-shared key. The exit checks it before creating a
  session, so forged packets cannot create sessions or reach the target.
  Payloads are not encrypted; games encrypt their own traffic, and doing it
  here safely needs a handshake (see [Not supported](#not-supported)).
- **Return paths:** the exit learns each path's return address from the
  packets arriving on it, but only from a packet newer than any seen on that
  path. Without that, anyone who captured a packet could replay it from
  their own address and receive the session's replies. Replies go only down
  paths heard from in the last second (or the most recent one, if none
  have been), so they follow the client's choice of paths.
- **Batching:** `recvmmsg`/`sendmmsg` on `epoll`, 32 datagrams per call.
  Sends use scatter-gather (header, payload, tag as separate `iovec`s), so a
  payload is never copied, however many paths it goes down. Sends to the
  target and through relays are batched per flow and flushed when the flow
  changes within a batch.
- **Addresses:** IPv4 and IPv6. Listeners on `[::]` are dual-stack, and IPv6
  literals take brackets (`[::1]:7000`).

## Testing

- **Dedup harness:** 200 seeded runs of 5000 packets over 1 to 3 simulated
  paths with loss, duplication and reordering, half of them reordering
  further than the window. Every decision is compared with a reference model
  built on `std::set`.
- **Path selection:** simulated probe rounds for picking the lowest RTT,
  tracking a latency change, a path going silent, a lossy path losing to a
  clean one, hysteresis, and dead paths filling in.
- **Loopback tests:** real sockets, covering 1, 2 and 4 paths, IPv4, IPv6
  and dual-stack, two clients interleaved in one batch, a chain of two
  relays, a wrong key, every byte of a packet flipped in turn, a replay from
  a new address, a fake exit sending a forged reply, unauthenticated packets
  sent to a relay, and the exit dropping a path the client stopped using.
- **Loss and failover tests:** traffic through the impairer. At 20% loss and
  5% duplication, one-way loss has to land within four standard deviations
  of `p^k` for 1, 2 and 3 paths. With the best of three paths cut, the
  client has to move to the next best and stop losing packets.

Each test was checked by breaking the code it covers and watching it fail:
not clearing reused window slots, an off-by-one at the window edge, either
side skipping dedup or MAC checks, a payload offset off by one, paths
numbering packets separately, learning return addresses from replays,
dropping a batch flush, `IPV6_V6ONLY`, wrong address lengths, a client
sending only one copy, the client ignoring probe replies or its own path
choice, the exit ignoring probes or replying down abandoned paths, no
hysteresis, liveness that ignores silence, and a relay that skips the MAC
check, never replies or drops its last batch. The dedup harness missed one
of these (a jump past the whole window not clearing it); a unit test
catches it.

### Testing without root

`tc netem` needs root, and so does running it in a network namespace on
Ubuntu 24.04, where `kernel.apparmor_restrict_unprivileged_userns` blocks
unprivileged user namespaces. The impairer does the same job in userspace:
seeded loss, duplication, delay and jitter, in both directions, and loss
that can be changed while it runs. That keeps the tests deterministic and
runnable in CI.

### Healthy paths declared dead

The first version of path selection sent only on live paths. With three
paths at 20% loss in each direction, the loss test measured 8.8% one-way
loss against an expected 0.8%, worse than with two paths. A probe round trip
fails 36% of the time at that loss, so three misses in a row happen often
enough that healthy paths were declared dead and stopped carrying copies.
Selection now always fills `k` paths, live ones first, so a path wrongly
declared dead only loses its place in the ranking. The same test then
measured 0.8%.

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
./build-release/pgain-bench failover
./build-release/pgain-bench relay
./build-release/pgain-bench throughput
```

## Not supported

- Routing between relays: paths are fixed chains configured at the client,
  which picks between them. Relays don't discover each other or compute
  routes.
- Capturing a game's traffic by process (WinDivert on Windows, a TUN device
  on Linux); the app has to be pointed at the client
- Encryption, and replay protection across sessions: once a session expires
  after 60 s idle, replaying its old packets opens a new one and forwards
  each of them once. Both need a handshake.
- More than one target per exit
- Benchmarks over real links between machines
