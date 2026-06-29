from __future__ import annotations

import base64
import json
import re
import sqlite3
from pathlib import Path
from typing import Annotated, Any, Optional

from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.openapi.utils import get_openapi
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field

from .indexer import DB_PATH, ROOT, make_file_id, rebuild_index, slugify, normalize_text


RESPONSE_CACHE: dict[tuple[Any, ...], dict] = {}
CACHE_GENERATION = 0


app = FastAPI(
    title="World of Might and Magic Lore API",
    version="0.1.0",
    openapi_url=None,
    description=(
        "Read-only lore retrieval API for Markdown canon, character cards, "
        "portrait references, and scene context."
    ),
)


class HealthResponse(BaseModel):
    status: str
    index_exists: bool
    db_path: str
    cache_entries: int
    cache_generation: int


class ReindexResponse(BaseModel):
    status: str
    documents: int
    chunks: int
    references: int
    db_path: str


class ChunkResult(BaseModel):
    id: int
    path: str
    title: str
    heading: str
    entity_type: str
    text: str
    score: Optional[float] = None
    source_chunk_id: Optional[int] = None
    chunk_index: Optional[int] = None
    chunk_total: Optional[int] = None


class SearchResponse(BaseModel):
    query: str
    results: list[ChunkResult]


class AssetReference(BaseModel):
    type: str
    path: str
    url: Optional[str] = None
    download_url: Optional[str] = None
    priority: str
    description: str = ""


class CharacterResponse(BaseModel):
    id: str
    name: str
    type: str
    path: str
    metadata: dict[str, Any] = Field(default_factory=dict)
    references: list[AssetReference]
    context: list[ChunkResult]


class ReferencesResponse(BaseModel):
    character: str
    path: str
    references: list[AssetReference]


class SceneCharacter(BaseModel):
    name: str
    found: bool
    path: Optional[str] = None
    metadata: dict[str, Any] = Field(default_factory=dict)
    references: list[AssetReference] = Field(default_factory=list)
    context: list[ChunkResult] = Field(default_factory=list)


class SceneSource(BaseModel):
    request: str
    found: bool
    title: Optional[str] = None
    path: Optional[str] = None
    branch: Optional[str] = None
    metadata: dict[str, Any] = Field(default_factory=dict)
    context: list[ChunkResult] = Field(default_factory=list)
    branch_context: list[ChunkResult] = Field(default_factory=list)


class SceneMatch(BaseModel):
    title: str
    path: str
    participants: list[str] = Field(default_factory=list)
    participant_overlap: int = 0
    score: float = 0.0
    scene_characters: list[SceneCharacter] = Field(default_factory=list)
    context: list[ChunkResult] = Field(default_factory=list)


class SceneContextResponse(BaseModel):
    characters: list[SceneCharacter]
    location: Optional[str] = None
    scene_matches: list[SceneMatch] = Field(default_factory=list)
    related_context: list[ChunkResult]
    cursor: Optional[str] = None
    next_cursor: Optional[str] = None
    has_more: bool = False
    page_size: int = 0
    total_related_context: int = 0


def request_base_url(request: Request) -> str:
    forwarded_proto = request.headers.get("x-forwarded-proto")
    forwarded_host = request.headers.get("x-forwarded-host") or request.headers.get("host")
    if forwarded_proto and forwarded_host:
        return f"{forwarded_proto}://{forwarded_host}".rstrip("/")
    return str(request.base_url).rstrip("/")


def build_openapi_schema(server_url: str) -> dict:
    schema = get_openapi(
        title=app.title,
        version=app.version,
        description=app.description,
        routes=app.routes,
    )
    schema["servers"] = [{"url": server_url}]
    return schema


@app.get("/openapi.json", include_in_schema=False)
def openapi_json(request: Request) -> dict:
    return build_openapi_schema(request_base_url(request))


