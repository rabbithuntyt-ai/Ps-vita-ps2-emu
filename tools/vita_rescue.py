#!/usr/bin/env python3
"""Reboot a Vita whose Vita Lab command server no longer answers.

Vita Lab's kernel plugin (with kernel/rescue.c, Vita-Lingo lab/vita) listens on UDP 1342,
independent of SceShell, where the command server runs:

    python3 tools/vita_rescue.py ping   [--host 192.168.1.115]
    python3 tools/vita_rescue.py reboot [--host 192.168.1.115]

Without --host the datagram is broadcast on the local network. "reboot" asks the power
service first and forces a hard reboot 5 seconds later if the system is still up.
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
    parser.add_argument('--timeout', type=float, default=3.0)
    args = parser.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(0.5)
    target = args.host or '255.255.255.255'
    replies = []
    deadline = time.time() + args.timeout
    sent = 0
    while time.time() < deadline:
        if sent < 3:
            s.sendto(MESSAGES[args.command], (target, PORT))
            sent += 1
        try:
            data, address = s.recvfrom(256)
        except socket.timeout:
            continue
        text = data.decode('latin1', 'replace')
        if (address[0], text) not in replies:
            replies.append((address[0], text))
            print('%s: %s' % (address[0], text))
        if args.command == 'reboot' and text.startswith('VITALAB-RESCUE-REBOOTING'):
            break
    if not replies:
        print('no answer on UDP %d (is the kernel plugin with the rescue listener installed?)' % PORT)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
