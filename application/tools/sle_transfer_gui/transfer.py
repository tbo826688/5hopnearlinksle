from __future__ import annotations

import hashlib
import os
import queue
import re
import secrets
import threading
import time
from pathlib import Path
from typing import Any

import serial

from protocol import (
    MAGIC_V1,
    MAGIC_V2,
    MAGIC_TEST,
    FileMetadata,
    MAX_FILENAME_BYTES,
    ProtocolError,
    V1_REMAINDER,
    V2_FIXED,
    TEST_FIXED,
    TEST_PAYLOAD_SIZE,
    LinkTestPacket,
    create_link_test_packet,
    decode_v1_remainder,
    decode_v2_header,
    encode_v1_header,
    encode_v2_header,
    encode_link_test_packet,
    encode_chain_probe_command,
    sha256_file,
    unique_destination,
)


TRANSFER_CHUNK_SIZE = 512
PROGRESS_INTERVAL = 0.1
RECEIVE_STALL_WARNING = 10.0
RECEIVE_STALL_REPEAT = 30.0
RECEIVE_STALL_FINALIZE = 30.0
CHAIN_PROBE_INTERVAL = 3.0
CHAIN_PROBE_RESPONSE_WINDOW = 2.5
CHAIN_PROBE_RETRY_INTERVAL = 0.5
CHAIN_STABLE_CYCLES = 3
CHAIN_MODE_FIVE_HOP = "five_hop"
CHAIN_MODE_TWO_HOP = "two_hop"
CHAIN_MODE_LABELS = {
    CHAIN_MODE_FIVE_HOP: "完整5跳模式",
    CHAIN_MODE_TWO_HOP: "两跳验证模式",
}
DIAG_REPORT_PATTERN = re.compile(rb"\[DIAG\](\d+),(\d+),(\d+)")
FLOW_CONTROL_PATTERN = re.compile(
    rb"\[FTFC\](\d+),(\d+)(?:,(\d+))?(?:,(\d+),(\d+))?"
)
PATH_STATUS_PATTERN = re.compile(
    rb"\[FTPATH\](\d+),(\d+),(\d+),(\d+),(\d+),(\d+),"
    rb"(\d+),(\d+),(\d+),(\d+),(\d+)"
)
FLOW_CONTROL_LOW_WATER = 16384
FLOW_CONTROL_TARGET_WATER = 32768
FLOW_CONTROL_RECOVERY_WATER = 49152
FLOW_CONTROL_EMERGENCY_LOW_WATER = 8192
FLOW_CONTROL_EMERGENCY_HIGH_WATER = 24576
FLOW_CONTROL_TIMEOUT = 3.0
ADAPTIVE_RATE_ADJUST_INTERVAL = 0.5
ADAPTIVE_RATE_LOG_INTERVAL = 2.0
ADAPTIVE_RATE_MIN_BPS = 20_000
ADAPTIVE_RATE_STEP_BPS = 5_000
FLOW_CONTROL_UNACKED_RECOVERY = 4
FLOW_CONTROL_UNACKED_CONGESTED = 12
FLOW_CONTROL_UNACKED_SEVERE = 24
PATH_STATUS_TIMEOUT = 1.5
PATH_DRAIN_TIMEOUT = 60.0
PATH_ACK_STALL_MIN_SECONDS = 2.0
PATH_QUEUE_RECOVERY_PERCENT = 25
PATH_QUEUE_CONGESTED_PERCENT = 35
PATH_QUEUE_SEVERE_PERCENT = 70
PATH_CREDIT_WAIT_CONGESTED = 100
PATH_LINK_MASK_FIVE_HOP = 0x3F
PATH_LINK_MASK_TWO_HOP = 0x23
PATH_FLAG_CREDIT_PRESSURE = 0x01
PATH_FLAG_TX_FAILURE = 0x02
PATH_FLAG_DROP = 0x04
PATH_FLAG_LINK_NOT_READY = 0x08
PATH_FLAG_RX_ERROR = 0x10
PATH_FLAG_OUTPUT_PRESSURE = 0x20
PACING_MODE_CONTINUOUS = "continuous"
PACING_MODE_FIXED = "fixed_burst"
PACING_MODE_ADAPTIVE = "adaptive_burst"
PACING_MODE_PULSE_DRAIN = "pulse_drain"
PACING_MODE_STABLE_260 = "stable_260"
PACING_MODE_DEFAULT = PACING_MODE_STABLE_260
PACING_MODE_LABELS = {
    PACING_MODE_STABLE_260: "SLE闭环稳定260（推荐）",
    PACING_MODE_PULSE_DRAIN: "260峰值排空（大间隔）",
    PACING_MODE_ADAPTIVE: "自适应间歇（推荐）",
    PACING_MODE_FIXED: "固定间歇（4包/20ms）",
    PACING_MODE_CONTINUOUS: "连续自适应（兼容）",
}
PACING_FRAME_PAYLOAD = 900
PACING_BURST_MIN_PACKETS = 2
PACING_BURST_INITIAL_PACKETS = 4
PACING_BURST_MAX_PACKETS = 8
PACING_GAP_MIN_SECONDS = 0.010
PACING_GAP_INITIAL_SECONDS = 0.020
PACING_GAP_CONGESTED_MAX_SECONDS = 0.080
PACING_GAP_MAX_SECONDS = 0.100
PACING_STABLE_CYCLES = 5
PULSE_BURST_MIN_PACKETS = 2
PULSE_BURST_INITIAL_PACKETS = 4
PULSE_BURST_MAX_PACKETS = 6
PULSE_GAP_MIN_SECONDS = 0.250
PULSE_GAP_INITIAL_SECONDS = 0.400
PULSE_GAP_MAX_SECONDS = 1.200
PULSE_RECOVERY_STABLE_REPORTS = 2
ACTIVE_RATE_GAP_SECONDS = 0.150


class TransferCancelled(RuntimeError):
    pass


