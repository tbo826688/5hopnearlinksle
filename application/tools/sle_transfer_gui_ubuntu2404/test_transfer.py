import queue
import tempfile
import unittest
from pathlib import Path
from protocol import ProtocolError
from transfer import (
    ActiveRateMeter,
    AdaptiveRateController,
    CHAIN_MODE_TWO_HOP,
    ChainMonitorWorker,
    FLOW_CONTROL_RECOVERY_WATER,
    IntermittentPacingController,
    PACING_MODE_ADAPTIVE,
    PACING_MODE_CONTINUOUS,
    PACING_MODE_FIXED,
    PACING_MODE_PULSE_DRAIN,
    PACING_MODE_STABLE_260,
    PATH_FLAG_DROP,
    PATH_LINK_MASK_FIVE_HOP,
    ReceiveStallMonitor,
    ReceiverWorker,
    SenderWorker,
)


class FakeSerial:
    def __init__(self, data: bytes) -> None:
        self.data = bytearray(data)

    @property
    def in_waiting(self) -> int:
        return len(self.data)

    def read(self, length: int) -> bytes:
        result = bytes(self.data[:length])
        del self.data[:length]
        return result


class FlowControlTests(unittest.TestCase):
    def test_sender_parses_node1_flow_status(self):
        worker = SenderWorker(queue.Queue(), "COM1", "video.mp4", 300, "V5HP", 921600)
        serial_port = FakeSerial(b"boot log\r\n[FTFC]49152,7,0\r\n")

        worker._poll_flow_control(serial_port)

        self.assertTrue(worker.flow_active)
        self.assertEqual(worker.flow_free, 49152)
        self.assertEqual(worker.flow_unacked, 7)
        self.assertEqual(worker.flow_overflow, 0)

    def test_sender_parses_extended_flow_status(self):
        worker = SenderWorker(queue.Queue(), "COM1", "video.ts", 260, "V5HP", 921600)

        worker._poll_flow_control(FakeSerial(b"[FTFC]49152,8,0,12,180\r\n"))

        self.assertEqual(worker.flow_cwnd, 12)
        self.assertEqual(worker.flow_rto_ms, 180)

    def test_sender_parses_full_path_status(self):
        worker = SenderWorker(
            queue.Queue(), "COM1", "video.ts", 260, "V5HP", 921600,
            PACING_MODE_STABLE_260,
        )

        worker._poll_flow_control(
            FakeSerial(b"[FTPATH]7,19,63,3,22,0,4,0,0,10,160\r\n")
        )

        self.assertTrue(worker.path_active)
        self.assertEqual(worker.path_link_mask, PATH_LINK_MASK_FIVE_HOP)
        self.assertEqual(worker.path_bottleneck, 3)
        self.assertEqual(worker.path_queue_percent, 22)
        self.assertEqual(worker._stable_pressure_level()[0], "normal")

    def test_stable_mode_stops_on_reported_drop(self):
        worker = SenderWorker(
            queue.Queue(), "COM1", "video.ts", 260, "V5HP", 921600,
            PACING_MODE_STABLE_260,
        )
        worker._poll_flow_control(FakeSerial(b"[FTFC]50000,4,0,8,200\r\n"))
        worker._poll_flow_control(
            FakeSerial(
                f"[FTPATH]7,19,{PATH_LINK_MASK_FIVE_HOP},2,20,{PATH_FLAG_DROP},0,0,1,8,200\r\n".encode()
            )
        )

        level, _ = worker._stable_pressure_level()

        self.assertEqual(level, "severe")

    def test_stable_mode_does_not_pause_for_normal_full_window(self):
        worker = SenderWorker(
            queue.Queue(), "COM1", "video.ts", 260, "V5HP", 921600,
            PACING_MODE_STABLE_260,
        )
        worker._poll_flow_control(FakeSerial(b"[FTFC]50000,8,0,8,200\r\n"))
        worker._poll_flow_control(
            FakeSerial(b"[FTPATH]7,19,63,3,10,0,0,0,0,8,200\r\n")
        )

        self.assertEqual(worker._stable_pressure_level()[0], "normal")

    def test_adaptive_rate_reduces_under_congestion_and_recovers(self):
        controller = AdaptiveRateController(300)

        self.assertEqual(controller.update(65535, 0, 0.0), 300_000)
        self.assertEqual(controller.update(24000, 24, 0.5), 240_000)
        self.assertEqual(controller.update(26000, 12, 1.0), 216_000)
        self.assertEqual(controller.update(60000, 4, 1.5), 221_000)

    def test_continuous_pacing_is_disabled(self):
        controller = IntermittentPacingController(PACING_MODE_CONTINUOUS)

        self.assertFalse(controller.enabled)
        self.assertEqual(controller.burst_bytes, 0)

    def test_fixed_pacing_keeps_four_packet_profile(self):
        controller = IntermittentPacingController(PACING_MODE_FIXED)

        self.assertFalse(controller.update(1000, 40, True))
        self.assertEqual(controller.burst_packets, 4)
        self.assertEqual(controller.gap_seconds, 0.020)

    def test_adaptive_pacing_backs_off_and_recovers(self):
        controller = IntermittentPacingController(PACING_MODE_ADAPTIVE)

        self.assertTrue(controller.update(7000, 24, True))
        self.assertEqual(controller.burst_packets, 2)
        self.assertEqual(controller.gap_seconds, 0.100)

        for _ in range(5):
            controller.update(FLOW_CONTROL_RECOVERY_WATER, 4, True)

        self.assertEqual(controller.burst_packets, 3)
        self.assertEqual(controller.gap_seconds, 0.095)

    def test_pulse_drain_keeps_large_gap_and_peak_rate(self):
        controller = IntermittentPacingController(PACING_MODE_PULSE_DRAIN)

        self.assertTrue(controller.keeps_peak_rate)
        self.assertTrue(controller.waits_for_recovery)
        self.assertEqual(controller.burst_packets, 4)
        self.assertEqual(controller.gap_seconds, 0.400)

        controller.update(7000, 24, True)
        self.assertEqual(controller.burst_packets, 2)
        self.assertEqual(controller.gap_seconds, 1.200)

    def test_pulse_drain_does_not_reduce_active_send_rate(self):
        worker = SenderWorker(
            queue.Queue(),
            "COM1",
            "video.mp4",
            260,
            "V5HP",
            921600,
            PACING_MODE_PULSE_DRAIN,
        )

        worker._poll_flow_control(FakeSerial(b"[FTFC]7000,24,0\r\n"))

        self.assertEqual(worker.flow_rate_controller.current_bps, 260_000)

    def test_recovery_water_adapts_to_32k_firmware_buffer(self):
        worker = SenderWorker(
            queue.Queue(),
            "COM1",
            "video.mp4",
            260,
            "V5HP",
            921600,
            PACING_MODE_PULSE_DRAIN,
        )

        worker._poll_flow_control(FakeSerial(b"[FTFC]32767,1,0\r\n"))

        self.assertEqual(worker._flow_recovery_water(), 24575)

    def test_active_rate_meter_excludes_large_idle_gaps(self):
        meter = ActiveRateMeter(gap_seconds=0.150)
        for now in (0.0, 0.015, 0.030, 0.400, 0.415):
            meter.observe(512, now)

        self.assertAlmostEqual(meter.active_seconds, 0.075, places=3)
        self.assertGreater(meter.active_bps, 260_000)

    def test_sender_accepts_legacy_flow_status(self):
        worker = SenderWorker(queue.Queue(), "COM1", "video.mp4", 300, "V5HP", 921600)

        worker._poll_flow_control(FakeSerial(b"[FTFC]24576,7\r\n"))

        self.assertTrue(worker.flow_active)
        self.assertIsNone(worker.flow_overflow)

    def test_sender_stops_when_node1_overflow_increases(self):
        worker = SenderWorker(queue.Queue(), "COM1", "video.mp4", 300, "V5HP", 921600)
        worker._poll_flow_control(FakeSerial(b"[FTFC]49152,7,2\r\n"))

        with self.assertRaises(ProtocolError):
            worker._poll_flow_control(FakeSerial(b"[FTFC]48000,8,5\r\n"))

    def test_receive_stall_monitor_warns_and_recovers(self):
        monitor = ReceiveStallMonitor(100.0)

        self.assertIsNone(monitor.observe_idle(109.9))
        self.assertEqual(monitor.observe_idle(110.0), 10.0)
        self.assertIsNone(monitor.observe_idle(120.0))
        self.assertEqual(monitor.observe_data(125.0), 25.0)
        self.assertIsNone(monitor.observe_idle(134.9))
        self.assertEqual(monitor.observe_idle(135.0), 10.0)

    def test_chain_monitor_reports_first_missing_link(self):
        self.assertEqual(
            ChainMonitorWorker.describe_result({0: 0x1E, 1: 0x0F}),
            "探测停在 R1，请检查 R1 → R2 链路",
        )
        self.assertIn(
            "全部响应",
            ChainMonitorWorker.describe_result({node_id: 0x0F for node_id in range(6)}),
        )

    def test_chain_monitor_requires_every_node(self):
        self.assertFalse(ChainMonitorWorker.is_complete({0: 0x1E, 5: 0x1F}))
        self.assertTrue(
            ChainMonitorWorker.is_complete({node_id: 0x0F for node_id in range(6)})
        )

    def test_two_hop_monitor_only_requires_node1_r1_node6(self):
        reports = {0: 0x1E, 1: 0x0F, 5: 0x1F}

        self.assertTrue(ChainMonitorWorker.is_complete(reports, CHAIN_MODE_TWO_HOP))
        self.assertEqual(
            ChainMonitorWorker.describe_result(reports, CHAIN_MODE_TWO_HOP),
            "Node1、R1、Node6 全部响应",
        )

    def test_two_hop_monitor_identifies_r1_to_node6_failure(self):
        self.assertEqual(
            ChainMonitorWorker.describe_result(
                {0: 0x1E, 1: 0x0F}, CHAIN_MODE_TWO_HOP
            ),
            "探测停在 R1，请检查 R1 → Node6 链路",
        )

    def test_preserve_part_keeps_original_extension(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "received.mp4"
            part_path = Path(directory) / "received.mp4.part"
            part_path.write_bytes(b"partial video")

            saved_path = ReceiverWorker._preserve_part(
                part_path, destination, "incomplete"
            )

            self.assertIsNotNone(saved_path)
            self.assertEqual(saved_path.name, "received_incomplete.mp4")
            self.assertEqual(saved_path.read_bytes(), b"partial video")

if __name__ == "__main__":
    unittest.main()
