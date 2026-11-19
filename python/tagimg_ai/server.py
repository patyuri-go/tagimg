from __future__ import annotations

import logging
import os
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from .clip_embedder import (
    EMBED_DIM,
    MODEL_NAME as CLIP_MODEL_NAME,
    ClipConfig,
    ClipInputError,
    get_embedder,
)
from .clip_embedder import ImageTooLargeError as ClipImageTooLargeError
from .clip_embedder import InvalidImageError as ClipInvalidImageError
from .tagger import (
    MAX_IMAGE_BYTES,
    ImageTooLargeError,
    InvalidImageError,
    get_tagger,
)

LOGGER = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI):
    tagger = await run_in_threadpool(get_tagger)
    await run_in_threadpool(tagger.warmup)
    app.state.tagger = tagger

    # CLIP 权重有 1.7 GB，默认由 TAGIMG_CLIP_PRELOAD 决定是否在启动阶段加载。
    # 预加载失败直接抛出，让进程退出，避免带着不可用的向量能力接单。
    clip_config = ClipConfig.from_env()
    if clip_config.preload:
        embedder = await run_in_threadpool(get_embedder)
        await run_in_threadpool(embedder.warmup)
        app.state.embedder = embedder
    else:
        app.state.embedder = None
        LOGGER.info("clip 未预加载，首个向量请求时再加载")
    yield


app = FastAPI(
    title="tagimg AI",
    version="0.2.0",
    description="Local WD SwinV2 tagger + Chinese-CLIP embedder sidecar",
    lifespan=lifespan,
)


class TextEmbedRequest(BaseModel):
    texts: list[str]


async def ensure_embedder(request: Request):
    """返回已加载的 CLIP 编码器，未预加载时在首次请求中加载。"""
    embedder = getattr(request.app.state, "embedder", None)
    if embedder is not None:
        return embedder
    try:
        embedder = await run_in_threadpool(get_embedder)
    except Exception as exc:  # noqa: BLE001 - 加载失败要转成 503
        LOGGER.exception("clip embedder is not available")
        raise HTTPException(
            status_code=503, detail="clip embedder is not available"
        ) from exc
    request.app.state.embedder = embedder
    return embedder


@app.get("/health")
def health(request: Request) -> dict[str, object]:
    tagger = getattr(request.app.state, "tagger", None)
    if tagger is None:
        raise HTTPException(status_code=503, detail="tagger is not ready")
    return {
        "status": "ok",
        "model": tagger.model_name,
        "loaded": True,
        "clip": {
            "model": CLIP_MODEL_NAME,
            "dim": EMBED_DIM,
            "loaded": getattr(request.app.state, "embedder", None) is not None,
        },
    }


@app.post("/v1/tag/image")
async def tag_image(request: Request) -> JSONResponse:
    raw_threshold = request.query_params.get("threshold")
    if raw_threshold is None:
        raise HTTPException(status_code=400, detail="threshold is required")
    try:
        threshold = float(raw_threshold)
    except ValueError as exc:
        raise HTTPException(
            status_code=400, detail="threshold must be a number in [0, 1]"
        ) from exc
    if not 0.0 <= threshold <= 1.0:
        raise HTTPException(
            status_code=400, detail="threshold must be a number in [0, 1]"
        )

    content_length = request.headers.get("content-length")
    if content_length is not None:
        try:
            if int(content_length) > MAX_IMAGE_BYTES:
                raise HTTPException(status_code=413, detail="image is too large")
        except ValueError:
            raise HTTPException(status_code=400, detail="invalid Content-Length")

    data = await request.body()
    if len(data) > MAX_IMAGE_BYTES:
        raise HTTPException(status_code=413, detail="image is too large")
    if not data:
        raise HTTPException(status_code=400, detail="empty request body")

    tagger = getattr(request.app.state, "tagger", None)
    if tagger is None:
        raise HTTPException(status_code=503, detail="tagger is not ready")

    try:
        result = await run_in_threadpool(tagger.tag_bytes, data, threshold)
    except ImageTooLargeError as exc:
        raise HTTPException(status_code=413, detail=str(exc)) from exc
    except InvalidImageError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:
        LOGGER.exception("tagger inference failed")
        raise HTTPException(status_code=500, detail="tagger inference failed") from exc

    return JSONResponse(result.as_dict())


@app.post("/v1/embed/image")
async def embed_image(request: Request) -> JSONResponse:
    content_length = request.headers.get("content-length")
    if content_length is not None:
        try:
            if int(content_length) > MAX_IMAGE_BYTES:
                raise HTTPException(status_code=413, detail="image is too large")
        except ValueError:
            raise HTTPException(status_code=400, detail="invalid Content-Length")

    data = await request.body()
    if len(data) > MAX_IMAGE_BYTES:
        raise HTTPException(status_code=413, detail="image is too large")
    if not data:
        raise HTTPException(status_code=400, detail="empty request body")

    embedder = await ensure_embedder(request)

    try:
        result = await run_in_threadpool(embedder.embed_image_bytes, data)
    except ClipImageTooLargeError as exc:
        raise HTTPException(status_code=413, detail=str(exc)) from exc
    except ClipInvalidImageError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:
        LOGGER.exception("clip image embedding failed")
        raise HTTPException(
            status_code=500, detail="clip image embedding failed"
        ) from exc

    return JSONResponse(result.as_dict())


@app.post("/v1/embed/text")
async def embed_text(request: Request, payload: TextEmbedRequest) -> JSONResponse:
    embedder = await ensure_embedder(request)

    try:
        result = await run_in_threadpool(embedder.embed_texts, payload.texts)
    except ClipInputError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:
        LOGGER.exception("clip text embedding failed")
        raise HTTPException(
            status_code=500, detail="clip text embedding failed"
        ) from exc

    return JSONResponse(result.as_dict())


def main() -> None:
    import uvicorn

    logging.basicConfig(
        level=os.environ.get("TAGIMG_LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    uvicorn.run(
        "tagimg_ai.server:app",
        host=os.environ.get("TAGIMG_AI_HOST", "127.0.0.1"),
        port=int(os.environ.get("TAGIMG_AI_PORT", "8001")),
        log_level=os.environ.get("TAGIMG_LOG_LEVEL", "info").lower(),
    )


if __name__ == "__main__":
    main()
