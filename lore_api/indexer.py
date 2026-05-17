from __future__ import annotations

import argparse
import json
import os
import re
import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


ROOT = Path(os.environ.get("LORE_REPO_ROOT", Path(__file__).resolve().parents[1])).resolve()
DB_PATH = Path(os.environ.get("LORE_INDEX_DB", ROOT / "lore_api" / ".data" / "lore_index.sqlite3")).resolve()

IGNORED_DIRS = {
    ".git",
    ".obsidian",
    ".venv",
    "__pycache__",
    ".data",
    "Все_MD_файлы",
}


@dataclass(frozen=True)
class SourceDocument:
    path: Path
    rel_path: str
    title: str
    metadata: dict[str, str]
    body: str


def normalize_text(value: str) -> str:
    value = value.lower().replace("ё", "е")
    value = re.sub(r"[^\w\s-]+", " ", value, flags=re.UNICODE)
    return re.sub(r"\s+", " ", value).strip()


def slugify(value: str) -> str:
    normalized = normalize_text(value)
    normalized = normalized.replace(" ", "_").replace("-", "_")
    normalized = re.sub(r"_+", "_", normalized)
    return normalized.strip("_")


def read_markdown_files(root: Path = ROOT) -> Iterable[Path]:
    for path in root.rglob("*.md"):
        rel_parts = path.relative_to(root).parts
        if any(part in IGNORED_DIRS for part in rel_parts):
            continue
        if any(part.startswith(".") for part in rel_parts):
            continue
        yield path


def parse_front_matter(text: str) -> tuple[dict[str, str], str]:
    lines = text.splitlines()
    if len(lines) < 3:
        return {}, text

    if lines and lines[0].startswith("\ufeff"):
        lines[0] = lines[0].lstrip("\ufeff")

    start_index = None
    for index, line in enumerate(lines[:12]):
        if line.strip() == "---":
            start_index = index
            break

    if start_index is None:
        return {}, "\n".join(lines)

    metadata: dict[str, str] = {}
    end_index = None
    for index in range(start_index + 1, len(lines)):
        if lines[index].strip() == "---":
            end_index = index
            break
        if ":" not in lines[index]:
            continue
        key, raw_value = lines[index].split(":", 1)
        metadata[key.strip()] = raw_value.strip().strip('"').strip("'")

    if end_index is None:
        return {}, "\n".join(lines)

    body_lines = lines[:start_index] + lines[end_index + 1 :]
    return metadata, "\n".join(body_lines).strip()


def extract_title(body: str, path: Path) -> str:
    for line in body.splitlines():
        line = line.strip()
        if line.startswith("# "):
            return line[2:].strip()
    return path.stem.replace("_", " ")


def load_document(path: Path, root: Path = ROOT) -> SourceDocument:
    text = path.read_text(encoding="utf-8-sig")
    metadata, body = parse_front_matter(text)
    title = extract_title(body, path)
    rel_path = path.relative_to(root).as_posix()
    return SourceDocument(path=path, rel_path=rel_path, title=title, metadata=metadata, body=body)


def infer_entity_type(rel_path: str, metadata: dict[str, str]) -> str:
    if metadata.get("type"):
        return metadata["type"]
    if rel_path.startswith("03_Персонажи/"):
        return "character"
    if rel_path.startswith("04_Локации/"):
        return "location"
    if rel_path.startswith("02_Лор/"):
        return "lore"
    if rel_path.startswith("01_Кампания/Ветки/"):
        return "scene"
    if rel_path.startswith("01_Кампания/"):
        return "campaign"
    if rel_path.startswith("11_Медиа/"):
        return "media"
    return "document"


def iter_chunks(body: str, max_chars: int = 1800, overlap: int = 220) -> Iterable[tuple[str, str]]:
    current_heading = ""
    current = ""

    def flush() -> Iterable[tuple[str, str]]:
        nonlocal current
        if current:
            yield current_heading, current
            current = ""

    for raw_block in re.split(r"\n\s*\n", body):
        block = raw_block.strip()
        if not block:
            continue
        if block.startswith("#"):
            yield from flush()
            current_heading = block.lstrip("#").strip()
            continue

        candidate = f"{current}\n\n{block}".strip() if current else block
        if len(candidate) <= max_chars:
            current = candidate
            continue
        if current:
            yield current_heading, current
            current = current[-overlap:].strip()
        if len(block) <= max_chars:
            current = f"{current}\n\n{block}".strip() if current else block
            continue
        for start in range(0, len(block), max_chars - overlap):
            piece = block[start : start + max_chars].strip()
            if piece:
                yield current_heading, piece
        current = ""

    if current:
        yield current_heading, current


def find_portrait_assets(doc: SourceDocument, root: Path = ROOT) -> list[dict[str, str]]:
    assets: list[dict[str, str]] = []
    portrait = doc.metadata.get("portrait")
    if portrait and portrait.lower() not in {"null", "none", "-"}:
        assets.append({"type": "main_portrait", "path": portrait, "priority": "primary"})

    portrait_dir = root / "11_Медиа" / "Портреты_персонажей" / doc.path.stem
    if portrait_dir.exists():
        for image_path in sorted(portrait_dir.iterdir()):
            if image_path.suffix.lower() not in {".png", ".jpg", ".jpeg", ".webp"}:
                continue
            rel_path = image_path.relative_to(root).as_posix()
            if all(asset["path"] != rel_path for asset in assets):
                priority = "primary" if "основной_портрет" in image_path.name else "secondary"
                assets.append({"type": "portrait", "path": rel_path, "priority": priority})

        prompt_path = portrait_dir / "Промпт_портрета.md"
        if prompt_path.exists():
            assets.append(
                {
                    "type": "portrait_prompt",
                    "path": prompt_path.relative_to(root).as_posix(),
                    "priority": "support",
                }
            )

    return assets


