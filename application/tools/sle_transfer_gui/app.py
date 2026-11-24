from __future__ import annotations

import json
import os
import queue
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Any

from serial.tools import list_ports

from transfer import (
    CHAIN_MODE_FIVE_HOP,
    CHAIN_MODE_LABELS,
    PACING_MODE_DEFAULT,
    PACING_MODE_LABELS,
    PACING_MODE_PULSE_DRAIN,
    PACING_MODE_STABLE_260,
    ChainMonitorWorker,
    LinkTestSenderWorker,
    ReceiverWorker,
    SenderWorker,
)


APP_TITLE = os.environ.get("SLE_TRANSFER_APP_TITLE", "NearLink SLE 五跳文件透传平台")
SETTINGS_PATH = Path.home() / os.environ.get(
    "SLE_TRANSFER_SETTINGS_FILE", ".sle_transfer_gui.json"
)
DEFAULT_RATE_KBPS = int(os.environ.get("SLE_TRANSFER_DEFAULT_RATE_KBPS", "260"))
DEFAULT_PACING_MODE = os.environ.get(
    "SLE_TRANSFER_DEFAULT_PACING_MODE", PACING_MODE_DEFAULT
)
if DEFAULT_PACING_MODE not in PACING_MODE_LABELS:
    DEFAULT_PACING_MODE = PACING_MODE_DEFAULT
POLL_INTERVAL_MS = 80
ROLE_SEND = "send"
ROLE_RECEIVE = "receive"
ROLE_DEBUG = "debug"
ROLE_LABELS = {
    ROLE_SEND: "发送端（Node1 电脑）",
    ROLE_RECEIVE: "接收端（Node6 电脑）",
    ROLE_DEBUG: "调试模式（双页面）",
}
ROLE_HINTS = {
    ROLE_SEND: "当前电脑连接 Node1：发送文件并测试所选链路拓扑",
    ROLE_RECEIVE: "当前电脑连接 Node6：监听链路并保存接收文件",
    ROLE_DEBUG: "同时显示发送和接收页面，适合单机维护与调试",
}


def normalize_role(value: Any) -> str:
    return value if value in ROLE_LABELS else ROLE_DEBUG


def role_page_keys(role: str) -> tuple[str, ...]:
    normalized_role = normalize_role(role)
    if normalized_role == ROLE_SEND:
        return (ROLE_SEND,)
    if normalized_role == ROLE_RECEIVE:
        return (ROLE_RECEIVE,)
    return (ROLE_SEND, ROLE_RECEIVE)


def format_size(byte_count: int | float) -> str:
    value = float(byte_count)
    units = ("B", "KB", "MB", "GB", "TB")
    for unit in units:
        if abs(value) < 1024 or unit == units[-1]:
            return f"{value:.0f} {unit}" if unit == "B" else f"{value:.2f} {unit}"
        value /= 1024
    return f"{value:.2f} TB"


def format_speed(bits_per_second: float) -> str:
    if bits_per_second >= 1_000_000:
        return f"{bits_per_second / 1_000_000:.2f} Mbps"
    return f"{bits_per_second / 1000:.1f} kbps"


def format_duration(seconds: float) -> str:
    seconds_int = max(int(seconds + 0.5), 0)
    minutes, seconds_value = divmod(seconds_int, 60)
    hours, minutes_value = divmod(minutes, 60)
    if hours:
        return f"{hours:d}:{minutes_value:02d}:{seconds_value:02d}"
    return f"{minutes_value:02d}:{seconds_value:02d}"


def load_settings() -> dict[str, Any]:
    try:
        return json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_settings(settings: dict[str, Any]) -> None:
    try:
        SETTINGS_PATH.write_text(
            json.dumps(settings, ensure_ascii=False, indent=2), encoding="utf-8"
        )
    except OSError:
        pass


def available_ports() -> list[str]:
    ports = sorted(list_ports.comports(), key=lambda item: item.device)
    return [f"{item.device} — {item.description}" for item in ports]


def port_name(display_value: str) -> str:
    return display_value.split(" — ", 1)[0].strip()


