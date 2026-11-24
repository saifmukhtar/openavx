#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
# Copyright (c) 2026 Saif Mukhtar
"""
openavx Live Terminal Packet & Protocol Probe
Connects to openavx TCP control port (7960), captures the live wire protocol,
and verifies handshake and frame delivery.
"""

import socket
import struct
import sys
import time

def main():
    host = "127.0.0.1"
    port = 7960

    print("=" * 60)
    print(" openavx Live Terminal Wire Protocol Probe")
    print("=" * 60)
    print(f"[*] Connecting to {host}:{port} via TCP...")

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)

    try:
        s.connect((host, port))
        print(f"\033[1;32m[+] Connected successfully to openavx on port {port}!\033[0m\n")

        # 1. Read Handshake Payload Length (4 bytes)
        len_bytes = s.recv(4)
        if len(len_bytes) < 4:
            print("[-] Failed to read handshake length")
            return 1
        
        xml_len = struct.unpack("<I", len_bytes)[0]
        print(f"\033[1;36m[RX WIRE PACKET 1: HANDSHAKE HEADER]\033[0m")
        print(f"  Length: {xml_len} bytes")

        # 2. Read XML Handshake Payload
        xml_data = b""
        while len(xml_data) < xml_len:
            chunk = s.recv(xml_len - len(xml_data))
            if not chunk:
                break
            xml_data += chunk
        
        xml_str = xml_data.decode("utf-8", errors="replace")
        print(f"\033[1;33m[RX XML DESCRIPTOR]\033[0m:\n{xml_str.strip()}\n")

        assert "<oavx_stream" in xml_str, "XML missing <oavx_stream"
        print("\033[1;32m[+] Handshake XML validated successfully!\033[0m\n")

        # 3. Read Video Frame Header (36 bytes)
        # struct { uint32 magic, uint32 xres, uint32 yres, uint32 fourcc, int32 n, int32 d, int64 tc, uint32 len }
        hdr_fmt = "<IIIIiiqI"
        hdr_size = struct.calcsize(hdr_fmt)
        print(f"[*] Waiting for live video frame from sender (Header size: {hdr_size} bytes)...")

        hdr_bytes = s.recv(hdr_size)
        if len(hdr_bytes) < hdr_size:
            print(f"[-] Incomplete video header received: {len(hdr_bytes)}/{hdr_size}")
            return 1

        magic, xres, yres, fourcc, fr_n, fr_d, tc, payload_len = struct.unpack(hdr_fmt, hdr_bytes)
        fourcc_str = "".join([chr((fourcc >> (8*i)) & 0xff) for i in range(4)])
        magic_str = "".join([chr((magic >> (8*i)) & 0xff) for i in range(4)])

        print(f"\033[1;36m[RX WIRE PACKET 2: VIDEO FRAME]\033[0m")
        print(f"  Magic:       0x{magic:08X} ('{magic_str}')")
        print(f"  Resolution:  {xres} x {yres}")
        print(f"  FourCC:      0x{fourcc:08X} ('{fourcc_str}')")
        print(f"  Frame Rate:  {fr_n} / {fr_d} ({fr_n/fr_d:.2f} FPS)")
        print(f"  Timecode:    {tc}")
        print(f"  Payload Len: {payload_len} bytes ({payload_len / 1024 / 1024:.2f} MB)")

        assert magic == 0x56585641, f"Invalid magic: 0x{magic:08X}"
        assert xres == 1920, f"Expected xres 1920, got {xres}"
        assert yres == 1080, f"Expected yres 1080, got {yres}"

        # 4. Read sample payload chunk
        sample_chunk = s.recv(min(4096, payload_len))
        hex_preview = " ".join([f"{b:02X}" for b in sample_chunk[:32]])
        print(f"  Sample Hex:  {hex_preview} ...\n")

        print("\033[1;32m[+] LIVE PROTOCOL VERIFICATION PASSED!\033[0m")
        print("[+] openavx successfully streamed verified AVX packets over TCP!")
        return 0

    except Exception as e:
        print(f"\033[1;31m[-] Error during socket probe: {e}\033[0m")
        return 1
    finally:
        s.close()

if __name__ == "__main__":
    sys.exit(main())