class AdaptiveRateController:
    def __init__(self, target_kbps: int) -> None:
        self.target_bps = max(target_kbps * 1000, 1)
        self.minimum_bps = min(self.target_bps, ADAPTIVE_RATE_MIN_BPS)
        self.current_bps = self.target_bps
        self.last_adjust_at: float | None = None
        self.last_adjust_free: int | None = None

    def update(self, free_bytes: int, unacked: int, now: float) -> int:
        if self.last_adjust_at is None:
            self.last_adjust_at = now
            self.last_adjust_free = free_bytes
            return self.current_bps
        if now - self.last_adjust_at < ADAPTIVE_RATE_ADJUST_INTERVAL:
            return self.current_bps

        previous_free = (
            self.last_adjust_free
            if self.last_adjust_free is not None
            else free_bytes
        )
        free_delta = free_bytes - previous_free
        if (
            free_bytes <= FLOW_CONTROL_LOW_WATER
            and free_delta < 0
        ) or unacked >= FLOW_CONTROL_UNACKED_SEVERE:
            self.current_bps = max(
                self.minimum_bps, int(self.current_bps * 0.80)
            )
        elif (
            free_bytes < FLOW_CONTROL_TARGET_WATER
            and free_delta < -256
        ) or unacked >= FLOW_CONTROL_UNACKED_CONGESTED:
            self.current_bps = max(
                self.minimum_bps, int(self.current_bps * 0.90)
            )
        elif (
            free_bytes >= FLOW_CONTROL_RECOVERY_WATER
            and free_delta >= 0
            and unacked <= FLOW_CONTROL_UNACKED_RECOVERY
        ):
            self.current_bps = min(
                self.target_bps, self.current_bps + ADAPTIVE_RATE_STEP_BPS
            )

        self.last_adjust_at = now
        self.last_adjust_free = free_bytes
        return self.current_bps


class IntermittentPacingController:
    def __init__(self, mode: str) -> None:
        self.mode = mode if mode in PACING_MODE_LABELS else PACING_MODE_DEFAULT
        if self.mode == PACING_MODE_CONTINUOUS:
            self.burst_packets = 0
            self.gap_seconds = 0.0
        elif self.mode == PACING_MODE_PULSE_DRAIN:
            self.burst_packets = PULSE_BURST_INITIAL_PACKETS
            self.gap_seconds = PULSE_GAP_INITIAL_SECONDS
        else:
            self.burst_packets = PACING_BURST_INITIAL_PACKETS
            self.gap_seconds = PACING_GAP_INITIAL_SECONDS
        self.stable_cycles = 0

    @property
    def enabled(self) -> bool:
        return self.mode not in (PACING_MODE_CONTINUOUS, PACING_MODE_STABLE_260)

    @property
    def keeps_peak_rate(self) -> bool:
        return self.mode == PACING_MODE_PULSE_DRAIN

    @property
    def waits_for_recovery(self) -> bool:
        return self.mode == PACING_MODE_PULSE_DRAIN

    @property
    def burst_bytes(self) -> int:
        return self.burst_packets * PACING_FRAME_PAYLOAD

    def update(
        self,
        free_bytes: int,
        unacked: int,
        flow_active: bool,
        recovery_water: int = FLOW_CONTROL_RECOVERY_WATER,
    ) -> bool:
        if self.mode not in (PACING_MODE_ADAPTIVE, PACING_MODE_PULSE_DRAIN) or not flow_active:
            return False

        previous_profile = (self.burst_packets, self.gap_seconds)
        if self.mode == PACING_MODE_PULSE_DRAIN:
            if (
                free_bytes <= FLOW_CONTROL_EMERGENCY_LOW_WATER
                or unacked >= FLOW_CONTROL_UNACKED_SEVERE
            ):
                self.burst_packets = PULSE_BURST_MIN_PACKETS
                self.gap_seconds = PULSE_GAP_MAX_SECONDS
                self.stable_cycles = 0
            elif (
                free_bytes <= FLOW_CONTROL_LOW_WATER
                or unacked >= FLOW_CONTROL_UNACKED_CONGESTED
            ):
                self.burst_packets = max(
                    PULSE_BURST_MIN_PACKETS, self.burst_packets - 1
                )
                self.gap_seconds = min(
                    PULSE_GAP_MAX_SECONDS, self.gap_seconds + 0.100
                )
                self.stable_cycles = 0
            elif (
                free_bytes >= recovery_water
                and unacked <= FLOW_CONTROL_UNACKED_RECOVERY
            ):
                self.stable_cycles += 1
                if self.stable_cycles >= PACING_STABLE_CYCLES:
                    self.burst_packets = min(
                        PULSE_BURST_MAX_PACKETS, self.burst_packets + 1
                    )
                    self.gap_seconds = max(
                        PULSE_GAP_MIN_SECONDS, self.gap_seconds - 0.050
                    )
                    self.stable_cycles = 0
            else:
                self.stable_cycles = 0
            return previous_profile != (self.burst_packets, self.gap_seconds)

        if (
            free_bytes <= FLOW_CONTROL_EMERGENCY_LOW_WATER
            or unacked >= FLOW_CONTROL_UNACKED_SEVERE
        ):
            self.burst_packets = PACING_BURST_MIN_PACKETS
            self.gap_seconds = PACING_GAP_MAX_SECONDS
            self.stable_cycles = 0
        elif (
            free_bytes <= FLOW_CONTROL_LOW_WATER
            or unacked >= FLOW_CONTROL_UNACKED_CONGESTED
        ):
            self.burst_packets = max(
                PACING_BURST_MIN_PACKETS, self.burst_packets - 1
            )
            self.gap_seconds = min(
                PACING_GAP_CONGESTED_MAX_SECONDS, self.gap_seconds + 0.010
            )
            self.stable_cycles = 0
        elif (
            free_bytes >= FLOW_CONTROL_RECOVERY_WATER
            and unacked <= FLOW_CONTROL_UNACKED_RECOVERY
        ):
            self.stable_cycles += 1
            if self.stable_cycles >= PACING_STABLE_CYCLES:
                self.burst_packets = min(
                    PACING_BURST_MAX_PACKETS, self.burst_packets + 1
                )
                self.gap_seconds = max(
                    PACING_GAP_MIN_SECONDS, self.gap_seconds - 0.005
                )
                self.stable_cycles = 0
        else:
            self.stable_cycles = 0

        return previous_profile != (self.burst_packets, self.gap_seconds)


