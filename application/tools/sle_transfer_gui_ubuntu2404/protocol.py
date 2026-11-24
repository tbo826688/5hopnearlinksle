from __future__ import annotations

import hashlib
import os
import re
import secrets
import struct
import zlib
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path


MAGIC_V1 = b"V5HP"
MAGIC_V2 = b"V5H2"
MAGIC_TEST = b"V5T1"
MAGIC_DIAG_HOST = b"V5DG"
VERSION_V2 = 2
MAX_FILENAME_BYTES = 512
MAX_FILE_SIZE = 1 << 40
V1_REMAINDER = struct.Struct("<Q32s")
V2_FIXED = struct.Struct("<4sBBHQ32sI")
V2_CRC_PAYLOAD = struct.Struct("<BBHQ32s")
TEST_FIXED = struct.Struct("<4sQH32s")
DIAG_HOST_COMMAND = struct.Struct("<4sI")
TEST_PAYLOAD_SIZE = 60 * 1024
INVALID_FILENAME = re.compile(r"[<>:\"/\\|?*\x00-\x1f]")


class ProtocolError(RuntimeError):
    pass


@dataclass(frozen=True)
class FileMetadata:
    filename: str
    file_size: int
    sha256: bytes
    protocol: str


@dataclass(frozen=True)
class LinkTestPacket:
    test_id: int
    payload: bytes
    sha256: bytes


def sha256_file(path: str | Path, chunk_size: int = 1024 * 1024) -> bytes:
    digest = hashlib.sha256()
    with Path(path).open("rb") as source_file:
        for chunk in iter(lambda: source_file.read(chunk_size), b""):
            digest.update(chunk)
    return digest.digest()


def sanitize_filename(filename: str) -> str:
    clean_name = INVALID_FILENAME.sub("_", Path(filename).name).strip(" .")
    return clean_name or f"received_{datetime.now():%Y%m%d_%H%M%S}.bin"


def encode_v2_header(filename: str, file_size: int, digest: bytes) -> bytes:
    if file_size < 0 or file_size > MAX_FILE_SIZE:
        raise ProtocolError("文件大小超出协议允许范围")
    if len(digest) != 32:
        raise ProtocolError("SHA-256 长度必须为 32 字节")

    safe_name = sanitize_filename(filename)
    filename_bytes = safe_name.encode("utf-8")
    if not filename_bytes or len(filename_bytes) > MAX_FILENAME_BYTES:
        raise ProtocolError("UTF-8 文件名长度必须在 1 到 512 字节之间")

    crc_payload = V2_CRC_PAYLOAD.pack(
        VERSION_V2, 0, len(filename_bytes), file_size, digest
    ) + filename_bytes
    header_crc = zlib.crc32(crc_payload) & 0xFFFFFFFF
    fixed_header = V2_FIXED.pack(
        MAGIC_V2,
        VERSION_V2,
        0,
        len(filename_bytes),
        file_size,
        digest,
        header_crc,
    )
    return fixed_header + filename_bytes


def encode_v1_header(file_size: int, digest: bytes) -> bytes:
    if file_size < 0 or file_size > MAX_FILE_SIZE:
        raise ProtocolError("文件大小超出协议允许范围")
    if len(digest) != 32:
        raise ProtocolError("SHA-256 长度必须为 32 字节")
    return MAGIC_V1 + V1_REMAINDER.pack(file_size, digest)


def create_link_test_packet(test_id: int | None = None) -> LinkTestPacket:
    resolved_test_id = test_id if test_id is not None else secrets.randbits(64)
    payload = os.urandom(TEST_PAYLOAD_SIZE)
    digest = hashlib.sha256(payload).digest()
    return LinkTestPacket(resolved_test_id, payload, digest)


def encode_link_test_packet(packet: LinkTestPacket) -> bytes:
    if not 0 < len(packet.payload) <= 0xFFFF:
        raise ProtocolError("链路测试负载长度无效")
    if len(packet.sha256) != 32:
        raise ProtocolError("链路测试 SHA-256 长度无效")
    return TEST_FIXED.pack(
        MAGIC_TEST,
        packet.test_id,
        len(packet.payload),
        packet.sha256,
    ) + packet.payload


def encode_chain_probe_command(probe_id: int) -> bytes:
    if not 0 <= probe_id <= 0xFFFFFFFF:
        raise ProtocolError("链路探测编号超出范围")
    return DIAG_HOST_COMMAND.pack(MAGIC_DIAG_HOST, probe_id)


def decode_v2_header(fixed_header: bytes, filename_bytes: bytes) -> FileMetadata:
    if len(fixed_header) != V2_FIXED.size:
        raise ProtocolError("V5H2 固定头长度错误")

    magic, version, flags, filename_len, file_size, digest, expected_crc = V2_FIXED.unpack(
        fixed_header
    )
    if magic != MAGIC_V2 or version != VERSION_V2 or flags != 0:
        raise ProtocolError("V5H2 文件头版本或标志无效")
    if filename_len != len(filename_bytes) or not 0 < filename_len <= MAX_FILENAME_BYTES:
        raise ProtocolError("V5H2 文件名长度无效")
    if file_size > MAX_FILE_SIZE:
        raise ProtocolError("接收文件大小超出安全限制")

    crc_payload = V2_CRC_PAYLOAD.pack(
        version, flags, filename_len, file_size, digest
    ) + filename_bytes
    actual_crc = zlib.crc32(crc_payload) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise ProtocolError("V5H2 文件头 CRC 校验失败")

    try:
        filename = sanitize_filename(filename_bytes.decode("utf-8"))
    except UnicodeDecodeError as error:
        raise ProtocolError("V5H2 文件名不是有效 UTF-8") from error

    return FileMetadata(filename, file_size, digest, "V5H2")


def decode_v1_remainder(remainder: bytes) -> FileMetadata:
    if len(remainder) != V1_REMAINDER.size:
        raise ProtocolError("V5HP 文件头长度错误")
    file_size, digest = V1_REMAINDER.unpack(remainder)
    if file_size > MAX_FILE_SIZE:
        raise ProtocolError("V5HP 文件大小超出安全限制")
    filename = f"received_{datetime.now():%Y%m%d_%H%M%S}.bin"
    return FileMetadata(filename, file_size, digest, "V5HP")


def unique_destination(directory: str | Path, filename: str) -> Path:
    directory_path = Path(directory)
    candidate = directory_path / sanitize_filename(filename)
    if not candidate.exists() and not candidate.with_name(candidate.name + ".part").exists():
        return candidate

    stem = candidate.stem
    suffix = candidate.suffix
    index = 1
    while True:
        numbered = candidate.with_name(f"{stem}_{index}{suffix}")
        if not numbered.exists() and not numbered.with_name(numbered.name + ".part").exists():
            return numbered
        index += 1
