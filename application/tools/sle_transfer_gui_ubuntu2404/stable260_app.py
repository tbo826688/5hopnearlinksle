import os

os.environ.setdefault(
    "SLE_TRANSFER_APP_TITLE",
    "NearLink SLE 五跳文件透传平台 - SLE闭环稳定260版（Ubuntu 24.04）",
)
os.environ.setdefault(
    "SLE_TRANSFER_SETTINGS_FILE",
    ".sle_transfer_gui_stable260.json",
)
os.environ.setdefault("SLE_TRANSFER_DEFAULT_RATE_KBPS", "260")
os.environ.setdefault("SLE_TRANSFER_DEFAULT_PACING_MODE", "stable_260")

from app import TransferApplication


if __name__ == "__main__":
    TransferApplication().mainloop()