class ActiveRateMeter:
    def __init__(self, gap_seconds: float = ACTIVE_RATE_GAP_SECONDS) -> None:
        self.gap_seconds = gap_seconds
        self.total_bytes = 0
        self.active_interval_seconds = 0.0
        self.active_interval_count = 0
        self.segment_count = 0
        self.last_data_at: float | None = None

    def observe(self, byte_count: int, now: float) -> None:
        if byte_count <= 0:
            return
        self.total_bytes += byte_count
        if self.last_data_at is None:
            self.segment_count = 1
        else:
            interval = max(now - self.last_data_at, 0.0)
            if interval > self.gap_seconds:
                self.segment_count += 1
            else:
                self.active_interval_seconds += interval
                self.active_interval_count += 1
        self.last_data_at = now

    @property
    def active_seconds(self) -> float:
        if self.active_interval_count <= 0:
            return 0.0
        mean_interval = self.active_interval_seconds / self.active_interval_count
        return self.active_interval_seconds + mean_interval * self.segment_count

    @property
    def active_bps(self) -> float:
        active_seconds = self.active_seconds
        if active_seconds <= 0:
            return 0.0
        return self.total_bytes * 8 / active_seconds


class ReceiveStallMonitor:
    def __init__(self, now: float) -> None:
        self.last_data_at = now
        self.last_notice_at: float | None = None
        self.stalled = False

    def observe_idle(self, now: float) -> float | None:
        duration = now - self.last_data_at
        if duration < RECEIVE_STALL_WARNING:
            return None
        if self.last_notice_at is not None and now - self.last_notice_at < RECEIVE_STALL_REPEAT:
            return None
        self.stalled = True
        self.last_notice_at = now
        return duration

    def observe_data(self, now: float) -> float | None:
        stalled_duration = now - self.last_data_at if self.stalled else None
        self.last_data_at = now
        self.last_notice_at = None
        self.stalled = False
        return stalled_duration


class BaseWorker(threading.Thread):
    def __init__(self, event_queue: queue.Queue[dict[str, Any]]) -> None:
        super().__init__(daemon=True)
        self.event_queue = event_queue
        self.cancel_event = threading.Event()

    def emit(self, event_type: str, **payload: Any) -> None:
        self.event_queue.put({"type": event_type, **payload})

    def cancel(self) -> None:
        self.cancel_event.set()

    def ensure_active(self) -> None:
        if self.cancel_event.is_set():
            raise TransferCancelled("用户已取消传输")


