from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .tagger import TaggerConfig, TaggerInputError, WdTagger


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Tag images with wd-swinv2-tagger-v3 using the local ONNX model."
    )
    parser.add_argument("images", nargs="+", type=Path, help="image files to tag")
    parser.add_argument(
        "--model-dir",
        type=Path,
        default=None,
        help="directory containing model.onnx and selected_tags.csv",
    )
    parser.add_argument(
        "--threads",
        type=int,
        default=None,
        help="ONNX Runtime intra-op thread count (default: 2)",
    )
    parser.add_argument(
        "--general-threshold",
        type=float,
        default=None,
        help="general tag threshold (default: 0.35)",
    )
    parser.add_argument(
        "--character-threshold",
        type=float,
        default=None,
        help="character tag threshold (default: 0.85)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    config = TaggerConfig.from_env()
    if args.model_dir is not None:
        config = TaggerConfig(
            model_dir=args.model_dir,
            general_threshold=config.general_threshold,
            character_threshold=config.character_threshold,
            threads=config.threads,
        )
    if (
        args.threads is not None
        or args.general_threshold is not None
        or args.character_threshold is not None
    ):
        config = TaggerConfig(
            model_dir=config.model_dir,
            general_threshold=(
                args.general_threshold
                if args.general_threshold is not None
                else config.general_threshold
            ),
            character_threshold=(
                args.character_threshold
                if args.character_threshold is not None
                else config.character_threshold
            ),
            threads=args.threads if args.threads is not None else config.threads,
        )

    try:
        tagger = WdTagger(config)
    except Exception as exc:
        print(f"failed to load model: {exc}", file=sys.stderr)
        return 2

    output: list[dict[str, object]] = []
    failed = False
    for image_path in args.images:
        try:
            result = tagger.tag_file(image_path)
            output.append({"path": str(image_path), **result.as_dict()})
        except TaggerInputError as exc:
            failed = True
            output.append({"path": str(image_path), "error": str(exc)})

    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
