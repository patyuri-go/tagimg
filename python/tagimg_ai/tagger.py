from __future__ import annotations

import csv
import io
import json
import logging
import os
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import onnxruntime as ort
from PIL import Image, ImageOps, UnidentifiedImageError

LOGGER = logging.getLogger(__name__)

MODEL_NAME = "wd-swinv2-tagger-v3"
MAX_IMAGE_BYTES = 50 * 1024 * 1024
DEFAULT_MODEL_DIR = Path(__file__).resolve().parents[2] / "models" / MODEL_NAME


class TaggerInputError(ValueError):
    """传给 tagger 的非法输入异常的基类。"""


class ImageTooLargeError(TaggerInputError):
    """图片超过 MAX_IMAGE_BYTES 上限。"""


class InvalidImageError(TaggerInputError):
    """字节流无法解码成图片。"""


@dataclass(frozen=True)
class TaggerConfig:
    model_dir: Path = DEFAULT_MODEL_DIR
    threads: int = 2

    @classmethod
    def from_env(cls) -> "TaggerConfig":
        return cls(
            model_dir=Path(os.environ.get("TAGIMG_MODEL_DIR", str(DEFAULT_MODEL_DIR))),
            threads=int(os.environ.get("TAGIMG_TAGGER_THREADS", "2")),
        )


@dataclass(frozen=True)
class TagScore:
    name: str
    score: float

    def as_dict(self) -> dict[str, object]:
        return {"name": self.name, "score": self.score}


@dataclass(frozen=True)
class TaggingResult:
    general: list[TagScore]
    character: list[TagScore]
    rating: list[TagScore]
    elapsed_ms: float

    def as_dict(self) -> dict[str, object]:
        return {
            "model": MODEL_NAME,
            "elapsed_ms": round(self.elapsed_ms, 3),
            "general": [tag.as_dict() for tag in self.general],
            "character": [tag.as_dict() for tag in self.character],
            "rating": [tag.as_dict() for tag in self.rating],
        }


