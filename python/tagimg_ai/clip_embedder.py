from __future__ import annotations

import io
import logging
import os
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import cn_clip.clip as clip
import torch
from PIL import Image, ImageOps, UnidentifiedImageError

LOGGER = logging.getLogger(__name__)

MODEL_NAME = "chinese-clip-vit-large-patch14"
MODEL_ARCH = "ViT-L-14"
EMBED_DIM = 768
MAX_IMAGE_BYTES = 50 * 1024 * 1024
# 单次文本请求最多编码多少条，避免一次请求把 CPU 占满。
MAX_TEXTS_PER_REQUEST = 32
MAX_TEXT_CHARS = 512
DEFAULT_MODEL_DIR = Path(__file__).resolve().parents[2] / "models" / MODEL_NAME


class ClipInputError(ValueError):
    """传给 CLIP embedder 的非法输入异常的基类。"""


class ImageTooLargeError(ClipInputError):
    """图片超过 MAX_IMAGE_BYTES 上限。"""


class InvalidImageError(ClipInputError):
    """字节流无法解码成图片。"""


class InvalidTextError(ClipInputError):
    """文本列表为空、某条为空或过长。"""


@dataclass(frozen=True)
class ClipConfig:
    model_dir: Path = DEFAULT_MODEL_DIR
    threads: int = 2
    preload: bool = False

    @classmethod
    def from_env(cls) -> "ClipConfig":
        return cls(
            model_dir=Path(
                os.environ.get("TAGIMG_CLIP_MODEL_DIR", str(DEFAULT_MODEL_DIR))
            ),
            threads=int(os.environ.get("TAGIMG_CLIP_THREADS", "2")),
            preload=_env_flag("TAGIMG_CLIP_PRELOAD", False),
        )


@dataclass(frozen=True)
class ImageEmbeddingResult:
    embedding: list[float]
    elapsed_ms: float

    def as_dict(self) -> dict[str, object]:
        return {
            "model": MODEL_NAME,
            "arch": MODEL_ARCH,
            "dim": EMBED_DIM,
            "normalized": True,
            "elapsed_ms": round(self.elapsed_ms, 3),
            "embedding": self.embedding,
        }


@dataclass(frozen=True)
class TextEmbeddingResult:
    embeddings: list[list[float]]
    elapsed_ms: float

    def as_dict(self) -> dict[str, object]:
        return {
            "model": MODEL_NAME,
            "arch": MODEL_ARCH,
            "dim": EMBED_DIM,
            "normalized": True,
            "elapsed_ms": round(self.elapsed_ms, 3),
            "embeddings": self.embeddings,
        }


def _env_flag(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


class ClipEmbedder:
    """Chinese-CLIP ViT-L/14 的图像/文本向量编码封装。

    图像和文本编码到同一个 768 维空间，输出都做 L2 归一化，
    因此余弦相似度可以直接用点积计算。
    """

    def __init__(self, config: ClipConfig | None = None) -> None:
        self._config = config or ClipConfig.from_env()
        if self._config.threads < 1:
            raise ValueError("threads must be at least 1")

        model_dir = self._config.model_dir.expanduser().resolve()
        self._model_dir = model_dir
        # load_from_name 会在 download_root 下找官方权重文件名，找不到才联网下载。
        checkpoint = model_dir / "clip_cn_vit-l-14.pt"
        if not checkpoint.is_file():
            LOGGER.warning(
                "clip checkpoint not found at %s, trying to download to %s",
                checkpoint,
                model_dir,
            )

        torch.set_num_threads(self._config.threads)
        started = time.perf_counter()
        try:
            model, preprocess = clip.load_from_name(
                MODEL_ARCH, device="cpu", download_root=str(model_dir)
            )
        except Exception as exc:  # noqa: BLE001 - 启动期需要给出清晰原因
            raise RuntimeError(
                f"failed to load {MODEL_NAME} from {model_dir}: {exc}"
            ) from exc

        model.eval()
        self._model = model
        self._preprocess = preprocess
        LOGGER.info(
            "loaded %s: arch=%s dim=%d threads=%d in %.1f ms",
            MODEL_NAME,
            MODEL_ARCH,
            EMBED_DIM,
            self._config.threads,
            (time.perf_counter() - started) * 1000,
        )

    @property
    def model_name(self) -> str:
        return MODEL_NAME

    @property
    def embed_dim(self) -> int:
        return EMBED_DIM

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

    @staticmethod
    def _normalize(features: torch.Tensor) -> torch.Tensor:
        return torch.nn.functional.normalize(features.float(), dim=-1)

    def embed_image_bytes(self, data: bytes) -> ImageEmbeddingResult:
        started = time.perf_counter()
        image = self._decode_image(data)
        tensor = self._preprocess(image).unsqueeze(0)
        with torch.inference_mode():
            features = self._normalize(self._model.encode_image(tensor))
        values = features[0].tolist()
        if len(values) != EMBED_DIM:
            raise RuntimeError(f"unexpected embedding size: {len(values)}")
        return ImageEmbeddingResult(
            embedding=values,
            elapsed_ms=(time.perf_counter() - started) * 1000,
        )

    def embed_image_file(self, path: str | os.PathLike[str]) -> ImageEmbeddingResult:
        with open(path, "rb") as handle:
            return self.embed_image_bytes(handle.read())

    def embed_texts(self, texts: list[str]) -> TextEmbeddingResult:
        if not texts:
            raise InvalidTextError("texts must not be empty")
        if len(texts) > MAX_TEXTS_PER_REQUEST:
            raise InvalidTextError(
                f"too many texts: {len(texts)} > {MAX_TEXTS_PER_REQUEST}"
            )
        cleaned: list[str] = []
        for text in texts:
            if not isinstance(text, str):
                raise InvalidTextError("text must be a string")
            value = text.strip()
            if not value:
                raise InvalidTextError("text must not be empty")
            if len(value) > MAX_TEXT_CHARS:
                raise InvalidTextError(
                    f"text too long: {len(value)} > {MAX_TEXT_CHARS}"
                )
            cleaned.append(value)

        started = time.perf_counter()
        tokens = clip.tokenize(cleaned)
        with torch.inference_mode():
            features = self._normalize(self._model.encode_text(tokens))
        rows = [row.tolist() for row in features]
        for row in rows:
            if len(row) != EMBED_DIM:
                raise RuntimeError(f"unexpected embedding size: {len(row)}")
        return TextEmbeddingResult(
            embeddings=rows,
            elapsed_ms=(time.perf_counter() - started) * 1000,
        )

    def warmup(self) -> None:
        started = time.perf_counter()
        dummy = Image.new("RGB", (224, 224), (128, 128, 128))
        tensor = self._preprocess(dummy).unsqueeze(0)
        with torch.inference_mode():
            self._model.encode_image(tensor)
        LOGGER.info(
            "warmup completed in %.1f ms", (time.perf_counter() - started) * 1000
        )


_embedder: ClipEmbedder | None = None
_embedder_lock = threading.Lock()


def get_embedder() -> ClipEmbedder:
    """懒加载单例；首次调用会读入约 1.7 GB 权重。"""
    global _embedder
    if _embedder is None:
        with _embedder_lock:
            if _embedder is None:
                _embedder = ClipEmbedder(ClipConfig.from_env())
    return _embedder