def index_has_required_tables(connection: sqlite3.Connection) -> bool:
    required_tables = {"documents", "chunks", "aliases", "asset_refs", "chunks_fts", "scene_participants", "file_links"}
    rows = connection.execute(
        """
        SELECT name
        FROM sqlite_master
        WHERE type IN ('table', 'view')
        """
    ).fetchall()
    existing = {row["name"] for row in rows}
    return required_tables.issubset(existing)


def connect() -> sqlite3.Connection:
    if not DB_PATH.exists():
        rebuild_index()
    connection = sqlite3.connect(DB_PATH)
    connection.row_factory = sqlite3.Row
    if not index_has_required_tables(connection):
        connection.close()
        rebuild_index()
        connection = sqlite3.connect(DB_PATH)
        connection.row_factory = sqlite3.Row
    return connection


def row_to_dict(row: sqlite3.Row) -> dict:
    return {key: row[key] for key in row.keys()}


def dump_chunk(chunk: Any) -> dict:
    if hasattr(chunk, "model_dump"):
        return chunk.model_dump()
    return dict(chunk)


def split_text_for_scene_context(text: str, max_chars: int) -> list[str]:
    cleaned = (text or "").strip()
    if not cleaned or len(cleaned) <= max_chars:
        return [cleaned] if cleaned else []

    paragraphs = [part.strip() for part in re.split(r"\n\s*\n", cleaned) if part.strip()]
    if not paragraphs:
        paragraphs = [cleaned]

    parts: list[str] = []
    current = ""

    def flush_current() -> None:
        nonlocal current
        if current.strip():
            parts.append(current.strip())
        current = ""

    def append_piece(piece: str) -> None:
        nonlocal current
        if not current:
            current = piece
            return
        candidate = f"{current}\n\n{piece}"
        if len(candidate) <= max_chars:
            current = candidate
            return
        flush_current()
        current = piece

    for paragraph in paragraphs:
        if len(paragraph) <= max_chars:
            append_piece(paragraph)
            continue

        sentences = [
            sentence.strip()
            for sentence in re.split(r"(?<=[.!?…])\s+", paragraph)
            if sentence.strip()
        ]
        if not sentences:
            sentences = [paragraph]

        sentence_buffer = ""
        for sentence in sentences:
            if len(sentence) > max_chars:
                flush_current()
                if sentence_buffer:
                    parts.append(sentence_buffer.strip())
                    sentence_buffer = ""
                for start in range(0, len(sentence), max_chars):
                    parts.append(sentence[start : start + max_chars].strip())
                continue

            candidate = f"{sentence_buffer} {sentence}".strip() if sentence_buffer else sentence
            if len(candidate) <= max_chars:
                sentence_buffer = candidate
                continue

            if sentence_buffer:
                append_piece(sentence_buffer.strip())
            sentence_buffer = sentence

        if sentence_buffer:
            append_piece(sentence_buffer.strip())

    flush_current()
    return parts


def split_chunk_for_scene_context(chunk: dict[str, Any], max_chars: int) -> list[dict[str, Any]]:
    chunk_data = dump_chunk(chunk)
    text_parts = split_text_for_scene_context(str(chunk_data.get("text", "")), max_chars)
    if len(text_parts) <= 1:
        if text_parts:
            chunk_data["text"] = text_parts[0]
        return [chunk_data]

    source_chunk_id = int(chunk_data["id"])
    chunk_total = len(text_parts)
    split_chunks: list[dict[str, Any]] = []
    for index, text_part in enumerate(text_parts, start=1):
        split_chunk = dict(chunk_data)
        split_chunk["text"] = text_part
        split_chunk["source_chunk_id"] = source_chunk_id
        split_chunk["chunk_index"] = index
        split_chunk["chunk_total"] = chunk_total
        split_chunks.append(split_chunk)
    return split_chunks


def split_chunks_for_scene_context(chunks: list[dict[str, Any]], max_chars: int) -> list[dict[str, Any]]:
    split_chunks: list[dict[str, Any]] = []
    for chunk in chunks:
        split_chunks.extend(split_chunk_for_scene_context(chunk, max_chars))
    return split_chunks