class SenderWorker(BaseWorker):
    def __init__(
        self,
        event_queue: queue.Queue[dict[str, Any]],
        port: str,
        source_path: str,
        rate_kbps: int,
        protocol: str,
        baudrate: int,
        pacing_mode: str = PACING_MODE_DEFAULT,
        chain_mode: str = CHAIN_MODE_FIVE_HOP,
    ) -> None:
        super().__init__(event_queue)
        self.port = port
        self.source_path = Path(source_path)
        self.rate_kbps = rate_kbps
        self.protocol = protocol
        self.baudrate = baudrate
        self.chain_mode = (
            chain_mode if chain_mode in CHAIN_MODE_LABELS else CHAIN_MODE_FIVE_HOP
        )
        self.flow_buffer = bytearray()
        self.flow_active = False
        self.flow_paused = False
        self.flow_free = 0
        self.flow_unacked = 0
        self.flow_overflow: int | None = None
        self.flow_cwnd = 8
        self.flow_rto_ms = 240
        self.flow_last_update = 0.0
        self.flow_capacity_seen = 0
        self.flow_rate_controller = AdaptiveRateController(rate_kbps)
        self.flow_last_rate_log = 0.0
        self.pacing_controller = IntermittentPacingController(pacing_mode)
        self.pacing_bytes = 0
        self.path_active = False
        self.path_last_update = 0.0
        self.path_generation = 0
        self.path_session = 0
        self.path_ack_seq = 0
        self.path_ack_progress_at = 0.0
        self.path_link_mask = 0
        self.path_bottleneck = 0
        self.path_queue_percent = 0
        self.path_flags = 0
        self.path_credit_wait = 0
        self.path_tx_fail = 0
        self.path_drop = 0
        self.path_last_log = 0.0
        self.path_pause_announced = False

    def _write_all(self, serial_port: serial.Serial, data: bytes) -> None:
        offset = 0
        while offset < len(data):
            self.ensure_active()
            written = serial_port.write(data[offset:])
            if written <= 0:
                raise serial.SerialTimeoutException("串口写入未取得进展")
            offset += written

    def _poll_flow_control(self, serial_port: serial.Serial) -> bool:
        waiting = serial_port.in_waiting
        if waiting <= 0:
            return False
        updated = False
        self.flow_buffer.extend(serial_port.read(waiting))
        while b"\n" in self.flow_buffer:
            raw_line, _, remainder = self.flow_buffer.partition(b"\n")
            self.flow_buffer = bytearray(remainder)
            match = FLOW_CONTROL_PATTERN.search(raw_line)
            if match is not None:
                was_active = self.flow_active
                self.flow_free = int(match.group(1))
                self.flow_capacity_seen = max(self.flow_capacity_seen, self.flow_free)
                self.flow_unacked = int(match.group(2))
                overflow = int(match.group(3)) if match.group(3) is not None else None
                if match.group(4) is not None:
                    self.flow_cwnd = max(int(match.group(4)), 1)
                if match.group(5) is not None:
                    self.flow_rto_ms = max(int(match.group(5)), 1)
                if overflow is not None:
                    if self.flow_overflow is not None and overflow > self.flow_overflow:
                        dropped = overflow - self.flow_overflow
                        self.flow_overflow = overflow
                        raise ProtocolError(
                            f"Node1 UART 缓冲溢出，已丢失 {dropped} 字节；请重新发送"
                        )
                    self.flow_overflow = overflow
                now = time.monotonic()
                self.flow_last_update = now
                self.flow_active = True
                updated = True
                if not was_active:
                    self.emit("phase", message="已启用 Node1 缓冲自适应发送")
                if self.pacing_controller.mode not in (
                    PACING_MODE_PULSE_DRAIN,
                    PACING_MODE_STABLE_260,
                ):
                    old_rate_bps = self.flow_rate_controller.current_bps
                    new_rate_bps = self.flow_rate_controller.update(
                        self.flow_free, self.flow_unacked, now
                    )
                    if (
                        new_rate_bps != old_rate_bps
                        and now - self.flow_last_rate_log >= ADAPTIVE_RATE_LOG_INTERVAL
                    ):
                        self.emit(
                            "flow_rate",
                            rate_bps=new_rate_bps,
                            limit_bps=self.flow_rate_controller.target_bps,
                            free_bytes=self.flow_free,
                            unacked=self.flow_unacked,
                        )
                        self.flow_last_rate_log = now
                continue

            path_match = PATH_STATUS_PATTERN.search(raw_line)
            if path_match is None:
                continue
            path_session = int(path_match.group(1))
            path_ack_seq = int(path_match.group(2))
            now = time.monotonic()
            if (
                path_session != self.path_session
                or self.path_ack_progress_at == 0.0
                or (
                    path_ack_seq != self.path_ack_seq
                    and ((path_ack_seq - self.path_ack_seq) & 0xFFFF) < 0x8000
                )
            ):
                self.path_ack_progress_at = now
            self.path_session = path_session
            self.path_ack_seq = path_ack_seq
            self.path_link_mask = int(path_match.group(3))
            self.path_bottleneck = int(path_match.group(4))
            self.path_queue_percent = int(path_match.group(5))
            self.path_flags = int(path_match.group(6))
            self.path_credit_wait = int(path_match.group(7))
            self.path_tx_fail = int(path_match.group(8))
            self.path_drop = int(path_match.group(9))
            self.flow_cwnd = max(int(path_match.group(10)), 1)
            self.flow_rto_ms = max(int(path_match.group(11)), 1)
            self.path_last_update = now
            self.path_generation += 1
            self.path_active = True
            updated = True
        if len(self.flow_buffer) > 4096:
            del self.flow_buffer[:-256]
        return updated

    def _flow_recovery_water(self) -> int:
        if self.flow_capacity_seen <= 0:
            return FLOW_CONTROL_EMERGENCY_HIGH_WATER
        estimated_water = int(self.flow_capacity_seen * 0.75)
        return min(
            FLOW_CONTROL_RECOVERY_WATER,
            max(FLOW_CONTROL_LOW_WATER, estimated_water),
        )

    def _wait_for_flow_control(self, serial_port: serial.Serial) -> float:
        self._poll_flow_control(serial_port)
        if not self.flow_active:
            return 0.0
        if (
            not self.flow_paused
            and self.flow_free > FLOW_CONTROL_EMERGENCY_LOW_WATER
        ):
            return 0.0

        wait_started = time.monotonic()
        if not self.flow_paused:
            self.flow_paused = True
            self.emit("phase", message="Node1 缓冲进入安全区，暂时停止写入…")

        while self.flow_free < FLOW_CONTROL_EMERGENCY_HIGH_WATER:
            self.ensure_active()
            self._poll_flow_control(serial_port)
            if time.monotonic() - self.flow_last_update >= FLOW_CONTROL_TIMEOUT:
                raise ProtocolError("Node1 缓冲状态反馈中断，请检查发送串口或重新连接链路")
            time.sleep(0.01)

        self.flow_paused = False
        self.emit("phase", message="Node1 缓冲已恢复，继续自适应发送")
        return time.monotonic() - wait_started

    def _expected_path_mask(self) -> int:
        if self.chain_mode == CHAIN_MODE_TWO_HOP:
            return PATH_LINK_MASK_TWO_HOP
        return PATH_LINK_MASK_FIVE_HOP

    def _stable_pressure_level(self) -> tuple[str, str]:
        now = time.monotonic()

        if self.flow_active:
            if self.flow_free <= FLOW_CONTROL_EMERGENCY_LOW_WATER:
                return "severe", f"Node1剩余缓冲仅{self.flow_free}字节"
            if self.flow_unacked > self.flow_cwnd:
                return "severe", f"Node1窗口状态异常：{self.flow_unacked}/{self.flow_cwnd}"
            if self.flow_free <= FLOW_CONTROL_LOW_WATER:
                return "mild", f"Node1缓冲{self.flow_free}字节，未确认{self.flow_unacked}包"

        path_fresh = self.path_active and now - self.path_last_update <= PATH_STATUS_TIMEOUT
        if self.path_active and not path_fresh:
            return "severe", "全链路状态反馈超时"
        if not self.path_active:
            return "normal", "等待全链路状态"

        expected_mask = self._expected_path_mask()
        if self.path_link_mask != expected_mask:
            return "severe", f"链路掩码0x{self.path_link_mask:02X}/0x{expected_mask:02X}"
        if self.path_flags & (PATH_FLAG_DROP | PATH_FLAG_LINK_NOT_READY):
            return "severe", f"节点{self.path_bottleneck}报告链路异常0x{self.path_flags:02X}"
        if self.path_queue_percent >= PATH_QUEUE_SEVERE_PERCENT:
            return "severe", (
                f"节点{self.path_bottleneck}队列占用{self.path_queue_percent}%"
            )
        ack_stall_limit = max(
            PATH_ACK_STALL_MIN_SECONDS,
            self.flow_rto_ms * 6 / 1000.0,
        )
        if (
            self.flow_unacked > 0
            and self.path_ack_progress_at > 0
            and now - self.path_ack_progress_at >= ack_stall_limit
        ):
            return "severe", (
                f"端到端确认序号{self.path_ack_seq}已{now - self.path_ack_progress_at:.1f}秒未推进"
            )
        if (
            self.path_queue_percent >= PATH_QUEUE_CONGESTED_PERCENT
            or self.path_credit_wait >= PATH_CREDIT_WAIT_CONGESTED
            or self.path_tx_fail > 0
            or self.path_flags & (PATH_FLAG_TX_FAILURE | PATH_FLAG_RX_ERROR | PATH_FLAG_OUTPUT_PRESSURE)
        ):
            return "mild", (
                f"瓶颈节点{self.path_bottleneck}，队列{self.path_queue_percent}%，"
                f"信用等待{self.path_credit_wait}"
            )
        return "normal", (
            f"链路正常，瓶颈节点{self.path_bottleneck}，队列{self.path_queue_percent}%"
        )

    def _emit_path_status(self, level: str, reason: str) -> None:
        now = time.monotonic()
        if now - self.path_last_log < 1.0:
            return
        self.emit(
            "path_status",
            level=level,
            reason=reason,
            link_mask=self.path_link_mask,
            bottleneck=self.path_bottleneck,
            queue_percent=self.path_queue_percent,
            unacked=self.flow_unacked,
            cwnd=self.flow_cwnd,
            rto_ms=self.flow_rto_ms,
        )
        self.path_last_log = now

    def _wait_for_stable_path(self, serial_port: serial.Serial) -> float:
        self._poll_flow_control(serial_port)
        level, reason = self._stable_pressure_level()
        if level == "normal":
            self.path_pause_announced = False
            self._emit_path_status(level, reason)
            return 0.0

        if level == "mild":
            delay = min(
                0.008,
                0.002 + max(self.path_queue_percent - PATH_QUEUE_CONGESTED_PERCENT, 0) * 0.0001,
            )
            self._emit_path_status(level, reason)
            deadline = time.monotonic() + delay
            while time.monotonic() < deadline:
                self.ensure_active()
                self._poll_flow_control(serial_port)
                remaining = deadline - time.monotonic()
                if remaining > 0:
                    time.sleep(min(remaining, 0.002))
            return delay

        wait_started = time.monotonic()
        recovery_started: float | None = None
        if not self.path_pause_announced:
            self.emit("phase", message=f"全链路进入排空保护：{reason}")
            self.path_pause_announced = True
        self._emit_path_status(level, reason)
        while True:
            self.ensure_active()
            self._poll_flow_control(serial_port)
            now = time.monotonic()
            level, reason = self._stable_pressure_level()
            if level == "normal":
                if recovery_started is None:
                    recovery_started = now
                elif now - recovery_started >= 0.15:
                    break
            else:
                recovery_started = None
            if self.flow_active and now - self.flow_last_update >= FLOW_CONTROL_TIMEOUT:
                raise ProtocolError("Node1状态反馈中断，无法安全继续发送")
            if now - wait_started >= PATH_DRAIN_TIMEOUT:
                raise ProtocolError(f"五跳链路持续拥塞超过{int(PATH_DRAIN_TIMEOUT)}秒：{reason}")
            time.sleep(0.01)

        self.path_pause_announced = False
        self.emit("phase", message="全链路队列已恢复，继续以260 kbps发送")
        return time.monotonic() - wait_started

    def _wait_for_pacing_interval(
        self, serial_port: serial.Serial, written_bytes: int
    ) -> float:
        controller = self.pacing_controller
        if controller.mode == PACING_MODE_STABLE_260:
            return self._wait_for_stable_path(serial_port)
        if not controller.enabled:
            return 0.0

        self.pacing_bytes += written_bytes
        if self.pacing_bytes < controller.burst_bytes:
            return 0.0
        self.pacing_bytes -= controller.burst_bytes

        profile_changed = controller.update(
            self.flow_free,
            self.flow_unacked,
            self.flow_active,
            self._flow_recovery_water(),
        )
        if profile_changed:
            self.emit(
                "pacing_profile",
                message=(
                    f"间歇策略调整：每批 {controller.burst_packets} 包，"
                    f"间隔 {controller.gap_seconds * 1000:.0f} ms；"
                    f"Node1 空闲 {self.flow_free} 字节，未确认 {self.flow_unacked} 包"
                ),
            )

        wait_started = time.monotonic()
        deadline = wait_started + controller.gap_seconds
        recovery_reports = 0
        drain_announced = False
        while True:
            self.ensure_active()
            status_updated = self._poll_flow_control(serial_port)
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(remaining, 0.01))
                continue
            if not controller.waits_for_recovery or not self.flow_active:
                break
            recovery_water = self._flow_recovery_water()
            recovered = (
                self.flow_free >= recovery_water
                and self.flow_unacked <= FLOW_CONTROL_UNACKED_RECOVERY
            )
            if recovered:
                if status_updated:
                    recovery_reports += 1
                if recovery_reports >= PULSE_RECOVERY_STABLE_REPORTS:
                    if drain_announced:
                        self.emit(
                            "phase",
                            message="Node1 缓冲和未确认包已恢复，开始下一轮260峰值发送",
                        )
                    break
            else:
                recovery_reports = 0
                if not drain_announced:
                    drain_announced = True
                    self.emit(
                        "phase",
                        message=(
                            "本轮峰值发送完成，等待链路排空；"
                            f"Node1 空闲 {self.flow_free}/{recovery_water} 字节，"
                            f"未确认 {self.flow_unacked} 包"
                        ),
                    )
            if time.monotonic() - self.flow_last_update >= FLOW_CONTROL_TIMEOUT:
                raise ProtocolError("Node1 缓冲状态反馈中断，请检查发送串口或重新连接链路")
            time.sleep(0.01)
        return time.monotonic() - wait_started

    def run(self) -> None:
        try:
            if not self.source_path.is_file():
                raise FileNotFoundError(f"文件不存在：{self.source_path}")
            if self.rate_kbps <= 0:
                raise ValueError("发送限速必须大于 0 kbps")

            file_size = self.source_path.stat().st_size
            self.emit("phase", message="正在计算 SHA-256…")
            digest = sha256_file(self.source_path)
            self.ensure_active()
            if self.protocol == "V5HP":
                header = encode_v1_header(file_size, digest)
            elif self.protocol == "V5H2":
                header = encode_v2_header(self.source_path.name, file_size, digest)
            else:
                raise ProtocolError(f"不支持的发送协议：{self.protocol}")
            self.emit(
                "metadata",
                filename=self.source_path.name,
                file_size=file_size,
                sha256=digest.hex(),
                protocol=self.protocol,
            )

            with serial.Serial(
                self.port,
                self.baudrate,
                timeout=0.2,
                write_timeout=5,
                rtscts=False,
                dsrdtr=False,
            ) as serial_port:
                serial_port.reset_input_buffer()
                self.emit("phase", message=f"已打开 {self.port}，正在发送文件头…")
                self._write_all(serial_port, header)
                pacing_label = PACING_MODE_LABELS[self.pacing_controller.mode]
                if self.pacing_controller.enabled:
                    self.emit(
                        "phase",
                        message=(
                            f"发送节奏：{pacing_label}，初始每批 "
                            f"{self.pacing_controller.burst_packets} 包 / "
                            f"{self.pacing_controller.gap_seconds * 1000:.0f} ms"
                        ),
                    )
                else:
                    self.emit("phase", message=f"发送节奏：{pacing_label}")

                sent = 0
                active_send_seconds = 0.0
                start_time = time.monotonic()
                next_send_at = start_time
                last_progress_time = 0.0
                with self.source_path.open("rb") as source_file:
                    while True:
                        self.ensure_active()
                        chunk = source_file.read(TRANSFER_CHUNK_SIZE)
                        if not chunk:
                            break

                        self._wait_for_flow_control(serial_port)
                        now = time.monotonic()
                        send_started_at = max(next_send_at, now)
                        if send_started_at > now:
                            time.sleep(send_started_at - now)
                        self._write_all(serial_port, chunk)
                        sent += len(chunk)
                        current_rate_bps = max(
                            self.flow_rate_controller.target_bps
                            if self.pacing_controller.keeps_peak_rate
                            else self.flow_rate_controller.current_bps,
                            1,
                        )
                        active_send_seconds += len(chunk) * 8 / current_rate_bps
                        next_send_at = send_started_at + len(chunk) * 8 / current_rate_bps
                        self._wait_for_pacing_interval(serial_port, len(chunk))

                        now = time.monotonic()
                        if now - last_progress_time >= PROGRESS_INTERVAL or sent == file_size:
                            elapsed = max(now - start_time, 0.001)
                            average_bps = sent * 8 / elapsed
                            active_bps = (
                                sent * 8 / max(active_send_seconds, 0.001)
                                if self.pacing_controller.keeps_peak_rate
                                else average_bps
                            )
                            remaining = max(file_size - sent, 0)
                            eta = remaining * elapsed / max(sent, 1)
                            self.emit(
                                "progress",
                                transferred=sent,
                                total=file_size,
                                speed_bps=active_bps,
                                average_bps=average_bps,
                                eta=eta,
                            )
                            last_progress_time = now

                serial_port.flush()

            elapsed = max(time.monotonic() - start_time, 0.001)
            active_bps = file_size * 8 / max(active_send_seconds, 0.001)
            self.emit(
                "complete",
                message="文件已写入 Node1，请等待接收端完成并校验",
                sha256=digest.hex(),
                average_bps=file_size * 8 / elapsed,
                active_bps=(
                    active_bps
                    if self.pacing_controller.keeps_peak_rate
                    else file_size * 8 / elapsed
                ),
            )
        except TransferCancelled as error:
            self.emit("cancelled", message=str(error))
        except Exception as error:
            self.emit("error", message=str(error))


