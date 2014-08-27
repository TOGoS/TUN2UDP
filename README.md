## TUN2UDP

Implements a TUN or TAP device that simply forwards all packets from
the device to a specified host over UDP, and the contents of any
received UDP packets back to the TUN/TAP device.

Using tun2udp you can create simple network bridges or forward packets
to another program for processing.

  make tun2udp   # Compile
  ./tun2udp -?   # Usage instructions

## TUN2FIFO

Implements a TUN or TAP driver by reading and writing SLIP-encoded
packets from a pair of files or pipes.  TUN/TAP options are the same
as for TUN2UDP.

  make tun2fifo  # Compile
  ./tun2fifo -?  # Usage instructions

The current implementation of TUN2FIFO uses a 2kB buffer and will have
~undefined behavior~ for packets larger than the buffer size (those
packets will be garbled).

To increase or decrease the buffer size for both tun2udp and tun2fifo,
edit bufsize.h.

## Example

This example assumes we have 2 hosts with IPv6 addresses 2001:1234::1
and 2001:9876::5.  If you want to join 2 IPv4 hosts, replace with the
hosts' IPv4 addresses and leave out the square brackets.

To create a simple tunnel:

  sudo ./tun2udp -local-address '[2001:1234::1]:55511' \
    -remote-address '[2001:9876::5]:55511' -tun -no-pi \
    -tun-dev tun2udp1 &
  sudo ip link set tun2udp1 up
  sudo ip addr add 10.9.8.1/24 dev tun2udp1
  ping 10.9.8.2

And on the other machine:

  sudo ./tun2udp -local-address '[2001:9876::5]:55511' \
    -remote-address '[2001:1234::1]:55511' -tun -no-pi \
    -tun-dev tun2udp1 &
  sudo ip link set tun2udp1 up
  sudo ip addr add 10.9.8.2/24 dev tun2udp1
  ping 10.9.8.1

In this case you could use either -tun or -tap so long as it's the
same on both ends.
