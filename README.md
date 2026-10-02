# mcchat

A small terminal chat client written in C that uses IPv6 multicast UDP
packets to talk to peers — no central server required. Every instance
joins the same multicast group on a chosen interface; messages you type
are sent to the group, and messages from other peers are printed as
they arrive.

## Build

```sh
make
```

Requires a C11 compiler and pthreads (available by default on Linux and
macOS).

## Run

You must pick a network interface for multicast (e.g. `en0` on macOS,
`eth0`/`wlan0` on Linux). List yours with `ifconfig` or `ip link`.

```sh
./mcchat -i en0
```

On another host (or another terminal on the same host) on the same
network segment, run the same command to join the chat:

```sh
./mcchat -i eth0
```

Type a line and press enter to broadcast it to everyone in the group.
Press Ctrl-C to quit.

### Sending a file

Use `-f <path>` to send the contents of a file as chat messages instead
of starting an interactive session. Each line of the file is sent as
its own chat message, prefixed with your nickname exactly like
interactive messages, and the program exits as soon as it hits EOF:

```sh
./mcchat -i en0 -f notes.txt
```

Blank lines are skipped (no empty message is sent). Any read, open, or
send error is reported to stderr and causes `mcchat` to exit with a
non-zero status instead of silently skipping the problem; sending stops
at the first failed line.

### Options

```
-i <interface>   network interface to use for multicast (required)
-g <group>       IPv6 multicast group address (default: ff12::1234:5678)
-p <port>        UDP port (default: 12345)
-n <nickname>    nickname to prefix messages with (default: $USER)
-f <path>        send the file's contents as chat messages and exit
```

The default group address (`ff12::1234:5678`) is link-local scoped, so
it only reaches peers on the same link/interface. Use a matching `-g`
value on all peers if you customize it, and make sure any firewall
allows UDP traffic on the chosen port.

### Environment variables

Every option except `-f` can also be set via an environment variable.
An explicit command-line option always overrides the matching
environment variable, option by option:

```
MCCHAT_INTERFACE   same as -i
MCCHAT_GROUP       same as -g
MCCHAT_PORT        same as -p
MCCHAT_NICKNAME    same as -n
```

For example, `MCCHAT_INTERFACE=en0 ./mcchat -n override` joins on
`en0` (from the environment) using the nickname `override` (from `-n`).

### Limitations

Each chat message is a single UDP datagram capped at 1024 bytes
(including the `nickname: ` prefix). A line — whether typed
interactively or read from a file with `-f` — that is longer than the
buffer is **not** reassembled: it is split across multiple separate
chat messages, each sent and displayed on its own. Keep lines
reasonably short (well under 1024 bytes including your nickname) to
avoid this.