class WdTagger:
    """SmilingWolf/wd-swinv2-tagger-v3 的 ONNX 推理封装。

    模型输入是 NHWC、float32、BGR 顺序、取值范围 [0, 255]；
    输出本身已经是 sigmoid 概率，不要再套一层 sigmoid。
    """

    def __init__(self, config: TaggerConfig | None = None) -> None:
        self._config = config or TaggerConfig.from_env()
        if self._config.threads < 1:
            raise ValueError("threads must be at least 1")

        model_dir = self._config.model_dir.expanduser().resolve()
        self._model_dir = model_dir
        model_path = model_dir / "model.onnx"
        tags_path = model_dir / "selected_tags.csv"
        cv_config_path = model_dir / "sw_jax_cv_config.json"

        for path in (model_path, tags_path, cv_config_path):
            if not path.is_file():
                raise FileNotFoundError(f"missing tagger model file: {path}")

        with cv_config_path.open("r", encoding="utf-8") as handle:
            cv_config = json.load(handle)
        self._configured_image_size = int(cv_config.get("image_size", 448))

        (
            self._names,
            self._general_indices,
            self._character_indices,
            self._rating_indices,
        ) = self._load_labels(tags_path)
        self._session = self._create_session(model_path)
        self._input_name = self._session.get_inputs()[0].name
        self._layout, self._image_size = self._inspect_input(self._session)
        self._validate_output(self._session, len(self._names))

        LOGGER.info(
            "loaded %s: input=%s layout=%s size=%d tags=%d threads=%d",
            MODEL_NAME,
            self._input_name,
            self._layout,
            self._image_size,
            len(self._names),
            self._config.threads,
        )

    @staticmethod
    def _load_labels(
        path: Path,
    ) -> tuple[list[str], list[int], list[int], list[int]]:
        with path.open("r", encoding="utf-8", newline="") as handle:
            rows = list(csv.DictReader(handle))
        if not rows:
            raise ValueError(f"empty tag list: {path}")

        names = [row["name"] for row in rows]
        general: list[int] = []
        character: list[int] = []
        rating: list[int] = []
        for index, row in enumerate(rows):
            category = int(row["category"])
            if category == 0:
                general.append(index)
            elif category == 4:
                character.append(index)
            elif category == 9:
                rating.append(index)

        if not general or not character or len(rating) != 4:
            raise ValueError(
                "unexpected selected_tags.csv categories: "
                f"general={len(general)} character={len(character)} rating={len(rating)}"
            )
        return names, general, character, rating

    def _create_session(self, model_path: Path) -> ort.InferenceSession:
        options = ort.SessionOptions()
        options.intra_op_num_threads = self._config.threads
        options.inter_op_num_threads = 1
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        return ort.InferenceSession(
            str(model_path),
            sess_options=options,
            providers=["CPUExecutionProvider"],
        )

    def _inspect_input(self, session: ort.InferenceSession) -> tuple[str, int]:
        shape = session.get_inputs()[0].shape
        if len(shape) != 4:
            raise ValueError(f"unexpected model input shape: {shape}")

        if isinstance(shape[1], int) and shape[1] == 3:
            layout = "NCHW"
            size = (
                shape[2] if isinstance(shape[2], int) else self._configured_image_size
            )
        elif isinstance(shape[3], int) and shape[3] == 3:
            layout = "NHWC"
            size = (
                shape[2] if isinstance(shape[2], int) else self._configured_image_size
            )
        else:
            raise ValueError(f"unsupported model input layout: {shape}")
        return layout, int(size)

    @staticmethod
    def _validate_output(session: ort.InferenceSession, tag_count: int) -> None:
        shape = session.get_outputs()[0].shape
        if len(shape) != 2:
            raise ValueError(f"unexpected model output shape: {shape}")
        last_dim = shape[-1]
        if isinstance(last_dim, int) and last_dim != tag_count:
            raise ValueError(
                f"model outputs {last_dim} scores but selected_tags.csv has {tag_count} tags"
            )

    def warmup(self) -> None:
        started = time.perf_counter()
        image = np.full(
            (1, self._image_size, self._image_size, 3),
            255.0,
            dtype=np.float32,
        )
        if self._layout == "NCHW":
            image = np.ascontiguousarray(np.transpose(image, (0, 3, 1, 2)))
        self._session.run(None, {self._input_name: image})
        LOGGER.info(
            "warmup completed in %.1f ms", (time.perf_counter() - started) * 1000
        )

    @staticmethod
    def _ensure_rgb(image: Image.Image) -> Image.Image:
        if image.mode in ("RGBA", "LA") or (
            image.mode == "P" and "transparency" in image.info
        ):
            rgba = image.convert("RGBA")
            background = Image.new("RGBA", rgba.size, (255, 255, 255, 255))
            return Image.alpha_composite(background, rgba).convert("RGB")
        if image.mode != "RGB":
            return image.convert("RGB")
        return image

    def _decode_image(self, data: bytes) -> Image.Image:
        if not data:
            raise InvalidImageError("empty image")
        if len(data) > MAX_IMAGE_BYTES:
            raise ImageTooLargeError(f"image exceeds {MAX_IMAGE_BYTES} bytes")
        try:
            with Image.open(io.BytesIO(data)) as source:
                source.load()
                image = ImageOps.exif_transpose(source).copy()
        except (
            UnidentifiedImageError,
            OSError,
            ValueError,
            Image.DecompressionBombError,
        ) as exc:
            raise InvalidImageError("cannot decode image") from exc
        return self._ensure_rgb(image)

    def _to_tensor(self, image: Image.Image) -> np.ndarray:
        width, height = image.size
        side = max(width, height)
        canvas = Image.new("RGB", (side, side), (255, 255, 255))
        canvas.paste(image, ((side - width) // 2, (side - height) // 2))
        resized = canvas.resize(
            (self._image_size, self._image_size), Image.Resampling.BICUBIC
        )

        # 官方 WD v3 预处理：BGR、0..255，不做 mean/std 归一化。
        array = np.asarray(resized, dtype=np.float32)[:, :, ::-1]
        array = np.ascontiguousarray(array)
        if self._layout == "NCHW":
            array = np.ascontiguousarray(np.transpose(array, (2, 0, 1)))
        return np.expand_dims(array, axis=0)

    def _select(
        self, scores: np.ndarray, indices: list[int], threshold: float
    ) -> list[TagScore]:
        tags = [
            TagScore(self._names[index], float(scores[index]))
            for index in indices
            if float(scores[index]) > threshold
        ]
        tags.sort(key=lambda tag: tag.score, reverse=True)
        return tags

    def tag_bytes(self, data: bytes, threshold: float) -> TaggingResult:
        if not 0.0 <= threshold <= 1.0:
            raise ValueError("threshold must be in [0, 1]")
        started = time.perf_counter()
        image = self._decode_image(data)
        tensor = self._to_tensor(image)
        output = self._session.run(None, {self._input_name: tensor})[0]
        probabilities = np.asarray(output, dtype=np.float32)
        if probabilities.ndim != 2 or probabilities.shape[0] != 1:
            raise RuntimeError(
                f"unexpected model output batch shape: {probabilities.shape}"
            )
        scores = probabilities[0]
        if scores.shape[0] != len(self._names):
            raise RuntimeError(
                f"model returned {scores.shape[0]} scores for {len(self._names)} tags"
            )

        elapsed_ms = (time.perf_counter() - started) * 1000
        return TaggingResult(
            general=self._select(scores, self._general_indices, threshold),
            character=self._select(scores, self._character_indices, threshold),
            rating=[
                TagScore(self._names[index], float(scores[index]))
                for index in self._rating_indices
            ],
            elapsed_ms=elapsed_ms,
        )

    def tag_file(self, path: str | Path, threshold: float) -> TaggingResult:
        file_path = Path(path)
        try:
            size = file_path.stat().st_size
        except OSError as exc:
            raise InvalidImageError(f"cannot stat image: {file_path}") from exc
        if size > MAX_IMAGE_BYTES:
            raise ImageTooLargeError(f"image exceeds {MAX_IMAGE_BYTES} bytes")
        try:
            data = file_path.read_bytes()
        except OSError as exc:
            raise InvalidImageError(f"cannot read image: {file_path}") from exc
        return self.tag_bytes(data, threshold)

    @property
    def model_name(self) -> str:
        return MODEL_NAME


_tagger: WdTagger | None = None
_tagger_lock = threading.Lock()


def get_tagger() -> WdTagger:
    global _tagger
    if _tagger is None:
        with _tagger_lock:
            if _tagger is None:
                _tagger = WdTagger(TaggerConfig.from_env())
    return _tagger