def connect(db_path: Path = DB_PATH) -> sqlite3.Connection:
    db_path.parent.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(db_path)
    connection.row_factory = sqlite3.Row
    return connection


def init_schema(connection: sqlite3.Connection) -> None:
    connection.executescript(
        """
        PRAGMA journal_mode=WAL;

        DROP TABLE IF EXISTS documents;
        DROP TABLE IF EXISTS chunks;
        DROP TABLE IF EXISTS aliases;
        DROP TABLE IF EXISTS asset_refs;
        DROP TABLE IF EXISTS chunks_fts;

        CREATE TABLE documents (
            id INTEGER PRIMARY KEY,
            path TEXT NOT NULL UNIQUE,
            title TEXT NOT NULL,
            title_norm TEXT NOT NULL,
            slug TEXT NOT NULL,
            entity_type TEXT NOT NULL,
            metadata_json TEXT NOT NULL
        );

        CREATE TABLE chunks (
            id INTEGER PRIMARY KEY,
            document_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
            path TEXT NOT NULL,
            title TEXT NOT NULL,
            heading TEXT NOT NULL,
            entity_type TEXT NOT NULL,
            text TEXT NOT NULL
        );

        CREATE VIRTUAL TABLE chunks_fts USING fts5(
            title,
            heading,
            text,
            path UNINDEXED,
            content='chunks',
            content_rowid='id',
            tokenize='unicode61'
        );

        CREATE TABLE aliases (
            alias_norm TEXT NOT NULL,
            document_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
            alias TEXT NOT NULL,
            PRIMARY KEY (alias_norm, document_id)
        );

        CREATE TABLE asset_refs (
            id INTEGER PRIMARY KEY,
            document_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
            ref_type TEXT NOT NULL,
            path TEXT NOT NULL,
            priority TEXT NOT NULL,
            description TEXT NOT NULL DEFAULT ''
        );
        """
    )


def insert_document(connection: sqlite3.Connection, doc: SourceDocument, root: Path = ROOT) -> int:
    entity_type = infer_entity_type(doc.rel_path, doc.metadata)
    cursor = connection.execute(
        """
        INSERT INTO documents(path, title, title_norm, slug, entity_type, metadata_json)
        VALUES (?, ?, ?, ?, ?, ?)
        """,
        (
            doc.rel_path,
            doc.title,
            normalize_text(doc.title),
            slugify(doc.title),
            entity_type,
            json.dumps(doc.metadata, ensure_ascii=False, sort_keys=True),
        ),
    )
    document_id = int(cursor.lastrowid)

    aliases = {doc.title, doc.path.stem.replace("_", " ")}
    if doc.metadata.get("role"):
        aliases.add(doc.metadata["role"])
    for alias in aliases:
        norm = normalize_text(alias)
        if norm:
            connection.execute(
                "INSERT OR IGNORE INTO aliases(alias_norm, document_id, alias) VALUES (?, ?, ?)",
                (norm, document_id, alias),
            )

    for heading, text in iter_chunks(doc.body):
        cursor = connection.execute(
            """
            INSERT INTO chunks(document_id, path, title, heading, entity_type, text)
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            (document_id, doc.rel_path, doc.title, heading, entity_type, text),
        )
        chunk_id = int(cursor.lastrowid)
        connection.execute(
            "INSERT INTO chunks_fts(rowid, title, heading, text, path) VALUES (?, ?, ?, ?, ?)",
            (chunk_id, doc.title, heading, text, doc.rel_path),
        )

    if entity_type == "character":
        for asset in find_portrait_assets(doc, root):
            connection.execute(
                """
                INSERT INTO asset_refs(document_id, ref_type, path, priority, description)
                VALUES (?, ?, ?, ?, ?)
                """,
                (
                    document_id,
                    asset["type"],
                    asset["path"],
                    asset["priority"],
                    "visual reference for image generation",
                ),
            )

    return document_id


def rebuild_index(root: Path = ROOT, db_path: Path = DB_PATH) -> dict[str, int | str]:
    connection = connect(db_path)
    try:
        init_schema(connection)
        document_count = 0
        for path in sorted(read_markdown_files(root)):
            doc = load_document(path, root)
            insert_document(connection, doc, root)
            document_count += 1
        chunk_count = connection.execute("SELECT COUNT(*) FROM chunks").fetchone()[0]
        reference_count = connection.execute("SELECT COUNT(*) FROM asset_refs").fetchone()[0]
        connection.commit()
        return {
            "documents": document_count,
            "chunks": int(chunk_count),
            "references": int(reference_count),
            "db_path": str(db_path),
        }
    finally:
        connection.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="Build the lore search index.")
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--db", type=Path, default=DB_PATH)
    args = parser.parse_args()

    result = rebuild_index(args.root.resolve(), args.db.resolve())
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