def encode_scene_context_cursor(offset: int) -> str:
    payload = json.dumps({"offset": max(0, offset)}, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return base64.urlsafe_b64encode(payload).decode("ascii").rstrip("=")


def decode_scene_context_cursor(cursor: Optional[str]) -> int:
    if not cursor:
        return 0
    padded = cursor + "=" * (-len(cursor) % 4)
    try:
        payload = base64.urlsafe_b64decode(padded.encode("ascii")).decode("utf-8")
        data = json.loads(payload)
        offset = int(data.get("offset", 0))
        return max(0, offset)
    except (ValueError, TypeError, json.JSONDecodeError) as exc:
        raise HTTPException(status_code=400, detail="Invalid cursor") from exc


def paginate_scene_context_chunks(chunks: list[dict[str, Any]], page_size: int, cursor: Optional[str]) -> dict[str, Any]:
    offset = decode_scene_context_cursor(cursor)
    total = len(chunks)
    if offset > total:
        raise HTTPException(status_code=400, detail="Cursor is out of range")
    page = chunks[offset : offset + page_size]
    next_offset = offset + len(page)
    has_more = next_offset < total
    return {
        "cursor": cursor,
        "next_cursor": encode_scene_context_cursor(next_offset) if has_more else None,
        "has_more": has_more,
        "page_size": page_size,
        "total_related_context": total,
        "related_context": page,
    }


def split_character_names(value: Optional[str]) -> list[str]:
    if not value:
        return []
    parts = re.split(r"[,\n;/]+", value)
    return [part.strip().strip(" .;,:") for part in parts if part.strip().strip(" .;,:")]


def cache_get(key: tuple[Any, ...]) -> Optional[dict]:
    return RESPONSE_CACHE.get((CACHE_GENERATION, *key))


def cache_set(key: tuple[Any, ...], value: dict) -> dict:
    RESPONSE_CACHE[(CACHE_GENERATION, *key)] = value
    return value


def clear_response_cache() -> None:
    global CACHE_GENERATION
    RESPONSE_CACHE.clear()
    CACHE_GENERATION += 1


def make_fts_query(query: str) -> str:
    tokens = re.findall(r"[\w-]+", normalize_text(query), flags=re.UNICODE)
    tokens = [token for token in tokens if len(token) > 1]
    if not tokens:
        raise HTTPException(status_code=400, detail="Query must contain searchable text")
    return " OR ".join(f'"{token}"*' for token in tokens[:12])


def normalize_repo_path(value: str) -> str:
    path = value.strip().replace("\\", "/")
    if path.startswith("./"):
        path = path[2:]
    if path.startswith(str(ROOT).replace("\\", "/")):
        path = path[len(str(ROOT).replace("\\", "/")) :].lstrip("/")
    return path.strip("/")


def find_document_by_path(connection: sqlite3.Connection, path: str) -> sqlite3.Row | None:
    normalized = normalize_repo_path(path)
    if not normalized:
        return None
    return connection.execute(
        """
        SELECT *
        FROM documents
        WHERE path = ?
        LIMIT 1
        """,
        (normalized,),
    ).fetchone()


def find_document(connection: sqlite3.Connection, name: str, entity_type: str | None = None) -> sqlite3.Row | None:
    norm = normalize_text(name)
    slug = slugify(name)
    filters = []
    params: list[str] = []
    if entity_type:
        filters.append("d.entity_type = ?")
        params.append(entity_type)
    where_entity = f"AND {' AND '.join(filters)}" if filters else ""

    row = connection.execute(
        f"""
        SELECT d.*
        FROM documents d
        JOIN aliases a ON a.document_id = d.id
        WHERE a.alias_norm = ? {where_entity}
        ORDER BY CASE WHEN d.title_norm = ? THEN 0 ELSE 1 END, length(d.title)
        LIMIT 1
        """,
        [norm, *params, norm],
    ).fetchone()
    if row:
        return row

    row = connection.execute(
        f"""
        SELECT d.*
        FROM documents d
        WHERE (d.slug = ? OR d.title_norm LIKE ?) {where_entity}
        ORDER BY CASE WHEN d.slug = ? THEN 0 ELSE 1 END, length(d.title)
        LIMIT 1
        """,
        [slug, f"%{norm}%", *params, slug],
    ).fetchone()
    return row


def get_chunks_for_document(connection: sqlite3.Connection, document_id: int, limit: int = 8) -> list[dict]:
    rows = connection.execute(
        """
        SELECT id, path, title, heading, entity_type, text
        FROM chunks
        WHERE document_id = ?
        ORDER BY id
        LIMIT ?
        """,
        (document_id, limit),
    ).fetchall()
    return [row_to_dict(row) for row in rows]


def get_section_chunks_for_document(
    connection: sqlite3.Connection,
    document_id: int,
    section_name: str,
    limit: int = 8,
) -> list[dict]:
    rows = connection.execute(
        """
        SELECT id, path, title, heading, entity_type, text
        FROM chunks
        WHERE document_id = ? AND LOWER(heading) = LOWER(?)
        ORDER BY id
        LIMIT ?
        """,
        (document_id, section_name, limit),
    ).fetchall()
    return [row_to_dict(row) for row in rows]


def get_references_for_document(connection: sqlite3.Connection, document_id: int) -> list[dict]:
    rows = connection.execute(
        """
        SELECT ref_type AS type, path, priority, description
        FROM asset_refs
        WHERE document_id = ?
        ORDER BY
            CASE priority WHEN 'primary' THEN 0 WHEN 'secondary' THEN 1 ELSE 2 END,
            path
        """,
        (document_id,),
    ).fetchall()
    return [row_to_dict(row) for row in rows]


def get_scene_documents_in_branch(connection: sqlite3.Connection, scene_path: str) -> list[sqlite3.Row]:
    if not scene_path.startswith("01_Кампания/Ветки/"):
        return []
    branch_dir = str(Path(scene_path).parent).replace("\\", "/")
    rows = connection.execute(
        """
        SELECT *
        FROM documents
        WHERE entity_type = 'scene' AND path LIKE ?
        ORDER BY path
        """,
        (f"{branch_dir}/%",),
    ).fetchall()
    return list(rows)


def get_branch_context_for_scene(
    connection: sqlite3.Connection,
    scene_document: sqlite3.Row,
    limit: int,
) -> list[dict]:
    branch_documents = get_scene_documents_in_branch(connection, scene_document["path"])
    if not branch_documents:
        return []

    try:
        current_index = next(
            index for index, document in enumerate(branch_documents) if document["id"] == scene_document["id"]
        )
    except StopIteration:
        return []

    previous_document = branch_documents[current_index - 1] if current_index > 0 else None
    next_document = branch_documents[current_index + 1] if current_index + 1 < len(branch_documents) else None

    current_limit = min(limit, 8)
    neighbor_limit = max(1, (limit - current_limit) // 2)

    chunks: list[dict] = []
    for document, doc_limit in (
        (previous_document, neighbor_limit),
        (scene_document, current_limit),
        (next_document, neighbor_limit),
    ):
        if document is None or doc_limit <= 0:
            continue
        chunks.extend(get_section_chunks_for_document(connection, int(document["id"]), "Событие", limit=doc_limit))

    deduped: list[dict] = []
    seen: set[tuple[str, str, str]] = set()
    for chunk in chunks:
        key = (chunk["path"], chunk["heading"], chunk["text"])
        if key in seen:
            continue
        seen.add(key)
        deduped.append(chunk)
        if len(deduped) >= limit:
            break
    return deduped


def with_file_urls(references: list[dict], base_url: str) -> list[dict]:
    payload = []
    for reference in references:
        item = dict(reference)
        item["url"] = f"{base_url}/files/{make_file_id(item['path'])}"
        item["download_url"] = item["url"]
        payload.append(item)
    return payload


@app.get("/health", response_model=HealthResponse)
def health() -> dict:
    return {
        "status": "ok",
        "index_exists": DB_PATH.exists(),
        "db_path": str(DB_PATH),
        "cache_entries": len(RESPONSE_CACHE),
        "cache_generation": CACHE_GENERATION,
    }


@app.post("/reindex", response_model=ReindexResponse)
def reindex() -> dict:
    clear_response_cache()
    return {"status": "rebuilt", **rebuild_index()}


@app.get("/search", response_model=SearchResponse)
def search_lore(
    q: Annotated[str, Query(min_length=2, description="Search query in Russian or English.")],
    limit: Annotated[int, Query(ge=1, le=25)] = 8,
    doc_type: Annotated[
        Optional[str],
        Query(alias="type", description="Optional document type filter, e.g. character, location, scene."),
    ] = None,
) -> dict:
    cache_key = ("search", q, limit, doc_type)
    cached = cache_get(cache_key)
    if cached is not None:
        return cached
    fts_query = make_fts_query(q)
    connection = connect()
    try:
        params: list[object] = [fts_query]
        type_clause = ""
        if doc_type:
            type_clause = "AND c.entity_type = ?"
            params.append(doc_type)
        params.append(limit)
        rows = connection.execute(
            f"""
            SELECT c.id, c.path, c.title, c.heading, c.entity_type, c.text,
                   bm25(chunks_fts) AS score
            FROM chunks_fts
            JOIN chunks c ON c.id = chunks_fts.rowid
            WHERE chunks_fts MATCH ? {type_clause}
            ORDER BY score
            LIMIT ?
            """,
            params,
        ).fetchall()
        return cache_set(cache_key, {"query": q, "results": [row_to_dict(row) for row in rows]})
    finally:
        connection.close()


@app.get("/characters/{name}", response_model=CharacterResponse)
def get_character(name: str, request: Request) -> dict:
    base_url = request_base_url(request)
    cache_key = ("character", base_url, name)
    cached = cache_get(cache_key)
    if cached is not None:
        return cached
    connection = connect()
    try:
        document = find_document(connection, name, "character")
        if not document:
            raise HTTPException(status_code=404, detail=f"Character not found: {name}")
        metadata = json.loads(document["metadata_json"])
        return cache_set(cache_key, {
            "id": document["slug"],
            "name": document["title"],
            "type": document["entity_type"],
            "path": document["path"],
            "metadata": metadata,
            "references": with_file_urls(
                get_references_for_document(connection, int(document["id"])),
                base_url,
            ),
            "context": get_chunks_for_document(connection, int(document["id"])),
        })
    finally:
        connection.close()


@app.get("/references/{character}", response_model=ReferencesResponse)
def get_references(character: str, request: Request) -> dict:
    base_url = request_base_url(request)
    cache_key = ("references", base_url, character)
    cached = cache_get(cache_key)
    if cached is not None:
        return cached
    connection = connect()
    try:
        document = find_document(connection, character, "character")
        if not document:
            raise HTTPException(status_code=404, detail=f"Character not found: {character}")
        return cache_set(cache_key, {
            "character": document["title"],
            "path": document["path"],
            "references": with_file_urls(
                get_references_for_document(connection, int(document["id"])),
                base_url,
            ),
        })
    finally:
        connection.close()


def get_scene_participants_for_document(connection: sqlite3.Connection, document_id: int) -> list[str]:
    rows = connection.execute(
        """
        SELECT participant
        FROM scene_participants
        WHERE document_id = ?
        ORDER BY id
        """,
        (document_id,),
    ).fetchall()
    return [row["participant"] for row in rows]


def collect_scene_participant_names(candidates: list[dict]) -> list[str]:
    names: list[str] = []
    seen: set[str] = set()
    for candidate in candidates:
        for participant in candidate.get("participants", []):
            normalized = normalize_text(participant)
            if not normalized or normalized in seen:
                continue
            seen.add(normalized)
            names.append(participant)
    return names


def build_scene_character_payload(
    connection: sqlite3.Connection,
    base_url: str,
    name: str,
    include_details: bool = True,
) -> dict:
    document = find_document(connection, name, "character")
    if not document:
        return {"name": name, "found": False}
    payload = {
        "name": document["title"],
        "found": True,
        "path": document["path"],
    }
    if include_details:
        metadata = json.loads(document["metadata_json"])
        payload.update(
            {
                "metadata": metadata,
                "references": with_file_urls(
                    get_references_for_document(connection, int(document["id"])),
                    base_url,
                ),
                "context": get_chunks_for_document(connection, int(document["id"])),
            }
        )
    return payload


def participant_matches_request(requested_norm: str, participant_norm: str) -> bool:
    if not requested_norm or not participant_norm:
        return False
    if requested_norm == participant_norm:
        return True
    if requested_norm in participant_norm or participant_norm in requested_norm:
        return True

    requested_tokens = [token for token in requested_norm.split() if len(token) > 2]
    participant_tokens = set(participant_norm.split())
    if not requested_tokens:
        return False
    return all(token in participant_tokens for token in requested_tokens)


def count_matching_participants(requested_names: list[str], participant_norms: set[str]) -> int:
    matched_requests: set[str] = set()
    for requested in requested_names:
        for participant_norm in participant_norms:
            if participant_matches_request(requested, participant_norm):
                matched_requests.add(requested)
                break
    return len(matched_requests)


def get_scene_candidates(
    connection: sqlite3.Connection,
    participant_names: list[str],
    scene_query: str,
    limit: int,
) -> list[dict]:
    normalized_requested = [normalize_text(name) for name in participant_names if normalize_text(name)]

    scene_rows = connection.execute(
        """
        SELECT d.id, d.path, d.title, d.metadata_json, sp.participant, sp.participant_norm
        FROM documents d
        LEFT JOIN scene_participants sp ON sp.document_id = d.id
        WHERE d.entity_type = 'scene'
        ORDER BY d.path, sp.id
        """,
    ).fetchall()

    grouped: dict[int, dict[str, Any]] = {}
    for row in scene_rows:
        document_id = int(row["id"])
        record = grouped.setdefault(
            document_id,
            {
                "id": document_id,
                "path": row["path"],
                "title": row["title"],
                "metadata_json": row["metadata_json"],
                "participants": [],
                "participant_norms": set(),
            },
        )
        if row["participant"]:
            record["participants"].append(row["participant"])
            if row["participant_norm"]:
                record["participant_norms"].add(row["participant_norm"])

    text_scores: dict[int, float] = {}
    if scene_query.strip():
        try:
            fts_query = make_fts_query(scene_query)
            rank_rows = connection.execute(
                """
                SELECT c.document_id, bm25(chunks_fts) AS score
                FROM chunks_fts
                JOIN chunks c ON c.id = chunks_fts.rowid
                WHERE chunks_fts MATCH ? AND c.entity_type = 'scene'
                ORDER BY score
                LIMIT ?
                """,
                (fts_query, max(20, limit * 20)),
            ).fetchall()
            for index, row in enumerate(rank_rows):
                document_id = int(row["document_id"])
                rank_score = 1.0 / (1.0 + index)
                current = text_scores.get(document_id, 0.0)
                if rank_score > current:
                    text_scores[document_id] = rank_score
        except HTTPException:
            text_scores = {}

    candidates: list[dict] = []
    for record in grouped.values():
        participant_norms = record["participant_norms"]
        overlap = (
            count_matching_participants(normalized_requested, participant_norms)
            if normalized_requested
            else len(participant_norms)
        )
        if normalized_requested and overlap == 0:
            continue

        participant_score = overlap / max(1, len(normalized_requested)) if normalized_requested else 0.0
        text_score = text_scores.get(record["id"], 0.0)
        score = (participant_score * 2.0) + text_score
        candidates.append(
            {
                "id": record["id"],
                "path": record["path"],
                "title": record["title"],
                "metadata_json": record["metadata_json"],
                "participants": record["participants"],
                "participant_overlap": overlap,
                "score": score,
            }
        )

    candidates.sort(key=lambda item: (-item["score"], item["path"]))
    return candidates[:limit]


def build_scene_character_payloads(
    connection: sqlite3.Connection,
    base_url: str,
    names: list[str],
) -> list[dict]:
    payloads: list[dict] = []
    seen: set[str] = set()
    for name in names:
        normalized = normalize_text(name)
        if not normalized or normalized in seen:
            continue
        seen.add(normalized)
        payloads.append(build_scene_character_payload(connection, base_url, name))
    return payloads


def build_scene_character_summaries(
    connection: sqlite3.Connection,
    base_url: str,
    names: list[str],
) -> list[dict]:
    payloads: list[dict] = []
    seen: set[str] = set()
    for name in names:
        normalized = normalize_text(name)
        if not normalized or normalized in seen:
            continue
        seen.add(normalized)
        payloads.append(build_scene_character_payload(connection, base_url, name, include_details=False))
    return payloads


def build_scene_match_payloads(
    connection: sqlite3.Connection,
    base_url: str,
    candidates: list[dict],
    limit: int,
    chunk_size: int,
) -> list[dict]:
    payloads: list[dict] = []
    for candidate in candidates[:limit]:
        participant_names = candidate["participants"]
        payloads.append(
            {
                "title": candidate["title"],
                "path": candidate["path"],
                "participants": participant_names,
                "participant_overlap": candidate["participant_overlap"],
                "score": candidate["score"],
                "scene_characters": build_scene_character_summaries(connection, base_url, participant_names),
                "context": split_chunks_for_scene_context(
                    get_section_chunks_for_document(connection, int(candidate["id"]), "Событие", limit=1),
                    chunk_size,
                ),
            }
        )
    return payloads


@app.get("/factions/{name}", response_model=SearchResponse)
def get_faction(name: str) -> dict:
    cache_key = ("faction", name)
    cached = cache_get(cache_key)
    if cached is not None:
        return cached
    query = f"{name} фракция государство дом орден"
    return cache_set(cache_key, search_lore(query, limit=10, doc_type="lore"))


@app.get("/scene-context", include_in_schema=False)
def get_scene_context(
    request: Request,
    characters: Annotated[str, Query(description="Comma-separated character names.")] = "",
    location: Annotated[str, Query(description="Optional location name.")] = "",
    scene_query: Annotated[str, Query(description="Freeform scene description to match against scene context.")] = "",
    limit: Annotated[int, Query(ge=1, le=20)] = 12,
    page_size: Annotated[int, Query(ge=1, le=50, description="Number of related context chunks to return in this page.")] = 5,
    cursor: Annotated[Optional[str], Query(description="Opaque cursor returned by a previous /scene-context response.")] = None,
    chunk_size: Annotated[int, Query(ge=300, le=8000, description="Maximum characters per scene-context text chunk.")] = 1200,
) -> dict:
    base_url = request_base_url(request)
    cache_key = ("scene-context", base_url, characters, location, scene_query, limit, page_size, cursor, chunk_size)
    cached = cache_get(cache_key)
    if cached is not None:
        return cached
    characters = characters or ""
    location = location or ""
    scene_query = scene_query or ""
    names = split_character_names(characters)
    character_payloads: list[dict] = []
    search_terms: list[str] = []
    candidate_matches: list[dict] = []
    combined_query = (scene_query or " ".join(search_terms) or " ".join(names) or characters or location or "").strip()
    match_payloads: list[dict] = []
    best_match_context: list[dict] = []
    related_context: list[dict] = []

    try:
        connection = connect()
        try:
            for name in names:
                document = find_document(connection, name, "character")
                if not document:
                    character_payloads.append({"name": name, "found": False})
                    search_terms.append(name)
                    continue
                character_payloads.append(
                    {
                        "name": document["title"],
                        "found": True,
                        "path": document["path"],
                        "metadata": json.loads(document["metadata_json"]),
                    }
                )
                search_terms.append(document["title"])

            if location:
                search_terms.append(location)
            combined_query = (scene_query or " ".join(search_terms) or " ".join(names) or characters or location or "").strip()

            if names or (scene_query or "").strip() or (location or "").strip():
                candidate_matches = get_scene_candidates(connection, names, combined_query, limit=min(limit, 5))
        finally:
            connection.close()

        try:
            if candidate_matches:
                with connect() as match_connection:
                    match_connection.row_factory = sqlite3.Row
                    match_payloads = build_scene_match_payloads(
                        match_connection,
                        base_url,
                        candidate_matches,
                        limit=min(limit, 5),
                        chunk_size=chunk_size,
                    )
                    if match_payloads:
                        best_match_context = match_payloads[0]["context"]
        except Exception:
            import traceback
            traceback.print_exc()
            candidate_matches = []
            match_payloads = []
            best_match_context = []

        if combined_query:
            try:
                search_results = search_lore(combined_query, limit=limit)
                related_context = search_results["results"]
            except Exception:
                import traceback
                traceback.print_exc()
                related_context = []
        if best_match_context:
            merged_context = []
            seen: set[tuple[str, str, str]] = set()
            for chunk in best_match_context + related_context:
                key = (chunk["path"], chunk["heading"], chunk["text"])
                if key in seen:
                    continue
                seen.add(key)
                merged_context.append(chunk)
                if len(merged_context) >= limit:
                    break
            related_context = merged_context
    except Exception as e:
        import traceback
        traceback.print_exc()
        raise HTTPException(status_code=500, detail=f"{type(e).__name__}: {e}") from e

    if not related_context and best_match_context:
        related_context = [dump_chunk(chunk) for chunk in best_match_context]

    related_context = split_chunks_for_scene_context(related_context, chunk_size)[:limit]
    pagination = paginate_scene_context_chunks(related_context, page_size=page_size, cursor=cursor)

    return cache_set(cache_key, {
        "characters": character_payloads,
        "location": location,
        "scene_matches": match_payloads,
        "related_context": pagination["related_context"],
        "cursor": pagination["cursor"],
        "next_cursor": pagination["next_cursor"],
        "has_more": pagination["has_more"],
        "page_size": pagination["page_size"],
        "total_related_context": pagination["total_related_context"],
    })


@app.get("/scene_context", response_model=SceneContextResponse)
def get_scene_context_alias(
    request: Request,
    characters: Annotated[str, Query(description="Comma-separated character names.")] = "",
    location: Annotated[str, Query(description="Optional location name.")] = "",
    scene_query: Annotated[str, Query(description="Freeform scene description to match against scene context.")] = "",
    limit: Annotated[int, Query(ge=1, le=20)] = 12,
    page_size: Annotated[int, Query(ge=1, le=50, description="Number of related context chunks to return in this page.")] = 5,
    cursor: Annotated[Optional[str], Query(description="Opaque cursor returned by a previous /scene-context response.")] = None,
    chunk_size: Annotated[int, Query(ge=300, le=8000, description="Maximum characters per scene-context text chunk.")] = 1200,
) -> dict:
    return get_scene_context(
        request=request,
        characters=characters,
        location=location,
        scene_query=scene_query,
        limit=limit,
        page_size=page_size,
        cursor=cursor,
        chunk_size=chunk_size,
    )


@app.get("/files/{file_id:path}", include_in_schema=False)
def get_file(file_id: str):
    path = file_id
    if "/" not in file_id:
        with connect() as connection:
            row = connection.execute(
                """
                SELECT path
                FROM file_links
                WHERE file_id = ?
                LIMIT 1
                """,
                (file_id,),
            ).fetchone()
            if row:
                path = row["path"]
    target = (ROOT / normalize_repo_path(path)).resolve()
    if ROOT not in target.parents and target != ROOT:
        raise HTTPException(status_code=400, detail="Path escapes repository root")
    if not target.exists() or not target.is_file():
        raise HTTPException(status_code=404, detail="File not found")
    return FileResponse(target)
