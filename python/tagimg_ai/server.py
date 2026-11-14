from __future__ import annotations

import logging
import os
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse
from starlette.concurrency import run_in_threadpool

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
    yield


app = FastAPI(
    title="tagimg AI",
    version="0.1.0",
    description="Local WD SwinV2 tagger sidecar",
    lifespan=lifespan,
)


@app.get("/health")
def health(request: Request) -> dict[str, object]:
    tagger = getattr(request.app.state, "tagger", None)
    if tagger is None:
        raise HTTPException(status_code=503, detail="tagger is not ready")
    return {
        "status": "ok",
        "model": tagger.model_name,
        "loaded": True,
    }


@app.post("/v1/tag/image")
async def tag_image(request: Request) -> JSONResponse:
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
        result = await run_in_threadpool(tagger.tag_bytes, data)
    except ImageTooLargeError as exc:
        raise HTTPException(status_code=413, detail=str(exc)) from exc
    except InvalidImageError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:
        LOGGER.exception("tagger inference failed")
        raise HTTPException(status_code=500, detail="tagger inference failed") from exc

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