class ReceiverWorker(BaseWorker):
    def __init__(
        self,
        event_queue: queue.Queue[dict[str, Any]],
        port: str,
        destination_directory: str,
        legacy_filename: str,
        baudrate: int,
    ) -> None:
        super().__init__(event_queue)
        self.port = port
        self.destination_directory = Path(destination_directory)
        self.legacy_filename = legacy_filename
        self.baudrate = baudrate

    def _read_exact(self, serial_port: serial.Serial, length: int) -> bytes:
        data = bytearray()
        while len(data) < length:
            self.ensure_active()
            chunk = serial_port.read(length - len(data))
            if chunk:
                data.extend(chunk)
        return bytes(data)

    @staticmethod
    def _preserve_part(
        part_path: Path | None,
        destination_path: Path | None,
        marker: str,
    ) -> Path | None:
        if (
            part_path is None
            or destination_path is None
            or not part_path.exists()
            or part_path.stat().st_size == 0
        ):
            return None
        marked_name = f"{destination_path.stem}_{marker}{destination_path.suffix}"
        saved_path = unique_destination(destination_path.parent, marked_name)
        os.replace(part_path, saved_path)
        return saved_path

    def _wait_for_magic(self, serial_port: serial.Serial) -> tuple[bytes, int]:
        window = bytearray()
        ignored = 0
        while True:
            self.ensure_active()
            byte = serial_port.read(1)
            if not byte:
                continue

            window.extend(byte)
            if len(window) > 4:
                del window[0]
            if bytes(window) in (MAGIC_V1, MAGIC_V2, MAGIC_TEST):
                return bytes(window), ignored

            ignored += 1
            if ignored and ignored % 65536 == 0:
                self.emit("ignored", count=ignored)

    def _read_metadata(self, serial_port: serial.Serial, magic: bytes):
        if magic == MAGIC_V1:
            metadata = decode_v1_remainder(
                self._read_exact(serial_port, V1_REMAINDER.size)
            )
            return FileMetadata(
                self.legacy_filename,
                metadata.file_size,
                metadata.sha256,
                metadata.protocol,
            )

        fixed_tail = self._read_exact(serial_port, V2_FIXED.size - len(MAGIC_V2))
        fixed_header = MAGIC_V2 + fixed_tail
        _, _, _, filename_len, _, _, _ = V2_FIXED.unpack(fixed_header)
        if not 0 < filename_len <= MAX_FILENAME_BYTES:
            raise ProtocolError("接收到的 V5H2 文件名长度无效")
        filename_bytes = self._read_exact(serial_port, filename_len)
        return decode_v2_header(fixed_header, filename_bytes)

    def _receive_link_test(
        self, serial_port: serial.Serial, magic: bytes, ignored: int
    ) -> None:
        fixed_tail = self._read_exact(serial_port, TEST_FIXED.size - len(magic))
        _, test_id, payload_len, expected_digest = TEST_FIXED.unpack(magic + fixed_tail)
        if payload_len != TEST_PAYLOAD_SIZE:
            raise ProtocolError(f"链路测试长度错误：{payload_len}")
        start_time = time.monotonic()
        payload = self._read_exact(serial_port, payload_len)
        elapsed = max(time.monotonic() - start_time, 0.001)
        actual_digest = hashlib.sha256(payload).digest()
        if actual_digest != expected_digest:
            raise ProtocolError("链路测试 SHA-256 校验失败")
        average_bps = payload_len * 8 / elapsed
        self.emit(
            "link_test_result",
            success=True,
            test_id=test_id,
            payload_len=payload_len,
            ignored=ignored,
            sha256=actual_digest.hex(),
            average_bps=average_bps,
            message=(
                f"链路测速通过：{payload_len} 字节，SHA-256 正确，"
                f"有效接收速率 {average_bps / 1000:.1f} kbps"
            ),
        )

    def run(self) -> None:
        part_path: Path | None = None
        destination_path: Path | None = None
        received = 0
        try:
            self.destination_directory.mkdir(parents=True, exist_ok=True)
            with serial.Serial(
                self.port,
                self.baudrate,
                timeout=0.2,
                write_timeout=5,
                rtscts=False,
                dsrdtr=False,
            ) as serial_port:
                serial_port.reset_input_buffer()
                self.emit("phase", message=f"正在监听 {self.port}，等待文件…")
                while True:
                    magic, ignored = self._wait_for_magic(serial_port)
                    if magic != MAGIC_TEST:
                        break
                    try:
                        self._receive_link_test(serial_port, magic, ignored)
                        self.emit("phase", message="链路测试完成，继续等待文件…")
                    except ProtocolError as error:
                        self.emit(
                            "link_test_result",
                            success=False,
                            test_id=None,
                            payload_len=0,
                            ignored=ignored,
                            sha256="",
                            message=str(error),
                        )
                metadata = self._read_metadata(serial_port, magic)
                destination_path = unique_destination(
                    self.destination_directory, metadata.filename
                )
                part_path = destination_path.with_name(destination_path.name + ".part")
                self.emit(
                    "metadata",
                    filename=destination_path.name,
                    file_size=metadata.file_size,
                    sha256=metadata.sha256.hex(),
                    protocol=metadata.protocol,
                    ignored=ignored,
                    destination=str(destination_path),
                )

                digest = hashlib.sha256()
                start_time = time.monotonic()
                stall_monitor = ReceiveStallMonitor(start_time)
                active_rate_meter = ActiveRateMeter()
                last_progress_time = 0.0
                with part_path.open("wb") as destination_file:
                    while received < metadata.file_size:
                        self.ensure_active()
                        chunk = serial_port.read(
                            min(TRANSFER_CHUNK_SIZE, metadata.file_size - received)
                        )
                        if not chunk:
                            now = time.monotonic()
                            stalled_duration = stall_monitor.observe_idle(now)
                            if stalled_duration is not None:
                                self.emit(
                                    "stalled",
                                    duration=stalled_duration,
                                    message=(
                                        f"已连续 {int(stalled_duration)} 秒无新数据，"
                                        "正在等待五跳链路重传恢复…"
                                    ),
                                )
                            if now - stall_monitor.last_data_at >= RECEIVE_STALL_FINALIZE:
                                self.emit(
                                    "phase",
                                    message="等待 30 秒仍无新数据，停止等待并保存当前已接收内容",
                                )
                                break
                            continue

                        now = time.monotonic()
                        stalled_duration = stall_monitor.observe_data(now)
                        if stalled_duration is not None:
                            self.emit(
                                "resumed",
                                duration=stalled_duration,
                                message=(
                                    f"五跳链路已恢复，停顿 {int(stalled_duration)} 秒后继续接收"
                                ),
                            )
                        destination_file.write(chunk)
                        digest.update(chunk)
                        received += len(chunk)
                        active_rate_meter.observe(len(chunk), now)
                        if now - last_progress_time >= PROGRESS_INTERVAL or received == metadata.file_size:
                            elapsed = max(now - start_time, 0.001)
                            self.emit(
                                "progress",
                                transferred=received,
                                total=metadata.file_size,
                                speed_bps=(
                                    active_rate_meter.active_bps
                                    or received * 8 / elapsed
                                ),
                                average_bps=received * 8 / elapsed,
                                eta=0.0,
                            )
                            last_progress_time = now

                actual_digest = digest.digest()
                elapsed = max(time.monotonic() - start_time, 0.001)
                if received < metadata.file_size:
                    incomplete_path = self._preserve_part(
                        part_path, destination_path, "incomplete"
                    )
                    part_path = None
                    if incomplete_path is None:
                        raise ProtocolError("接收超时且没有收到可保存的文件数据")
                    self.emit(
                        "incomplete",
                        message=(
                            f"接收超时：已保留 {received}/{metadata.file_size} 字节，"
                            "文件可能缺少少量尾部数据"
                        ),
                        destination=str(incomplete_path),
                        sha256=actual_digest.hex(),
                        transferred=received,
                        total=metadata.file_size,
                        average_bps=received * 8 / elapsed,
                        active_bps=active_rate_meter.active_bps,
                    )
                    return
                if actual_digest != metadata.sha256:
                    failed_path = self._preserve_part(
                        part_path, destination_path, "checksum_failed"
                    )
                    part_path = None
                    if failed_path is None:
                        raise ProtocolError("SHA-256 校验失败，且没有可保存的数据")
                    self.emit(
                        "checksum_failed",
                        message="SHA-256 校验失败，但文件已保留，可直接尝试播放或打开",
                        destination=str(failed_path),
                        sha256=actual_digest.hex(),
                        transferred=received,
                        total=metadata.file_size,
                        average_bps=received * 8 / elapsed,
                        active_bps=active_rate_meter.active_bps,
                    )
                    return

                os.replace(part_path, destination_path)
                part_path = None
                self.emit(
                    "complete",
                    message="接收完成，SHA-256 校验通过",
                    destination=str(destination_path),
                    sha256=actual_digest.hex(),
                    average_bps=metadata.file_size * 8 / elapsed,
                    active_bps=active_rate_meter.active_bps,
                )
        except TransferCancelled as error:
            saved_path = self._preserve_part(part_path, destination_path, "interrupted")
            message = str(error)
            if saved_path is not None:
                message += f"；已接收内容保存在：{saved_path}"
            self.emit("cancelled", message=message, destination=str(saved_path or ""))
        except Exception as error:
            saved_path = self._preserve_part(part_path, destination_path, "interrupted")
            message = str(error)
            if saved_path is not None:
                message += f"；已接收内容保存在：{saved_path}"
            self.emit("error", message=message, destination=str(saved_path or ""))


