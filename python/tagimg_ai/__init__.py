from __future__ import annotations

from .tagger import (
    DEFAULT_MODEL_DIR,
    MAX_IMAGE_BYTES,
    MODEL_NAME,
    ImageTooLargeError,
    InvalidImageError,
    TaggingResult,
    TagScore,
    TaggerConfig,
    TaggerInputError,
    WdTagger,
    get_tagger,
)

__all__ = [
    "DEFAULT_MODEL_DIR",
    "MAX_IMAGE_BYTES",
    "MODEL_NAME",
    "ImageTooLargeError",
    "InvalidImageError",
    "TaggingResult",
    "TagScore",
    "TaggerConfig",
    "TaggerInputError",
    "WdTagger",
    "get_tagger",
]
