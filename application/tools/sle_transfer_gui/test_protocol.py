import tempfile
import unittest
from pathlib import Path

from protocol import (
    DIAG_HOST_COMMAND,
    MAGIC_DIAG_HOST,
    MAGIC_TEST,
    MAGIC_V1,
    MAGIC_V2,
    ProtocolError,
    TEST_FIXED,
    TEST_PAYLOAD_SIZE,
    V1_REMAINDER,
    V2_FIXED,
    create_link_test_packet,
    decode_v1_remainder,
    decode_v2_header,
    encode_link_test_packet,
    encode_chain_probe_command,
    encode_v1_header,
    encode_v2_header,
    unique_destination,
)


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.digest = bytes(range(32))

    def test_v1_round_trip(self):
        header = encode_v1_header(123456, self.digest)
        self.assertEqual(header[:4], MAGIC_V1)
        metadata = decode_v1_remainder(header[4:])
        self.assertEqual(metadata.file_size, 123456)
        self.assertEqual(metadata.sha256, self.digest)

    def test_v2_round_trip(self):
        header = encode_v2_header("测试视频.mp4", 987654, self.digest)
        self.assertEqual(header[:4], MAGIC_V2)
        fixed = header[: V2_FIXED.size]
        filename = header[V2_FIXED.size :]
        metadata = decode_v2_header(fixed, filename)
        self.assertEqual(metadata.filename, "测试视频.mp4")
        self.assertEqual(metadata.file_size, 987654)
        self.assertEqual(metadata.sha256, self.digest)

    def test_v2_rejects_corrupt_header(self):
        header = bytearray(encode_v2_header("video.mp4", 100, self.digest))
        header[-1] ^= 0x01
        with self.assertRaises(ProtocolError):
            decode_v2_header(bytes(header[: V2_FIXED.size]), bytes(header[V2_FIXED.size :]))

    def test_unique_destination_avoids_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            existing = Path(directory) / "video.mp4"
            existing.write_bytes(b"data")
            destination = unique_destination(directory, "video.mp4")
            self.assertEqual(destination.name, "video_1.mp4")

    def test_link_test_packet_round_trip(self):
        self.assertLessEqual(TEST_PAYLOAD_SIZE, 0xFFFF)
        packet = create_link_test_packet(test_id=0x123456789ABCDEF0)
        encoded = encode_link_test_packet(packet)
        magic, test_id, payload_len, digest = TEST_FIXED.unpack(
            encoded[: TEST_FIXED.size]
        )
        payload = encoded[TEST_FIXED.size :]

        self.assertEqual(magic, MAGIC_TEST)
        self.assertEqual(test_id, packet.test_id)
        self.assertEqual(payload_len, TEST_PAYLOAD_SIZE)
        self.assertEqual(payload, packet.payload)
        self.assertEqual(digest, packet.sha256)

    def test_chain_probe_command(self):
        encoded = encode_chain_probe_command(0x12345678)
        magic, probe_id = DIAG_HOST_COMMAND.unpack(encoded)
        self.assertEqual(magic, MAGIC_DIAG_HOST)
        self.assertEqual(probe_id, 0x12345678)

if __name__ == "__main__":
    unittest.main()