class LinkTestSenderWorker(SenderWorker):
    def __init__(
        self,
        event_queue: queue.Queue[dict[str, Any]],
        port: str,
        rate_kbps: int,
        baudrate: int,
        packet: LinkTestPacket | None = None,
    ) -> None:
        super().__init__(event_queue, port, ".", rate_kbps, "V5HP", baudrate)
        self.packet = packet or create_link_test_packet()

    def run(self) -> None:
        try:
            if self.rate_kbps <= 0:
                raise ValueError("链路测试限速必须大于 0 kbps")
            encoded = encode_link_test_packet(self.packet)
            with serial.Serial(
                self.port,
                self.baudrate,
                timeout=0.2,
                write_timeout=5,
                rtscts=False,
                dsrdtr=False,
            ) as serial_port:
                serial_port.reset_input_buffer()
                sent = 0
                start_time = time.monotonic()
                next_send_at = start_time
                while sent < len(encoded):
                    self.ensure_active()
                    self._wait_for_flow_control(serial_port)
                    chunk = encoded[sent : sent + TRANSFER_CHUNK_SIZE]
                    now = time.monotonic()
                    send_started_at = max(next_send_at, now)
                    if send_started_at > now:
                        time.sleep(send_started_at - now)
                    self._write_all(serial_port, chunk)
                    sent += len(chunk)
                    rate_bps = max(self.flow_rate_controller.current_bps, 1)
                    next_send_at = send_started_at + len(chunk) * 8 / rate_bps
                serial_port.flush()

            elapsed = max(time.monotonic() - start_time, 0.001)
            self.emit(
                "link_test_sent",
                test_id=self.packet.test_id,
                payload_len=len(self.packet.payload),
                sha256=self.packet.sha256.hex(),
                average_bps=len(self.packet.payload) * 8 / elapsed,
                message="测速包已写入 Node1，请查看 Node6 接收端有效吞吐结果",
            )
        except TransferCancelled as error:
            self.emit("cancelled", message=str(error))
        except Exception as error:
            self.emit("error", message=str(error))