class TransferPage(ttk.Frame):
    def __init__(self, master, settings: dict[str, Any], settings_key: str) -> None:
        super().__init__(master, padding=18)
        self.configure(padding=0)
        self.settings = settings
        self.settings_key = settings_key
        self.event_queue: queue.Queue[dict[str, Any]] = queue.Queue()
        self.worker: SenderWorker | ReceiverWorker | LinkTestSenderWorker | None = None
        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(
            value=str(settings.get(settings_key, {}).get("baudrate", 921600))
        )
        self.status_var = tk.StringVar(value="就绪")
        self.file_var = tk.StringVar(value="—")
        self.size_var = tk.StringVar(value="—")
        self.hash_var = tk.StringVar(value="—")
        self.speed_var = tk.StringVar(value="—")
        self.eta_var = tk.StringVar(value="—")
        self.progress_var = tk.DoubleVar(value=0)

        self.canvas = tk.Canvas(self, highlightthickness=0, borderwidth=0)
        self.scrollbar = ttk.Scrollbar(
            self, orient="vertical", command=self.canvas.yview
        )
        self.canvas.configure(yscrollcommand=self.scrollbar.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        self.scrollbar.pack(side="right", fill="y")
        self.content = ttk.Frame(self.canvas, padding=18)
        self.content_window = self.canvas.create_window(
            (0, 0), window=self.content, anchor="nw"
        )
        self.content.bind("<Configure>", self._update_scroll_region)
        self.canvas.bind("<Configure>", self._resize_content)
        self.canvas.bind("<Enter>", self._bind_mousewheel)
        self.canvas.bind("<Leave>", self._unbind_mousewheel)

    def _update_scroll_region(self, _event=None) -> None:
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))

    def _resize_content(self, event) -> None:
        self.canvas.itemconfigure(self.content_window, width=event.width)

    def _bind_mousewheel(self, _event=None) -> None:
        self.canvas.bind_all("<MouseWheel>", self._on_mousewheel)

    def _unbind_mousewheel(self, _event=None) -> None:
        self.canvas.unbind_all("<MouseWheel>")

    def _on_mousewheel(self, event) -> None:
        self.canvas.yview_scroll(int(-event.delta / 120), "units")

    def add_port_row(self, parent: ttk.Frame, row: int) -> None:
        ttk.Label(parent, text="串口").grid(row=row, column=0, sticky="w", padx=(0, 10), pady=6)
        self.port_combo = ttk.Combobox(
            parent, textvariable=self.port_var, state="readonly", width=42
        )
        self.port_combo.grid(row=row, column=1, sticky="ew", pady=6)
        ttk.Button(parent, text="刷新", command=self.refresh_ports).grid(
            row=row, column=2, padx=(8, 0), pady=6
        )
        self.baud_combo = ttk.Combobox(
            parent,
            textvariable=self.baud_var,
            values=("115200", "230400", "460800", "921600"),
            width=12,
        )
        self.baud_combo.grid(row=row, column=3, padx=(12, 0), pady=6)

    def get_baudrate(self) -> int:
        try:
            baudrate = int(self.baud_var.get().strip())
        except ValueError as error:
            raise ValueError("波特率必须是整数") from error
        if not 1200 <= baudrate <= 4_000_000:
            raise ValueError("波特率必须在 1200 到 4000000 之间")
        return baudrate

    def saved_baudrate(self) -> int:
        try:
            return self.get_baudrate()
        except ValueError:
            return 921600

    def log_nonstandard_baudrate(self, baudrate: int) -> None:
        if baudrate != 921600:
            self.log(
                f"提示：当前波特率为 {baudrate}，请确认开发板固件 UART 配置完全一致。"
            )

    def refresh_ports(self) -> None:
        values = available_ports()
        current_port = port_name(self.port_var.get())
        self.port_combo["values"] = values
        for value in values:
            if port_name(value) == current_port:
                self.port_var.set(value)
                break
        else:
            preferred = self.settings.get(self.settings_key, {}).get("port", "")
            selected = next(
                (value for value in values if port_name(value) == preferred),
                values[0] if values else "",
            )
            self.port_var.set(selected)

    def add_transfer_status(self, parent: ttk.Frame, row: int) -> None:
        status_frame = ttk.LabelFrame(parent, text="传输状态", padding=14)
        status_frame.grid(row=row, column=0, columnspan=4, sticky="nsew", pady=(14, 0))
        status_frame.columnconfigure(1, weight=1)

        fields = (
            ("文件", self.file_var),
            ("大小", self.size_var),
            ("SHA-256", self.hash_var),
        )
        for field_row, (label, variable) in enumerate(fields):
            ttk.Label(status_frame, text=label).grid(
                row=field_row, column=0, sticky="nw", padx=(0, 12), pady=4
            )
            ttk.Label(status_frame, textvariable=variable, wraplength=650).grid(
                row=field_row, column=1, columnspan=3, sticky="w", pady=4
            )

        self.progress_bar = ttk.Progressbar(
            status_frame, variable=self.progress_var, maximum=100, mode="determinate"
        )
        self.progress_bar.grid(row=3, column=0, columnspan=4, sticky="ew", pady=(12, 6))
        ttk.Label(status_frame, textvariable=self.status_var).grid(
            row=4, column=0, columnspan=2, sticky="w", pady=4
        )
        ttk.Label(status_frame, text="速度：").grid(row=4, column=2, sticky="e", pady=4)
        ttk.Label(status_frame, textvariable=self.speed_var).grid(
            row=4, column=3, sticky="w", pady=4
        )
        ttk.Label(status_frame, text="预计剩余：").grid(row=5, column=2, sticky="e", pady=4)
        ttk.Label(status_frame, textvariable=self.eta_var).grid(
            row=5, column=3, sticky="w", pady=4
        )

        self.log_text = tk.Text(
            status_frame,
            height=7,
            state="disabled",
            wrap="word",
            bg="#f7f8fa",
            relief="flat",
            padx=8,
            pady=8,
        )
        self.log_text.grid(row=6, column=0, columnspan=4, sticky="nsew", pady=(10, 0))

    def log(self, message: str) -> None:
        self.log_text.configure(state="normal")
        self.log_text.insert("end", message.rstrip() + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def reset_status(self) -> None:
        self.progress_var.set(0)
        self.speed_var.set("—")
        self.eta_var.set("—")
        self.hash_var.set("—")

    def cancel_transfer(self) -> None:
        if self.worker and self.worker.is_alive():
            self.status_var.set("正在停止…")
            self.worker.cancel()

    def poll_events(self) -> None:
        try:
            while True:
                self.handle_event(self.event_queue.get_nowait())
        except queue.Empty:
            pass
        self.after(POLL_INTERVAL_MS, self.poll_events)

    def handle_event(self, event: dict[str, Any]) -> None:
        event_type = event["type"]
        if event_type == "phase":
            self.status_var.set(event["message"])
            self.log(event["message"])
        elif event_type == "progress":
            total = max(event["total"], 1)
            self.progress_var.set(event["transferred"] * 100 / total)
            self.speed_var.set(format_speed(event["speed_bps"]))
            self.eta_var.set(format_duration(event.get("eta", 0)))
            self.status_var.set(
                f"{format_size(event['transferred'])} / {format_size(event['total'])}"
            )
        elif event_type == "ignored":
            self.log(f"已忽略 {event['count']} 字节非文件数据，继续等待文件头。")
        elif event_type == "flow_rate":
            self.log(
                f"自适应发送速率：{format_speed(event['rate_bps'])} "
                f"/ 上限 {format_speed(event['limit_bps'])}"
            )
        elif event_type == "pacing_profile":
            self.log(event["message"])
        elif event_type == "path_status":
            level_text = {
                "normal": "正常",
                "mild": "轻度积压",
                "severe": "排空保护",
            }.get(event.get("level"), "未知")
            self.log(
                f"全链路状态：{level_text}；{event['reason']}；"
                f"窗口 {event['unacked']}/{event['cwnd']}，RTO {event['rto_ms']} ms"
            )
        elif event_type == "stalled":
            self.status_var.set("链路暂时阻塞，正在等待恢复…")
            self.speed_var.set("0.0 kbps")
            self.eta_var.set("等待恢复")
            self.log(event["message"])
        elif event_type == "resumed":
            self.status_var.set("链路已恢复，继续接收")
            self.log(event["message"])
        elif event_type == "error":
            self.status_var.set("传输失败")
            self.log("错误：" + event["message"])
            self.set_running(False)
            messagebox.showerror(APP_TITLE, event["message"])
        elif event_type == "cancelled":
            self.status_var.set("已停止")
            self.log(event["message"])
            self.set_running(False)
        elif event_type == "complete":
            self.progress_var.set(100)
            self.hash_var.set(event["sha256"])
            display_bps = event.get("active_bps") or event["average_bps"]
            self.speed_var.set(format_speed(display_bps))
            self.eta_var.set("00:00")
            self.status_var.set(event["message"])
            self.log(event["message"])
            if event.get("active_bps"):
                duty_cycle = min(event["average_bps"] / event["active_bps"], 1.0)
                self.log(
                    f"活动时段速率：{format_speed(event['active_bps'])}；"
                    f"全程平均：{format_speed(event['average_bps'])}；"
                    f"发送占空比：{duty_cycle * 100:.1f}%"
                )
            self.set_running(False)
        elif event_type in ("incomplete", "checksum_failed"):
            total = max(event["total"], 1)
            self.progress_var.set(event["transferred"] * 100 / total)
            self.hash_var.set(event["sha256"])
            self.speed_var.set(format_speed(event["average_bps"]))
            self.eta_var.set("00:00")
            self.status_var.set(event["message"])
            self.log(event["message"])
            self.log("已保留文件：" + event["destination"])
            self.set_running(False)

    def set_running(self, running: bool) -> None:
        raise NotImplementedError


class SendPage(TransferPage):
    def __init__(self, master, settings: dict[str, Any]) -> None:
        super().__init__(master, settings, "send")
        self.source_var = tk.StringVar(value=settings.get("send", {}).get("source", ""))
        self.rate_var = tk.IntVar(
            value=settings.get("send", {}).get("rate_kbps", DEFAULT_RATE_KBPS)
        )
        saved_protocol = settings.get("send", {}).get("protocol", "V5HP")
        if saved_protocol not in ("V5HP", "V5H2"):
            saved_protocol = "V5HP"
        self.protocol_var = tk.StringVar(
            value=saved_protocol
        )
        saved_chain_mode = settings.get("send", {}).get("chain_mode", CHAIN_MODE_FIVE_HOP)
        if saved_chain_mode not in CHAIN_MODE_LABELS:
            saved_chain_mode = CHAIN_MODE_FIVE_HOP
        self.chain_mode_var = tk.StringVar(value=CHAIN_MODE_LABELS[saved_chain_mode])
        saved_pacing_mode = settings.get("send", {}).get(
            "pacing_mode", DEFAULT_PACING_MODE
        )
        if saved_pacing_mode not in PACING_MODE_LABELS:
            saved_pacing_mode = PACING_MODE_DEFAULT
        self.pacing_mode_var = tk.StringVar(
            value=PACING_MODE_LABELS[saved_pacing_mode]
        )
        self.link_status_var = tk.StringVar(value="● 未测试")
        self.pending_test_id: int | None = None
        self.receiver_is_active = lambda: False
        self.chain_monitor_active = False
        self.chain_node_labels: dict[int, tk.Label] = {}

        self.content.columnconfigure(0, weight=1)
        title = ttk.Label(self.content, text="发送文件", style="Title.TLabel")
        title.grid(row=0, column=0, sticky="w", pady=(0, 14))

        form = ttk.LabelFrame(self.content, text="发送设置", padding=14)
        form.grid(row=1, column=0, sticky="nsew")
        form.columnconfigure(1, weight=1)
        self.add_port_row(form, 0)

        ttk.Label(form, text="文件").grid(row=1, column=0, sticky="w", padx=(0, 10), pady=6)
        self.source_entry = ttk.Entry(form, textvariable=self.source_var)
        self.source_entry.grid(row=1, column=1, columnspan=2, sticky="ew", pady=6)
        ttk.Button(form, text="选择文件", command=self.choose_file).grid(
            row=1, column=3, padx=(8, 0), pady=6
        )

        ttk.Label(form, text="限速").grid(row=2, column=0, sticky="w", padx=(0, 10), pady=6)
        self.rate_spin = ttk.Spinbox(
            form, from_=10, to=800, increment=10, textvariable=self.rate_var, width=12
        )
        self.rate_spin.grid(row=2, column=1, sticky="w", pady=6)
        ttk.Label(form, text="kbps").grid(row=2, column=2, sticky="w", pady=6)

        ttk.Label(form, text="电脑端协议").grid(
            row=3, column=0, sticky="w", padx=(0, 10), pady=6
        )
        self.protocol_combo = ttk.Combobox(
            form,
            textvariable=self.protocol_var,
            values=("V5HP", "V5H2"),
            state="readonly",
            width=16,
        )
        self.protocol_combo.grid(row=3, column=1, sticky="w", pady=6)
        ttk.Label(form, text="V5HP 兼容旧脚本；V5H2 自动携带文件名").grid(
            row=3, column=2, columnspan=2, sticky="w", pady=6
        )

        ttk.Label(form, text="链路拓扑").grid(
            row=4, column=0, sticky="w", padx=(0, 10), pady=6
        )
        self.chain_mode_combo = ttk.Combobox(
            form,
            textvariable=self.chain_mode_var,
            values=tuple(CHAIN_MODE_LABELS.values()),
            state="readonly",
            width=16,
        )
        self.chain_mode_combo.grid(row=4, column=1, sticky="w", pady=6)
        self.chain_mode_combo.bind("<<ComboboxSelected>>", self.on_chain_mode_changed)
        ttk.Label(form, text="两跳模式只等待 Node1、R1、Node6").grid(
            row=4, column=2, columnspan=2, sticky="w", pady=6
        )

        ttk.Label(form, text="发送节奏").grid(
            row=5, column=0, sticky="w", padx=(0, 10), pady=6
        )
        self.pacing_mode_combo = ttk.Combobox(
            form,
            textvariable=self.pacing_mode_var,
            values=tuple(PACING_MODE_LABELS.values()),
            state="readonly",
            width=22,
        )
        self.pacing_mode_combo.grid(row=5, column=1, sticky="w", pady=6)
        ttk.Label(form, text="稳定260模式依据Node1及R1-R4队列状态闭环控制").grid(
            row=5, column=2, columnspan=2, sticky="w", pady=6
        )

        button_frame = ttk.Frame(form)
        button_frame.grid(row=6, column=0, columnspan=4, sticky="w", pady=(12, 0))
        self.test_button = ttk.Button(
            button_frame, text="开始建链", command=self.start_link_test
        )
        self.test_button.pack(side="left")
        self.speed_test_button = ttk.Button(
            button_frame, text="链路速率测速", command=self.start_speed_test
        )
        self.speed_test_button.pack(side="left", padx=(8, 0))
        self.start_button = ttk.Button(button_frame, text="开始发送", command=self.start_send)
        self.start_button.pack(side="left", padx=(8, 0))
        self.cancel_button = ttk.Button(
            button_frame, text="取消", command=self.cancel_transfer, state="disabled"
        )
        self.cancel_button.pack(side="left", padx=(8, 0))
        self.link_status_label = tk.Label(
            button_frame,
            textvariable=self.link_status_var,
            fg="#666666",
            font=("Microsoft YaHei UI", 9, "bold"),
        )
        self.link_status_label.pack(side="left", padx=(16, 0))

        self.chain_frame = ttk.LabelFrame(form, text="逐跳状态", padding=(10, 8))
        self.chain_frame.grid(row=7, column=0, columnspan=4, sticky="ew", pady=(10, 0))
        self.rebuild_chain_nodes()

        self.add_transfer_status(form, 8)
        self.refresh_ports()
        self.poll_events()

    def selected_chain_mode(self) -> str:
        selected_label = self.chain_mode_var.get()
        return next(
            (mode for mode, label in CHAIN_MODE_LABELS.items() if label == selected_label),
            CHAIN_MODE_FIVE_HOP,
        )

    def selected_pacing_mode(self) -> str:
        selected_label = self.pacing_mode_var.get()
        return next(
            (
                mode
                for mode, label in PACING_MODE_LABELS.items()
                if label == selected_label
            ),
            PACING_MODE_DEFAULT,
        )

    def rebuild_chain_nodes(self) -> None:
        for label in self.chain_node_labels.values():
            label.destroy()
        self.chain_node_labels.clear()
        for index, node_id in enumerate(
            ChainMonitorWorker.node_ids(self.selected_chain_mode())
        ):
            node_name = ChainMonitorWorker.NODE_NAMES[node_id]
            label = tk.Label(
                self.chain_frame,
                text=f"● {node_name}",
                fg="#888888",
                font=("Microsoft YaHei UI", 9, "bold"),
            )
            label.pack(side="left", padx=(0 if index == 0 else 14, 0))
            self.chain_node_labels[node_id] = label

    def on_chain_mode_changed(self, _event=None) -> None:
        self.rebuild_chain_nodes()
        self.link_status_var.set("● 未测试")
        self.link_status_label.configure(fg="#666666")

    def start_link_test(self) -> None:
        if self.chain_monitor_active and self.worker and self.worker.is_alive():
            self.status_var.set("正在停止建链探测…")
            self.worker.cancel()
            return
        selected_port = port_name(self.port_var.get())
        if not selected_port:
            messagebox.showwarning(APP_TITLE, "请选择 Node1 串口")
            return
        try:
            baudrate = self.get_baudrate()
        except ValueError as error:
            messagebox.showwarning(APP_TITLE, str(error))
            return

        self.link_status_var.set("● 正在逐跳建链")
        self.link_status_label.configure(fg="#b26a00")
        for label in self.chain_node_labels.values():
            label.configure(fg="#888888")
        chain_mode = self.selected_chain_mode()
        self.log(
            f"启动{CHAIN_MODE_LABELS[chain_mode]}：{selected_port} @ {baudrate}"
        )
        self.log_nonstandard_baudrate(baudrate)
        self.worker = ChainMonitorWorker(
            self.event_queue,
            selected_port,
            baudrate,
            chain_mode,
        )
        self.chain_monitor_active = True
        self.set_running(True)
        self.worker.start()

    def update_chain_nodes(self, reports: dict[int, int]) -> None:
        required_node_ids = ChainMonitorWorker.node_ids(self.selected_chain_mode())
        first_missing = next((node_id for node_id in required_node_ids if node_id not in reports), None)
        for node_id, label in self.chain_node_labels.items():
            if node_id in reports:
                flags = reports[node_id]
                downstream_ready = node_id == 5 or bool(flags & 0x02 and flags & 0x04)
                label.configure(fg="#14804a" if downstream_ready else "#b26a00")
            elif node_id == first_missing:
                label.configure(fg="#b42318")
            else:
                label.configure(fg="#888888")

    def handle_link_test_result(self, event: dict[str, Any]) -> None:
        if not event.get("success"):
            self.link_status_var.set("● 测试失败")
            self.link_status_label.configure(fg="#b42318")
            self.log("链路测试失败：" + event["message"])
            return
        if self.pending_test_id == event.get("test_id"):
            self.pending_test_id = None
            self.link_status_var.set("● 链路通过")
            self.link_status_label.configure(fg="#14804a")
            self.speed_var.set(format_speed(event.get("average_bps", 0)))
            self.log("本机接收端确认：" + event["message"])

    def choose_file(self) -> None:
        filename = filedialog.askopenfilename(title="选择要发送的文件")
        if filename:
            self.source_var.set(filename)

    def start_speed_test(self) -> None:
        selected_port = port_name(self.port_var.get())
        if not selected_port:
            messagebox.showwarning(APP_TITLE, "请选择 Node1 串口")
            return
        try:
            rate_kbps = int(self.rate_var.get())
            baudrate = self.get_baudrate()
        except (TypeError, ValueError):
            messagebox.showwarning(APP_TITLE, "请输入有效的测速上限和波特率")
            return

        self.reset_status()
        worker = LinkTestSenderWorker(
            self.event_queue,
            selected_port,
            rate_kbps,
            baudrate,
        )
        self.pending_test_id = worker.packet.test_id
        self.file_var.set("随机链路测速数据")
        self.size_var.set(format_size(len(worker.packet.payload)))
        self.hash_var.set(worker.packet.sha256.hex())
        self.status_var.set("正在发送链路测速数据…")
        self.log(
            f"链路速率测速：{selected_port} @ {baudrate}，上限 {rate_kbps} kbps；"
            "结果为端到端有效吞吐，不是芯片标称空口速率"
        )
        self.log_nonstandard_baudrate(baudrate)
        self.worker = worker
        self.set_running(True)
        self.worker.start()

    def start_send(self) -> None:
        selected_port = port_name(self.port_var.get())
        source_path = Path(self.source_var.get().strip())
        if not selected_port:
            messagebox.showwarning(APP_TITLE, "请选择 Node1 串口")
            return
        if not source_path.is_file():
            messagebox.showwarning(APP_TITLE, "请选择有效的发送文件")
            return
        try:
            rate_kbps = int(self.rate_var.get())
            baudrate = self.get_baudrate()
        except (TypeError, ValueError):
            messagebox.showwarning(APP_TITLE, "请输入有效的发送限速和波特率")
            return

        self.reset_status()
        self.file_var.set(source_path.name)
        self.size_var.set(format_size(source_path.stat().st_size))
        self.status_var.set("正在启动发送任务…")
        protocol = self.protocol_var.get()
        pacing_mode = self.selected_pacing_mode()
        if pacing_mode == PACING_MODE_STABLE_260 and rate_kbps != 260:
            rate_kbps = 260
            self.rate_var.set(rate_kbps)
            self.log("SLE闭环稳定模式已将目标速率固定为 260 kbps")
        elif pacing_mode == PACING_MODE_PULSE_DRAIN and rate_kbps < 260:
            rate_kbps = 260
            self.rate_var.set(rate_kbps)
            self.log("260峰值排空模式的活动发送速率已自动提升到 260 kbps")
        self.log(
            f"发送：{source_path} → {selected_port}，限速 {rate_kbps} kbps，"
            f"协议 {protocol}，节奏 {PACING_MODE_LABELS[pacing_mode]}"
        )
        self.log_nonstandard_baudrate(baudrate)
        self.worker = SenderWorker(
            self.event_queue,
            selected_port,
            str(source_path),
            rate_kbps,
            protocol,
            baudrate,
            pacing_mode,
            self.selected_chain_mode(),
        )
        self.set_running(True)
        self.worker.start()

    def handle_event(self, event: dict[str, Any]) -> None:
        if event["type"] == "chain_cycle":
            self.update_chain_nodes(event["reports"])
            self.status_var.set(event["message"])
            self.link_status_var.set(f"● 建链中（全通 {event['stable_cycles']}/3）")
            self.link_status_label.configure(fg="#b26a00")
            self.log(event["message"])
            return
        if event["type"] == "chain_complete":
            self.chain_monitor_active = False
            self.link_status_var.set(f"● {event['mode_label']}已通")
            self.link_status_label.configure(fg="#14804a")
            self.status_var.set(event["message"])
            self.log(event["message"] + "；建链串口已释放。")
            self.set_running(False)
            return
        if event["type"] in ("cancelled", "error") and self.chain_monitor_active:
            self.chain_monitor_active = False
        if event["type"] == "metadata":
            self.file_var.set(event["filename"])
            self.size_var.set(format_size(event["file_size"]))
            self.hash_var.set(event["sha256"])
            self.log("SHA-256：" + event["sha256"])
            self.log("电脑端协议：" + event["protocol"])
            return
        if event["type"] == "link_test_sent":
            self.status_var.set("测速包已写入 Node1；端到端结果以 Node6 为准")
            self.progress_var.set(100)
            self.speed_var.set("Node1写入 " + format_speed(event["average_bps"]))
            self.log(event["message"])
            if not self.receiver_is_active():
                self.link_status_var.set("● 请查看接收端")
                self.link_status_label.configure(fg="#b26a00")
            self.set_running(False)
            return
        super().handle_event(event)

    def set_running(self, running: bool) -> None:
        state = "disabled" if running else "normal"
        self.start_button.configure(state=state)
        self.speed_test_button.configure(state=state)
        self.source_entry.configure(state=state)
        self.rate_spin.configure(state=state)
        self.protocol_combo.configure(state="disabled" if running else "readonly")
        self.chain_mode_combo.configure(state="disabled" if running else "readonly")
        self.pacing_mode_combo.configure(state="disabled" if running else "readonly")
        self.baud_combo.configure(state="disabled" if running else "normal")
        self.test_button.configure(
            state="normal" if self.chain_monitor_active else state,
            text="停止建链" if self.chain_monitor_active else "开始建链",
        )
        self.port_combo.configure(state="disabled" if running else "readonly")
        self.cancel_button.configure(state="normal" if running else "disabled")

    def settings_snapshot(self) -> dict[str, Any]:
        return {
            "port": port_name(self.port_var.get()),
            "source": self.source_var.get(),
            "rate_kbps": int(self.rate_var.get() or DEFAULT_RATE_KBPS),
            "protocol": self.protocol_var.get(),
            "chain_mode": self.selected_chain_mode(),
            "pacing_mode": self.selected_pacing_mode(),
            "baudrate": self.saved_baudrate(),
        }


class ReceivePage(TransferPage):
    def __init__(self, master, settings: dict[str, Any], link_test_callback) -> None:
        super().__init__(master, settings, "receive")
        self.link_test_callback = link_test_callback
        default_directory = settings.get("receive", {}).get(
            "directory", str(Path.home() / "Downloads")
        )
        self.directory_var = tk.StringVar(value=default_directory)
        self.legacy_filename_var = tk.StringVar(
            value=settings.get("receive", {}).get("legacy_filename", "received.mp4")
        )
        self.link_status_var = tk.StringVar(value="● 等待测试")
        self.last_saved_path: Path | None = None

        self.content.columnconfigure(0, weight=1)
        ttk.Label(self.content, text="接收文件", style="Title.TLabel").grid(
            row=0, column=0, sticky="w", pady=(0, 14)
        )

        form = ttk.LabelFrame(self.content, text="接收设置", padding=14)
        form.grid(row=1, column=0, sticky="nsew")
        form.columnconfigure(1, weight=1)
        self.add_port_row(form, 0)

        ttk.Label(form, text="保存目录").grid(
            row=1, column=0, sticky="w", padx=(0, 10), pady=6
        )
        self.directory_entry = ttk.Entry(form, textvariable=self.directory_var)
        self.directory_entry.grid(row=1, column=1, columnspan=2, sticky="ew", pady=6)
        ttk.Button(form, text="选择目录", command=self.choose_directory).grid(
            row=1, column=3, padx=(8, 0), pady=6
        )

        ttk.Label(form, text="V5HP文件名").grid(
            row=2, column=0, sticky="w", padx=(0, 10), pady=6
        )
        self.legacy_filename_entry = ttk.Entry(
            form, textvariable=self.legacy_filename_var
        )
        self.legacy_filename_entry.grid(row=2, column=1, columnspan=2, sticky="ew", pady=6)
        ttk.Label(form, text="V5H2 会自动使用原文件名").grid(
            row=2, column=3, sticky="w", padx=(8, 0), pady=6
        )

        button_frame = ttk.Frame(form)
        button_frame.grid(row=3, column=0, columnspan=4, sticky="w", pady=(12, 0))
        self.start_button = ttk.Button(
            button_frame, text="开始监听", command=self.start_receive
        )
        self.start_button.pack(side="left")
        self.cancel_button = ttk.Button(
            button_frame, text="停止监听", command=self.cancel_transfer, state="disabled"
        )
        self.cancel_button.pack(side="left", padx=(8, 0))
        self.open_file_button = ttk.Button(
            button_frame, text="打开文件", command=self.open_saved_file, state="disabled"
        )
        self.open_file_button.pack(side="left", padx=(8, 0))
        self.open_folder_button = ttk.Button(
            button_frame, text="打开目录", command=self.open_saved_folder, state="disabled"
        )
        self.open_folder_button.pack(side="left", padx=(8, 0))
        self.link_status_label = tk.Label(
            button_frame,
            textvariable=self.link_status_var,
            fg="#666666",
            font=("Microsoft YaHei UI", 9, "bold"),
        )
        self.link_status_label.pack(side="left", padx=(16, 0))

        self.add_transfer_status(form, 4)
        self.refresh_ports()
        self.poll_events()

    def is_listening(self) -> bool:
        return bool(self.worker and self.worker.is_alive())

    def choose_directory(self) -> None:
        directory = filedialog.askdirectory(title="选择接收文件保存目录")
        if directory:
            self.directory_var.set(directory)

    def open_saved_file(self) -> None:
        if self.last_saved_path and self.last_saved_path.exists():
            os.startfile(self.last_saved_path)
        else:
            messagebox.showwarning(APP_TITLE, "当前没有可打开的已保存文件")

    def open_saved_folder(self) -> None:
        if self.last_saved_path:
            os.startfile(self.last_saved_path.parent)
        else:
            messagebox.showwarning(APP_TITLE, "当前没有已保存文件目录")

    def remember_saved_file(self, path_value: str) -> None:
        if not path_value:
            return
        self.last_saved_path = Path(path_value)
        self.open_file_button.configure(state="normal")
        self.open_folder_button.configure(state="normal")

    def start_receive(self) -> None:
        selected_port = port_name(self.port_var.get())
        directory = Path(self.directory_var.get().strip())
        if not selected_port:
            messagebox.showwarning(APP_TITLE, "请选择 Node6 串口")
            return
        if not self.directory_var.get().strip():
            messagebox.showwarning(APP_TITLE, "请选择文件保存目录")
            return
        try:
            baudrate = self.get_baudrate()
        except ValueError as error:
            messagebox.showwarning(APP_TITLE, str(error))
            return

        self.reset_status()
        self.file_var.set("等待文件头…")
        self.size_var.set("—")
        self.status_var.set("正在启动监听…")
        self.log(f"监听：{selected_port}，保存目录：{directory}")
        self.log_nonstandard_baudrate(baudrate)
        self.worker = ReceiverWorker(
            self.event_queue,
            selected_port,
            str(directory),
            self.legacy_filename_var.get().strip() or "received.mp4",
            baudrate,
        )
        self.set_running(True)
        self.worker.start()

    def handle_event(self, event: dict[str, Any]) -> None:
        if event["type"] == "metadata":
            self.file_var.set(event["filename"])
            self.size_var.set(format_size(event["file_size"]))
            self.hash_var.set(event["sha256"])
            self.log(
                f"检测到 {event['protocol']} 文件：{event['filename']}，"
                f"忽略启动字节 {event['ignored']}"
            )
            self.log("目标路径：" + event["destination"])
            return
        if event["type"] == "link_test_result":
            if event["success"]:
                self.link_status_var.set("● 链路通过")
                self.link_status_label.configure(fg="#14804a")
                self.file_var.set("随机链路测速数据")
                self.size_var.set(format_size(event["payload_len"]))
                self.hash_var.set(event["sha256"])
                self.progress_var.set(100)
                self.speed_var.set(format_speed(event.get("average_bps", 0)))
                self.status_var.set("链路速率测速完成")
                self.log(event["message"])
            else:
                self.link_status_var.set("● 测试失败")
                self.link_status_label.configure(fg="#b42318")
                self.log("链路测试失败：" + event["message"])
            self.link_test_callback(event)
            return
        if event["type"] == "complete":
            self.log("保存位置：" + event["destination"])
            self.remember_saved_file(event["destination"])
        elif event["type"] in ("incomplete", "checksum_failed"):
            self.remember_saved_file(event["destination"])
        elif event["type"] in ("cancelled", "error"):
            self.remember_saved_file(event.get("destination", ""))
        super().handle_event(event)

    def set_running(self, running: bool) -> None:
        state = "disabled" if running else "normal"
        self.start_button.configure(state=state)
        self.directory_entry.configure(state=state)
        self.legacy_filename_entry.configure(state=state)
        self.baud_combo.configure(state="disabled" if running else "normal")
        self.port_combo.configure(state="disabled" if running else "readonly")
        self.cancel_button.configure(state="normal" if running else "disabled")

    def settings_snapshot(self) -> dict[str, Any]:
        return {
            "port": port_name(self.port_var.get()),
            "directory": self.directory_var.get(),
            "legacy_filename": self.legacy_filename_var.get(),
            "baudrate": self.saved_baudrate(),
        }


class TransferApplication(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title(APP_TITLE)
        self.geometry("980x780")
        self.minsize(820, 640)
        self.settings = load_settings()
        self.current_role = normalize_role(self.settings.get("role"))
        self._configure_style()

        header = ttk.Frame(self, padding=(22, 18, 22, 8))
        header.pack(fill="x")
        header.columnconfigure(0, weight=1)
        ttk.Label(header, text=APP_TITLE, style="AppTitle.TLabel").grid(
            row=0, column=0, sticky="w"
        )

        role_frame = ttk.Frame(header)
        role_frame.grid(row=0, column=1, sticky="e", padx=(18, 0))
        ttk.Label(role_frame, text="运行角色").pack(side="left", padx=(0, 8))
        self.role_var = tk.StringVar(value=ROLE_LABELS[self.current_role])
        self.role_combo = ttk.Combobox(
            role_frame,
            textvariable=self.role_var,
            values=tuple(ROLE_LABELS.values()),
            state="readonly",
            width=22,
        )
        self.role_combo.pack(side="left")
        self.role_combo.bind("<<ComboboxSelected>>", self.on_role_changed)

        self.role_hint_var = tk.StringVar()
        ttk.Label(
            header, textvariable=self.role_hint_var, style="Subtitle.TLabel"
        ).grid(row=1, column=0, columnspan=2, sticky="w", pady=(6, 0))

        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill="both", expand=True, padx=18, pady=(6, 18))
        self.send_page = SendPage(self.notebook, self.settings)
        self.receive_page = ReceivePage(
            self.notebook, self.settings, self.send_page.handle_link_test_result
        )
        self.send_page.receiver_is_active = self.receive_page.is_listening
        self.notebook.add(self.send_page, text=" 发送文件 ")
        self.notebook.add(self.receive_page, text=" 接收文件 ")
        self.apply_role(self.current_role)

        self.protocol("WM_DELETE_WINDOW", self.on_close)

    def _configure_style(self) -> None:
        style = ttk.Style(self)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("AppTitle.TLabel", font=("Microsoft YaHei UI", 18, "bold"))
        style.configure("Title.TLabel", font=("Microsoft YaHei UI", 14, "bold"))
        style.configure("Subtitle.TLabel", foreground="#586174")
        style.configure("TLabel", font=("Microsoft YaHei UI", 9))
        style.configure("TButton", font=("Microsoft YaHei UI", 9), padding=(10, 5))
        style.configure("TLabelframe.Label", font=("Microsoft YaHei UI", 10, "bold"))

    def active_workers(self) -> list[SenderWorker | ReceiverWorker | LinkTestSenderWorker | ChainMonitorWorker]:
        return [
            page.worker
            for page in (self.send_page, self.receive_page)
            if page.worker and page.worker.is_alive()
        ]

    def on_role_changed(self, _event=None) -> None:
        selected_role = next(
            (
                role
                for role, label in ROLE_LABELS.items()
                if label == self.role_var.get()
            ),
            ROLE_DEBUG,
        )
        if selected_role == self.current_role:
            return
        if self.active_workers():
            self.role_var.set(ROLE_LABELS[self.current_role])
            messagebox.showwarning(APP_TITLE, "请先停止当前发送或监听任务，再切换运行角色。")
            return
        self.apply_role(selected_role)

    def apply_role(self, role: str) -> None:
        self.current_role = normalize_role(role)
        self.role_var.set(ROLE_LABELS[self.current_role])
        visible_tabs = set(self.notebook.tabs())
        for page in (self.send_page, self.receive_page):
            if str(page) in visible_tabs:
                self.notebook.hide(page)

        page_map = {
            ROLE_SEND: (self.send_page, " 发送文件 "),
            ROLE_RECEIVE: (self.receive_page, " 接收文件 "),
        }
        for page_key in role_page_keys(self.current_role):
            page, title = page_map[page_key]
            self.notebook.add(page, text=title)

        first_page = page_map[role_page_keys(self.current_role)[0]][0]
        self.notebook.select(first_page)
        self.role_hint_var.set(ROLE_HINTS[self.current_role])

    def on_close(self) -> None:
        active_workers = self.active_workers()
        if active_workers and not messagebox.askyesno(APP_TITLE, "传输仍在进行，确定退出吗？"):
            return
        for worker in active_workers:
            worker.cancel()

        save_settings(
            {
                "role": self.current_role,
                "send": self.send_page.settings_snapshot(),
                "receive": self.receive_page.settings_snapshot(),
            }
        )
        self.destroy()


if __name__ == "__main__":
    TransferApplication().mainloop()
