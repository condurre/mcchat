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

### Options

```
-i <interface>   network interface to use for multicast (required)
-g <group>       IPv6 multicast group address (default: ff12::1234:5678)
-p <port>        UDP port (default: 12345)
-n <nickname>    nickname to prefix messages with (default: $USER)
```

The default group address (`ff12::1234:5678`) is link-local scoped, so
it only reaches peers on the same link/interface. Use a matching `-g`
value on all peers if you customize it, and make sure any firewall
allows UDP traffic on the chosen port.