class ChainMonitorWorker(BaseWorker):
    NODE_NAMES = ("Node1", "R1", "R2", "R3", "R4", "Node6")
    MODE_NODE_IDS = {
        CHAIN_MODE_FIVE_HOP: (0, 1, 2, 3, 4, 5),
        CHAIN_MODE_TWO_HOP: (0, 1, 5),
    }

    def __init__(
        self,
        event_queue: queue.Queue[dict[str, Any]],
        port: str,
        baudrate: int,
        mode: str = CHAIN_MODE_FIVE_HOP,
    ) -> None:
        super().__init__(event_queue)
        if mode not in self.MODE_NODE_IDS:
            raise ValueError(f"不支持的链路模式：{mode}")
        self.port = port
        self.baudrate = baudrate
        self.mode = mode

    @classmethod
    def node_ids(cls, mode: str = CHAIN_MODE_FIVE_HOP) -> tuple[int, ...]:
        try:
            return cls.MODE_NODE_IDS[mode]
        except KeyError as error:
            raise ValueError(f"不支持的链路模式：{mode}") from error

    @classmethod
    def describe_result(
        cls,
        reports: dict[int, int],
        mode: str = CHAIN_MODE_FIVE_HOP,
    ) -> str:
        required_node_ids = cls.node_ids(mode)
        if 0 not in reports:
            return "Node1 未响应，请检查串口、Node1 固件或复位状态"
        for index, node_id in enumerate(required_node_ids[1:], start=1):
            if node_id not in reports:
                previous = cls.NODE_NAMES[required_node_ids[index - 1]]
                current = cls.NODE_NAMES[node_id]
                return f"探测停在 {previous}，请检查 {previous} → {current} 链路"
        return "、".join(cls.NODE_NAMES[node_id] for node_id in required_node_ids) + " 全部响应"

    @classmethod
    def is_complete(
        cls,
        reports: dict[int, int],
        mode: str = CHAIN_MODE_FIVE_HOP,
    ) -> bool:
        return all(node_id in reports for node_id in cls.node_ids(mode))

    def run(self) -> None:
        try:
            chain_ready = False
            with serial.Serial(
                self.port,
                self.baudrate,
                timeout=0.1,
                write_timeout=2,
                rtscts=False,
                dsrdtr=False,
            ) as serial_port:
                serial_port.reset_input_buffer()
                self.emit("phase", message=f"已打开 {self.port}，开始逐跳建链探测…")
                stable_cycles = 0
                while True:
                    self.ensure_active()
                    probe_id = secrets.randbits(32)
                    encoded_probe = encode_chain_probe_command(probe_id)
                    serial_port.write(encoded_probe)
                    serial_port.flush()
                    reports: dict[int, int] = {}
                    response_buffer = bytearray()
                    deadline = time.monotonic() + CHAIN_PROBE_RESPONSE_WINDOW
                    next_retry_at = time.monotonic() + CHAIN_PROBE_RETRY_INTERVAL
                    while time.monotonic() < deadline:
                        self.ensure_active()
                        now = time.monotonic()
                        if now >= next_retry_at and not self.is_complete(reports, self.mode):
                            serial_port.write(encoded_probe)
                            serial_port.flush()
                            next_retry_at = now + CHAIN_PROBE_RETRY_INTERVAL
                        waiting = serial_port.in_waiting
                        chunk = serial_port.read(waiting if waiting > 0 else 1)
                        if chunk:
                            response_buffer.extend(chunk)
                            if len(response_buffer) > 8192:
                                del response_buffer[:-4096]
                            for match in DIAG_REPORT_PATTERN.finditer(response_buffer):
                                if int(match.group(1)) != probe_id:
                                    continue
                                node_id = int(match.group(2))
                                if 0 <= node_id < len(self.NODE_NAMES):
                                    reports[node_id] = int(match.group(3))

                    all_ready = self.is_complete(reports, self.mode)
                    stable_cycles = stable_cycles + 1 if all_ready else 0
                    self.emit(
                        "chain_cycle",
                        probe_id=probe_id,
                        reports=reports,
                        stable_cycles=stable_cycles,
                        mode=self.mode,
                        mode_label=CHAIN_MODE_LABELS[self.mode],
                        message=self.describe_result(reports, self.mode),
                    )
                    if stable_cycles >= CHAIN_STABLE_CYCLES:
                        chain_ready = True
                        break
                    sleep_until = time.monotonic() + max(
                        CHAIN_PROBE_INTERVAL - CHAIN_PROBE_RESPONSE_WINDOW, 0
                    )
                    while time.monotonic() < sleep_until:
                        self.ensure_active()
                        time.sleep(0.02)
            if chain_ready:
                self.emit(
                    "chain_complete",
                    mode=self.mode,
                    mode_label=CHAIN_MODE_LABELS[self.mode],
                    message=(
                        f"{CHAIN_MODE_LABELS[self.mode]}连续 {CHAIN_STABLE_CYCLES} 次全部响应，"
                        "可以开始传输文件"
                    ),
                )
        except TransferCancelled as error:
            self.emit("cancelled", message=str(error))
        except Exception as error:
            self.emit("error", message=str(error))
