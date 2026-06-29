from __future__ import annotations

import hashlib
import os
import shutil
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


SUPPORTED_IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp"}
OPENAI_FILE_HOSTS = {"files.oaiusercontent.com"}
MAX_ACTION_FILE_BYTES = 20 * 1024 * 1024


class FaceLockStoreError(RuntimeError):
    pass


@dataclass(frozen=True)
class CachedUpload:
    path: Path
    sha256: str
    cache_hit: bool


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def atomic_write_bytes(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_bytes(value)
    temporary.replace(path)


def safe_name(value: str) -> str:
    normalized = "".join(character if character.isalnum() or character in "-_" else "_" for character in value)
    normalized = normalized.strip("_")
    return normalized or "face_lock"


def detect_image_extension(content: bytes, mime_type: str = "") -> str:
    if content.startswith(b"\x89PNG\r\n\x1a\n"):
        return ".png"
    if content.startswith(b"\xff\xd8\xff"):
        return ".jpg"
    if content.startswith(b"RIFF") and content[8:12] == b"WEBP":
        return ".webp"

    normalized_mime = mime_type.split(";", 1)[0].strip().lower()
    mime_extensions = {
        "image/png": ".png",
        "image/jpeg": ".jpg",
        "image/jpg": ".jpg",
        "image/webp": ".webp",
    }
    extension = mime_extensions.get(normalized_mime)
    if extension:
        return extension
    raise FaceLockStoreError("Downloaded action file is not a supported image")


class FaceLockStore:
    def __init__(self, root: Path, cache_root: Path | None = None) -> None:
        self.root = root.resolve()
        self.cache_root = (
            cache_root or self.root / "lore_api" / ".data" / "face_lock_cache"
        ).resolve()

    def cache_action_file(self, file_ref: dict[str, Any]) -> CachedUpload:
        download_link = str(file_ref.get("download_link") or "").strip()
        mime_type = str(file_ref.get("mime_type") or "").strip()
        if not download_link:
            raise FaceLockStoreError("Action file reference has no download_link")

        parsed = urllib.parse.urlparse(download_link)
        hostname = (parsed.hostname or "").lower()
        allowed_host = hostname in OPENAI_FILE_HOSTS or hostname.endswith(".oaiusercontent.com")
        if parsed.scheme != "https" or not allowed_host:
            raise FaceLockStoreError(
                "Action file download_link must use an OpenAI HTTPS file host"
            )

        request = urllib.request.Request(
            download_link,
            headers={"User-Agent": "World-of-Might-and-Magic-Lore-API/1.0"},
        )
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                response_mime = response.headers.get("Content-Type", mime_type)
                content = response.read(MAX_ACTION_FILE_BYTES + 1)
        except Exception as error:
            raise FaceLockStoreError(
                f"Could not download action file: {type(error).__name__}"
            ) from error

        if len(content) > MAX_ACTION_FILE_BYTES:
            raise FaceLockStoreError("Action image exceeds the 20 MB Lore API limit")
        extension = detect_image_extension(content, response_mime)
        digest = sha256_bytes(content)
        target = self.cache_root / f"{digest}{extension}"
        cache_hit = target.exists()
        if not cache_hit:
            atomic_write_bytes(target, content)
        return CachedUpload(path=target, sha256=digest, cache_hit=cache_hit)

    def cache_action_files(
        self,
        file_refs: Iterable[dict[str, Any]],
    ) -> list[CachedUpload]:
        uploads: list[CachedUpload] = []
        seen: set[str] = set()
        for file_ref in file_refs:
            upload = self.cache_action_file(file_ref)
            if upload.sha256 in seen:
                continue
            seen.add(upload.sha256)
            uploads.append(upload)
        return uploads

    def save_to_repository(
        self,
        *,
        character_folder: str,
        character_file_stem: str,
        variant: str,
        upload: CachedUpload,
        overwrite: bool = False,
    ) -> tuple[Path, bool]:
        return self.save_many_to_repository(
            character_folder=character_folder,
            character_file_stem=character_file_stem,
            variant_uploads=[(variant, upload)],
            overwrite=overwrite,
        )[0]

    def save_many_to_repository(
        self,
        *,
        character_folder: str,
        character_file_stem: str,
        variant_uploads: list[tuple[str, CachedUpload]],
        overwrite: bool = False,
    ) -> list[tuple[Path, bool]]:
        portraits_root = (
            self.root / "11_Медиа" / "Портреты_персонажей"
        ).resolve()
        folder = (portraits_root / character_folder / "Face_Lock").resolve()
        if portraits_root not in folder.parents:
            raise FaceLockStoreError("Face lock folder escapes portrait storage")
        folder.mkdir(parents=True, exist_ok=True)

        operations: list[dict[str, Any]] = []
        for variant, upload in variant_uploads:
            base_name = f"{character_file_stem}_face_lock_{safe_name(variant)}"
            destination = folder / f"{base_name}{upload.path.suffix.lower()}"
            existing_variants = [
                path
                for path in folder.glob(f"{base_name}.*")
                if path.is_file() and path.suffix.lower() in SUPPORTED_IMAGE_SUFFIXES
            ]
            matching = next(
                (
                    existing
                    for existing in existing_variants
                    if sha256_file(existing) == upload.sha256
                ),
                None,
            )
            if matching is not None:
                operations.append(
                    {
                        "destination": matching,
                        "cache_hit": True,
                        "upload": upload,
                        "existing": existing_variants,
                    }
                )
                continue
            if existing_variants and not overwrite:
                raise FaceLockStoreError(
                    f"Face lock already exists: {existing_variants[0].relative_to(self.root)}"
                )
            operations.append(
                {
                    "destination": destination,
                    "cache_hit": False,
                    "upload": upload,
                    "existing": existing_variants,
                }
            )

        temporary_paths: list[Path] = []
        try:
            for operation in operations:
                if operation["cache_hit"]:
                    continue
                destination = operation["destination"]
                temporary = destination.with_name(
                    f".{destination.name}.{os.getpid()}.tmp"
                )
                shutil.copy2(operation["upload"].path, temporary)
                operation["temporary"] = temporary
                temporary_paths.append(temporary)
        except Exception:
            for temporary in temporary_paths:
                temporary.unlink(missing_ok=True)
            raise

        results: list[tuple[Path, bool]] = []
        for operation in operations:
            destination = operation["destination"]
            if not operation["cache_hit"]:
                operation["temporary"].replace(destination)
                for existing in operation["existing"]:
                    if existing != destination and existing.exists():
                        existing.unlink()
            results.append((destination, operation["cache_hit"]))
        return results
