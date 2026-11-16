#!/usr/bin/env python3
"""Reboot a Vita whose Vita Lab command server no longer answers.

Vita Lab's kernel plugin (with kernel/rescue.c, Vita-Lingo lab/vita) listens on UDP 1342,
independent of SceShell, where the command server runs:

    python3 tools/vita_rescue.py reboot [--host 192.168.1.115]
    python3 tools/vita_rescue.py ping   [--host 192.168.1.115]

The kernel cannot read the datagrams' contents on this firmware (recvfrom refuses kernel
buffers), so the listener counts arrivals: 3 within 3 seconds reboot (the power service
first, a hard syscon reboot 5 seconds later if the system is still up). "reboot" sends 5.
"ping" sends exactly 1, which can never reboot; it gets no reply on this firmware, so check
the listener in ux0:data/vitalab_rescue.txt ("listening", then "arrival" per datagram).
Without --host the datagrams are broadcast on the local network.
"""
import argparse
import socket
import sys
import time

PORT = 1342
MESSAGES = {'ping': b'VITALAB-RESCUE-PING', 'reboot': b'VITALAB-RESCUE-REBOOT'}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('command', choices=sorted(MESSAGES))
    parser.add_argument('--host', help='the Vita address (default: broadcast)')
    args = parser.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(0.5)
    target = args.host or '255.255.255.255'
    count = 5 if args.command == 'reboot' else 1
    for _ in range(count):
        s.sendto(MESSAGES[args.command], (target, PORT))
        time.sleep(0.2)
    print('sent %d datagram(s) to %s:%d' % (count, target, PORT))
    try:
        data, address = s.recvfrom(256)
        print('%s: %s' % (address[0], data.decode('latin1', 'replace')))
    except socket.timeout:
        pass
    if args.command == 'reboot':
        print('if the listener is running, the Vita reboots now (hard reboot after 5 s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
