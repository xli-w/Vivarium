import logging
import threading
from typing import Optional

from app.config import config

logger = logging.getLogger("vivarium.camera")

try:
    import cv2
except ImportError:
    cv2 = None  # type: ignore[assignment]


class USBCamera:
    def __init__(
        self,
        device_index: int = 0,
        width: int = 1280,
        height: int = 720,
        fps: int = 15,
    ) -> None:
        self.device_index = device_index
        self.width = width
        self.height = height
        self.fps = fps
        self._lock = threading.Lock()
        self._cap = None

    def is_available(self) -> bool:
        if cv2 is None or not config.camera_enabled:
            return False
        with self._lock:
            cap = self._get_capture()
            return cap is not None and cap.isOpened()

    def _get_capture(self):
        if cv2 is None:
            return None
        if self._cap is None or not self._cap.isOpened():
            try:
                # Prefer V4L2 on Linux if available, fallback to default backend
                backend = getattr(cv2, "CAP_V4L2", cv2.CAP_ANY)
                self._cap = cv2.VideoCapture(self.device_index, backend)
                if not self._cap.isOpened():
                    self._cap = cv2.VideoCapture(self.device_index)
                if self._cap.isOpened():
                    self._cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
                    self._cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)
                    self._cap.set(cv2.CAP_PROP_FPS, self.fps)
                    fourcc = getattr(cv2, "VideoWriter_fourcc", None)
                    if fourcc:
                        self._cap.set(cv2.CAP_PROP_FOURCC, fourcc(*"MJPG"))
            except Exception as exc:
                logger.warning("Failed to open camera device %s: %s", self.device_index, exc)
                self._cap = None
        return self._cap if self._cap and self._cap.isOpened() else None

    def get_snapshot(self) -> Optional[bytes]:
        if cv2 is None or not config.camera_enabled:
            return None
        with self._lock:
            cap = self._get_capture()
            if cap is None:
                return None
            try:
                # Flush any stale buffered frame in V4L2 queue
                cap.grab()
                ret, frame = cap.read()
                if not ret or frame is None:
                    logger.warning("Failed to read frame from USB camera device %s", self.device_index)
                    self._release_locked()
                    return None
                success, buffer = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
                if not success or buffer is None:
                    return None
                return buffer.tobytes()
            except Exception as exc:
                logger.warning("Error capturing snapshot from USB camera: %s", exc)
                self._release_locked()
                return None

    def _release_locked(self) -> None:
        if self._cap is not None:
            try:
                self._cap.release()
            except Exception:
                pass
            self._cap = None

    def release(self) -> None:
        with self._lock:
            self._release_locked()


usb_camera = USBCamera(
    device_index=config.camera_device_index,
    width=config.camera_width,
    height=config.camera_height,
    fps=config.camera_fps,
)
