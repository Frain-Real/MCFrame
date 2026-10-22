#!/usr/bin/env python3
"""mcflash — хост-инструмент MOLP v1 (Phase 6).

Команды:
  pack     образ + метаданные -> .mcfp
  flash    залить .bin/.mcfp по UART или TCP/TLS
  verify   проверить образ в слоте (VERIFY)
  monitor  периодически печатать STATE

MOLP: SOF 'O','T', version 1, little-endian, CRC-8/SMBUS заголовка и
CRC-32/ISO-HDLC кадра. Прошивка принимается с продолжением по next_offset.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import socket
import ssl
import struct
import sys
import time
import zlib
from dataclasses import dataclass
from pathlib import Path

SOF0, SOF1, VER = 0x4F, 0x54, 1
OP_INFO, OP_START, OP_DATA, OP_END = 1, 2, 3, 4
OP_VERIFY, OP_ACTIVATE, OP_CONFIRM, OP_STATE = 8, 9, 10, 11
ACK, NAK = 0x80, 0x81
AUTO_SLOT = 0xFFFF
START_RESUME = 1 << 0
HASH_SHA256 = 1


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def frame(op: int, seq: int, payload: bytes = b"", flags: int = 0) -> bytes:
    head = struct.pack("<BBBBH", VER, op, flags, seq, len(payload))
    hcrc = bytes([crc8(head)])
    body = head + hcrc + payload
    return bytes([SOF0, SOF1]) + body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


@dataclass
class Response:
    op: int
    code: int
    state: int
    slot: int
    max_payload: int
    detail: int
    next_offset: int
    image_crc32: int
    fw_version: int
    slot_offset: int
    slot_capacity: int

    @property
    def ok(self) -> bool:
        return self.op == ACK and self.code == 0

    @property
    def text(self) -> str:
        return f"code={self.code} state={self.state} slot={self.slot} next={self.next_offset}"


class Transport:
    def read(self, n: int) -> bytes: ...
    def write(self, data: bytes) -> None: ...
    def close(self) -> None: ...


class SerialTransport(Transport):
    def __init__(self, port: str, baud: int) -> None:
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise SystemExit("нужен pyserial: pip install pyserial") from exc
        self._s = serial.Serial(port, baud, timeout=0.05)

    def read(self, n: int) -> bytes:
        return self._s.read(n)

    def write(self, data: bytes) -> None:
        self._s.write(data)

    def close(self) -> None:
        self._s.close()


class TcpTransport(Transport):
    def __init__(self, host: str, port: int, tls: bool, cafile: str | None = None) -> None:
        raw = socket.create_connection((host, port), timeout=10)
        if tls:
            ctx = ssl.create_default_context(cafile=cafile)
            self._s = ctx.wrap_socket(raw, server_hostname=host)
        else:
            self._s = raw
        self._s.settimeout(0.05)

    def read(self, n: int) -> bytes:
        try:
            return self._s.recv(n)
        except socket.timeout:
            return b""

    def write(self, data: bytes) -> None:
        self._s.sendall(data)

    def close(self) -> None:
        self._s.close()


class MolpClient:
    def __init__(self, transport: Transport, timeout: float = 5.0) -> None:
        self.t = transport
        self.timeout = timeout
        self.seq = 0
        self.buf = b""
        self.max_payload = 128

    def _read_frame(self) -> tuple[int, int, bytes]:
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            self.buf += self.t.read(256)
            start = self.buf.find(bytes([SOF0, SOF1]))
            if start < 0:
                self.buf = self.buf[-1:]
                continue
            if len(self.buf) - start < 13:
                continue
            head = self.buf[start + 2:start + 8]
            if len(head) < 6 or self.buf[start + 8] != crc8(head):
                self.buf = self.buf[start + 3:]
                continue
            length = struct.unpack("<H", head[4:6])[0]
            total = 13 + length
            if len(self.buf) - start < total:
                continue
            body = self.buf[start + 2:start + 9 + length]
            if struct.unpack("<I", self.buf[start + 9 + length:start + total])[0] != zlib.crc32(body):
                self.buf = self.buf[start + 3:]
                continue
            op = head[1]
            seq = head[3]
            payload = self.buf[start + 9:start + 9 + length]
            self.buf = self.buf[start + total:]
            return op, seq, payload
        raise TimeoutError("ответ не получен")

    def request(self, op: int, payload: bytes = b"", flags: int = 0) -> Response:
        self.t.write(frame(op, self.seq, payload, flags))
        self.seq = (self.seq + 1) & 0xFF
        while True:
            r_op, _r_seq, data = self._read_frame()
            if r_op not in (ACK, NAK) or len(data) < 28:
                continue
            code, state, slot, max_payload, detail, nxt, crc, fw, off, cap = struct.unpack(
                "<BBHHHIIIII", data[:28])
            if r_op == NAK and code == 1:  # посторонний BadFrame
                continue
            if max_payload:
                self.max_payload = min(self.max_payload, max_payload)
            return Response(r_op, code, state, slot, max_payload, detail, nxt, crc, fw, off, cap)

    def info(self) -> Response:
        r = self.request(OP_INFO, bytes([VER]))
        if not r.ok:
            raise RuntimeError(f"INFO отклонён: {r.text}")
        return r

    def start(self, image: bytes, fw_version: int, resume: bool) -> Response:
        sha = hashlib.sha256(image).digest()
        flags = START_RESUME if resume else 0
        payload = struct.pack("<IIIHHBBI32s", len(image), zlib.crc32(image) & 0xFFFFFFFF,
                              fw_version, AUTO_SLOT, self.max_payload - 4,
                              HASH_SHA256, flags, 0, sha)
        return self.request(OP_START, payload)

    def data(self, offset: int, chunk: bytes) -> Response:
        return self.request(OP_DATA, struct.pack("<I", offset) + chunk)

    def end(self, image: bytes) -> Response:
        return self.request(OP_END, struct.pack("<II32s", len(image),
                                                zlib.crc32(image) & 0xFFFFFFFF,
                                                hashlib.sha256(image).digest()))

    def activate(self) -> Response:
        return self.request(OP_ACTIVATE, struct.pack("<H", AUTO_SLOT))

    def verify(self, image: bytes) -> Response:
        payload = struct.pack("<IIHBB32s", len(image), zlib.crc32(image) & 0xFFFFFFFF,
                              AUTO_SLOT, HASH_SHA256, 0, hashlib.sha256(image).digest())
        return self.request(OP_VERIFY, payload)

    def state(self) -> Response:
        return self.request(OP_STATE)


@dataclass
class PackImage:
    image: bytes
    fw_version: int
    sha256: bytes
    crc32: int


PACK_MAGIC = b"MCFP"


def pack(image: bytes, fw_version: int) -> bytes:
    sha = hashlib.sha256(image).digest()
    return (PACK_MAGIC + struct.pack("<HIII32s", 1, fw_version, len(image),
                                     zlib.crc32(image) & 0xFFFFFFFF, sha) + image)


def unpack(data: bytes) -> PackImage:
    if len(data) < 50 or data[:4] != PACK_MAGIC:
        raise ValueError("не .mcfp")
    version, fw, size, crc, sha = struct.unpack("<HIII32s", data[4:50])
    if version != 1 or len(data) - 50 != size:
        raise ValueError("повреждённый .mcfp")
    image = data[50:]
    if (zlib.crc32(image) & 0xFFFFFFFF) != crc or hashlib.sha256(image).digest() != sha:
        raise ValueError("CRC/SHA .mcfp не сошлись")
    return PackImage(image, fw, sha, crc)


def load_image(path: str) -> PackImage:
    data = Path(path).read_bytes()
    if data[:4] == PACK_MAGIC:
        return unpack(data)
    return PackImage(data, 0, hashlib.sha256(data).digest(), zlib.crc32(data) & 0xFFFFFFFF)


def open_client(args: argparse.Namespace) -> MolpClient:
    if args.port:
        return MolpClient(SerialTransport(args.port, args.baud), args.timeout)
    if args.host:
        return MolpClient(TcpTransport(args.host, args.tcp_port, args.tls, args.cafile), args.timeout)
    raise SystemExit("нужен --port или --host")


def cmd_pack(args: argparse.Namespace) -> int:
    img = load_image(args.image)
    out = Path(args.output)
    out.write_bytes(pack(img.image, args.fw_version))
    print(f"pack: {out} image={len(img.image)} fw={args.fw_version} crc=0x{img.crc32:08x}")
    return 0


def cmd_flash(args: argparse.Namespace) -> int:
    img = load_image(args.image)
    fw = args.fw_version if args.fw_version is not None else img.fw_version
    client = open_client(args)
    try:
        client.info()
        r = client.start(img.image, fw, args.resume)
        if not r.ok:
            raise RuntimeError(f"START: {r.text}")
        offset = r.next_offset
        print(f"start: offset={offset} max_payload={client.max_payload}")
        while offset < len(img.image):
            chunk = img.image[offset:offset + max(1, client.max_payload - 4)]
            try:
                r = client.data(offset, chunk)
            except TimeoutError:
                continue
            if not r.ok and r.code == 3:  # BadOffset
                offset = r.next_offset
                continue
            if not r.ok:
                raise RuntimeError(f"DATA at {offset}: {r.text}")
            offset = r.next_offset
            pct = 100 * offset // max(1, len(img.image))
            print(f"\r{min(pct, 100)}%", end="", flush=True)
        print()
        r = client.end(img.image)
        if not r.ok:
            raise RuntimeError(f"END: {r.text}")
        if args.verify:
            r = client.verify(img.image)
            if not r.ok:
                raise RuntimeError(f"VERIFY: {r.text}")
        if args.activate:
            r = client.activate()
            if not r.ok:
                raise RuntimeError(f"ACTIVATE: {r.text}")
        print(f"done: {r.text}")
        return 0
    finally:
        client.t.close()


def cmd_verify(args: argparse.Namespace) -> int:
    img = load_image(args.image)
    client = open_client(args)
    try:
        client.info()
        r = client.verify(img.image)
        print(r.text)
        return 0 if r.ok else 1
    finally:
        client.t.close()


def cmd_monitor(args: argparse.Namespace) -> int:
    client = open_client(args)
    try:
        while True:
            r = client.state()
            print(time.strftime("%H:%M:%S"), r.text, flush=True)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0
    finally:
        client.t.close()


def main() -> int:
    ap = argparse.ArgumentParser(description="MCFrame MOLP v1 flasher")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pack")
    p.add_argument("image")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--fw-version", type=int, default=0)
    p.set_defaults(func=cmd_pack)

    def io_args(sp: argparse.ArgumentParser) -> None:
        sp.add_argument("image")
        sp.add_argument("--port")
        sp.add_argument("--baud", type=int, default=115200)
        sp.add_argument("--host")
        sp.add_argument("--tcp-port", type=int, default=3333)
        sp.add_argument("--tls", action="store_true")
        sp.add_argument("--cafile")
        sp.add_argument("--timeout", type=float, default=5.0)

    p = sub.add_parser("flash")
    io_args(p)
    p.add_argument("--fw-version", type=int)
    p.add_argument("--resume", action="store_true")
    p.add_argument("--verify", action="store_true")
    p.add_argument("--activate", action="store_true")
    p.set_defaults(func=cmd_flash)

    p = sub.add_parser("verify")
    io_args(p)
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("monitor")
    p.add_argument("--port")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--host")
    p.add_argument("--tcp-port", type=int, default=3333)
    p.add_argument("--tls", action="store_true")
    p.add_argument("--cafile")
    p.add_argument("--timeout", type=float, default=5.0)
    p.add_argument("--interval", type=float, default=1.0)
    p.set_defaults(func=cmd_monitor)

    args = ap.parse_args()
    try:
        return int(args.func(args))
    except (RuntimeError, TimeoutError, ValueError) as exc:
        print(f"mcflash: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
