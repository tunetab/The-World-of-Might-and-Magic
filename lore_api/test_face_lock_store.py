from __future__ import annotations

import tempfile
import unittest
from os import environ
from pathlib import Path
from unittest.mock import patch

from fastapi import HTTPException

import lore_api.app as app_module
from lore_api.app import (
    FaceLockStoreRequest,
    action_file_dicts,
    build_face_lock_prompt,
    require_write_token,
)
from lore_api.face_lock_store import (
    CachedUpload,
    FaceLockStore,
    FaceLockStoreError,
    sha256_bytes,
)
from lore_api.indexer import SourceDocument, find_portrait_assets


PNG_BYTES = b"\x89PNG\r\n\x1a\nface-lock-test"
OTHER_PNG_BYTES = b"\x89PNG\r\n\x1a\nreplacement-face-lock"


class FakeResponse:
    def __init__(self, content: bytes) -> None:
        self.content = content
        self.headers = {"Content-Type": "image/png"}

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        return False

    def read(self, limit: int) -> bytes:
        return self.content[:limit]


class FaceLockStoreTests(unittest.TestCase):
    def test_write_token_is_required_and_compared(self) -> None:
        with patch.dict(environ, {}, clear=True):
            with self.assertRaises(HTTPException) as missing:
                require_write_token(None)
            self.assertEqual(missing.exception.status_code, 503)

        with patch.dict(environ, {"LORE_WRITE_TOKEN": "secret"}, clear=True):
            with self.assertRaises(HTTPException) as invalid:
                require_write_token("wrong")
            self.assertEqual(invalid.exception.status_code, 401)
            self.assertIsNone(require_write_token("secret"))

    def test_chatgpt_runtime_file_object_matches_string_openapi_field(self) -> None:
        payload = FaceLockStoreRequest.model_validate(
            {
                "openaiFileIdRefs": [
                    {
                        "name": "generated.png",
                        "id": "file-test",
                        "mime_type": "image/png",
                        "download_link": "https://files.oaiusercontent.com/generated.png",
                    }
                ],
            }
        )

        parsed = action_file_dicts(payload.openaiFileIdRefs)

        self.assertEqual(parsed[0]["id"], "file-test")
        self.assertEqual(parsed[0]["mime_type"], "image/png")

    def test_face_lock_request_requires_exactly_one_composite_file(self) -> None:
        file_ref = {
            "name": "generated.png",
            "id": "file-test",
            "mime_type": "image/png",
            "download_link": "https://files.oaiusercontent.com/generated.png",
        }

        with self.assertRaises(ValueError):
            FaceLockStoreRequest.model_validate({"openaiFileIdRefs": []})
        with self.assertRaises(ValueError):
            FaceLockStoreRequest.model_validate(
                {"openaiFileIdRefs": [file_ref, file_ref]}
            )

    def test_composite_prompt_requests_all_sections_without_text(self) -> None:
        prompt = build_face_lock_prompt("Test Character")

        for section in ("profile", "three-quarter", "front", "angry", "laughing"):
            self.assertIn(section, prompt)
        self.assertIn("exactly one image", prompt)
        self.assertIn("No title, labels, captions", prompt)

    def test_action_upload_is_cached_by_content_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            store = FaceLockStore(root)
            file_ref = {
                "download_link": "https://files.oaiusercontent.com/example.png",
                "mime_type": "image/png",
            }
            with patch(
                "lore_api.face_lock_store.urllib.request.urlopen",
                side_effect=lambda *args, **kwargs: FakeResponse(PNG_BYTES),
            ):
                first = store.cache_action_file(file_ref)
                second = store.cache_action_file(file_ref)

            self.assertFalse(first.cache_hit)
            self.assertTrue(second.cache_hit)
            self.assertEqual(first.path, second.path)
            self.assertEqual(first.sha256, sha256_bytes(PNG_BYTES))

    def test_action_upload_rejects_non_openai_host(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            store = FaceLockStore(Path(temporary))
            with self.assertRaises(FaceLockStoreError):
                store.cache_action_file(
                    {
                        "download_link": "https://example.com/face.png",
                        "mime_type": "image/png",
                    }
                )

    def test_repository_save_deduplicates_and_requires_explicit_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            store = FaceLockStore(root)
            cache = root / "cache"
            cache.mkdir()
            first_path = cache / "first.png"
            first_path.write_bytes(PNG_BYTES)
            first_upload = CachedUpload(
                path=first_path,
                sha256=sha256_bytes(PNG_BYTES),
                cache_hit=False,
            )

            destination, cache_hit = store.save_to_repository(
                character_folder="Test_Character",
                character_file_stem="Test_Character",
                variant="front",
                upload=first_upload,
            )
            same_destination, same_cache_hit = store.save_to_repository(
                character_folder="Test_Character",
                character_file_stem="Test_Character",
                variant="front",
                upload=first_upload,
            )
            self.assertFalse(cache_hit)
            self.assertTrue(same_cache_hit)
            self.assertEqual(destination, same_destination)

            replacement_path = cache / "replacement.png"
            replacement_path.write_bytes(OTHER_PNG_BYTES)
            replacement_upload = CachedUpload(
                path=replacement_path,
                sha256=sha256_bytes(OTHER_PNG_BYTES),
                cache_hit=False,
            )
            with self.assertRaises(FaceLockStoreError):
                store.save_to_repository(
                    character_folder="Test_Character",
                    character_file_stem="Test_Character",
                    variant="front",
                    upload=replacement_upload,
                )

            replaced, replaced_cache_hit = store.save_to_repository(
                character_folder="Test_Character",
                character_file_stem="Test_Character",
                variant="front",
                upload=replacement_upload,
                overwrite=True,
            )
            self.assertFalse(replaced_cache_hit)
            self.assertEqual(replaced.read_bytes(), OTHER_PNG_BYTES)

    def test_batch_preflight_does_not_partially_save_on_conflict(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            store = FaceLockStore(root)
            cache = root / "cache"
            cache.mkdir()
            front_path = cache / "front.png"
            front_path.write_bytes(PNG_BYTES)
            angle_path = cache / "angle.png"
            angle_path.write_bytes(OTHER_PNG_BYTES)
            front = CachedUpload(
                path=front_path,
                sha256=sha256_bytes(PNG_BYTES),
                cache_hit=False,
            )
            angle = CachedUpload(
                path=angle_path,
                sha256=sha256_bytes(OTHER_PNG_BYTES),
                cache_hit=False,
            )
            store.save_to_repository(
                character_folder="Test_Character",
                character_file_stem="Test_Character",
                variant="three_quarter",
                upload=front,
            )

            with self.assertRaises(FaceLockStoreError):
                store.save_many_to_repository(
                    character_folder="Test_Character",
                    character_file_stem="Test_Character",
                    variant_uploads=[("front", front), ("three_quarter", angle)],
                )

            front_destination = (
                root
                / "11_Медиа"
                / "Портреты_персонажей"
                / "Test_Character"
                / "Face_Lock"
                / "Test_Character_face_lock_front.png"
            )
            self.assertFalse(front_destination.exists())

    def test_indexer_classifies_nested_face_lock(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            character_path = root / "03_Персонажи" / "Test_Character.md"
            character_path.parent.mkdir(parents=True)
            character_path.write_text("# Test Character", encoding="utf-8")
            face_lock_path = (
                root
                / "11_Медиа"
                / "Портреты_персонажей"
                / "Test_Character"
                / "Face_Lock"
                / "Test_Character_face_lock_front.png"
            )
            face_lock_path.parent.mkdir(parents=True)
            face_lock_path.write_bytes(PNG_BYTES)
            document = SourceDocument(
                path=character_path,
                rel_path="03_Персонажи/Test_Character.md",
                title="Test Character",
                metadata={},
                body="# Test Character",
            )

            assets = find_portrait_assets(document, root)

            self.assertEqual(len(assets), 1)
            self.assertEqual(assets[0]["type"], "face_lock")
            self.assertEqual(assets[0]["priority"], "primary")

    def test_get_face_locks_omits_missing_repository_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            canonical_path = (
                root
                / "11_Медиа"
                / "Портреты_персонажей"
                / "Test"
                / "Test_основной_портрет.png"
            )
            canonical_path.parent.mkdir(parents=True)
            canonical_path.write_bytes(PNG_BYTES)

            document = {"title": "Test Character", "path": "03_Персонажи/Test_Character.md"}
            references = [
                {
                    "type": "face_lock",
                    "path": "11_Медиа/Портреты_персонажей/Test_Character/Face_Lock/Test_Character_face_lock_front.png",
                    "priority": "primary",
                    "description": "",
                },
                {
                    "type": "main_portrait",
                    "path": "11_Медиа/Портреты_персонажей/Test/Test_основной_портрет.png",
                    "priority": "primary",
                    "description": "",
                },
            ]

            class FakeConnection:
                def close(self) -> None:
                    return None

            with patch.object(app_module, "ROOT", root), patch.object(
                app_module,
                "character_document_and_references",
                return_value=(document, references),
            ), patch.object(
                app_module,
                "request_base_url",
                return_value="http://example.com",
            ), patch.object(
                app_module,
                "connect",
                return_value=FakeConnection(),
            ):
                response = app_module.get_face_locks("Test Character", request=None)  # type: ignore[arg-type]
                self.assertEqual(app_module.face_lock_references(references), [])
            self.assertFalse(response["has_face_lock"])
            self.assertEqual(response["face_locks"], [])
            self.assertEqual(
                response["required_sections"],
                ["profile", "three_quarter", "front", "anger", "laughter"],
            )
            self.assertEqual(response["recommended_variants"], ["composite"])
            self.assertEqual(list(response["generation_prompts"]), ["composite"])


if __name__ == "__main__":
    unittest.main()
