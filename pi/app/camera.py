import glob
import logging
import os
import threading
import time
from typing import Optional, Union

from app.config import config

logger = logging.getLogger("vivarium.camera")

try:
    import cv2
except ImportError:
    cv2 = None  # type: ignore[assignment]


class USBCamera:
    def __init__(
        self,
        device: Union[int, str] = 0,
        width: int = 1280,
        height: int = 720,
        fps: int = 15,
    ) -> None:
        self.configured_device = device
        self.active_device: Optional[Union[int, str]] = None
        self.width = width
        self.height = height
        self.fps = fps
        self._lock = threading.Lock()
        self._cap = None
        self._last_probe_time: float = 0.0

    def is_available(self) -> bool:
        if cv2 is None or not config.camera_enabled:
            return False
        with self._lock:
            cap = self._get_capture()
            return cap is not None and cap.isOpened()

    def _open_device(self, dev: Union[int, str]):
        if cv2 is None:
            return None
        backend = getattr(cv2, "CAP_V4L2", cv2.CAP_ANY)
        cap = None
        try:
            if isinstance(dev, int):
                cap = cv2.VideoCapture(dev, backend)
                if not cap.isOpened():
                    cap = cv2.VideoCapture(dev)
            else:
                cap = cv2.VideoCapture(str(dev), backend)
                if not cap.isOpened():
                    cap = cv2.VideoCapture(str(dev))
        except Exception as exc:
            logger.debug("Failed opening video device %s: %s", dev, exc)
            return None

        if not cap or not cap.isOpened():
            if cap:
                cap.release()
            return None

        fourcc = getattr(cv2, "VideoWriter_fourcc", None)
        if fourcc:
            try:
                cap.set(cv2.CAP_PROP_FOURCC, fourcc(*"MJPG"))
            except Exception:
                pass

        try:
            cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)
            cap.set(cv2.CAP_PROP_FPS, self.fps)
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        except Exception:
            pass

        # Warm-up and test frame read
        frame_ok = False
        for _ in range(4):
            ret, frame = cap.read()
            if ret and frame is not None and frame.size > 0:
                frame_ok = True
                break
            time.sleep(0.05)

        if not frame_ok:
            logger.debug("Device %s opened but returned no valid test frames", dev)
            cap.release()
            return None

        logger.info("Successfully opened and verified camera device: %s", dev)
        return cap

    def _probe_devices(self):
        # 1. Try configured device first
        cap = self._open_device(self.configured_device)
        if cap is not None:
            self.active_device = self.configured_device
            return cap

        # 2. Probe candidate /dev/video* devices on Linux
        candidates: list[Union[int, str]] = []
        if os.name != "nt":
            video_nodes = sorted(glob.glob("/dev/video*"))
            for node in video_nodes:
                if node != self.configured_device and node != f"/dev/video{self.configured_device}":
                    candidates.append(node)

        # 3. Add numerical indices
        for idx in range(6):
            if idx != self.configured_device and idx not in candidates:
                candidates.append(idx)

        for dev in candidates:
            cap = self._open_device(dev)
            if cap is not None:
                self.active_device = dev
                logger.info(
                    "Auto-discovered working camera device %s (configured was %s)",
                    dev,
                    self.configured_device,
                )
                return cap

        logger.warning(
            "No working camera device could be opened (checked configured: %s, candidates: %s)",
            self.configured_device,
            candidates[:5],
        )
        return None

    def _get_capture(self):
        if cv2 is None or not config.camera_enabled:
            return None
        if self._cap is not None and self._cap.isOpened():
            return self._cap

        # Throttle failed probe attempts
        now = time.time()
        if now - self._last_probe_time < 3.0:
            return None
        self._last_probe_time = now

        self._cap = self._probe_devices()
        return self._cap

    def get_snapshot(self) -> Optional[bytes]:
        if cv2 is None or not config.camera_enabled:
            return None
        with self._lock:
            cap = self._get_capture()
            if cap is None:
                return None
            try:
                ret, frame = cap.read()
                if not ret or frame is None or frame.size == 0:
                    ret, frame = cap.read()
                    if not ret or frame is None or frame.size == 0:
                        logger.warning(
                            "Failed to read frame from active camera %s",
                            self.active_device,
                        )
                        self._release_locked()
                        return None
                success, buffer = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
                if not success or buffer is None:
                    return None
                return buffer.tobytes()
            except Exception as exc:
                logger.warning("Error capturing snapshot from camera: %s", exc)
                self._release_locked()
                return None

    def _release_locked(self) -> None:
        if self._cap is not None:
            try:
                self._cap.release()
            except Exception:
                pass
            self._cap = None
            self.active_device = None

    def release(self) -> None:
        with self._lock:
            self._release_locked()


usb_camera = USBCamera(
    device=config.camera_device,
    width=config.camera_width,
    height=config.camera_height,
    fps=config.camera_fps,
)
