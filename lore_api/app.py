from __future__ import annotations

import json
import re
import sqlite3
from pathlib import Path
from typing import Annotated, Any, Optional
from urllib.parse import quote

from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.openapi.utils import get_openapi
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field

from .indexer import DB_PATH, ROOT, rebuild_index, slugify, normalize_text


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


class SceneContextResponse(BaseModel):
    characters: list[SceneCharacter]
    location: Optional[str] = None
    related_context: list[ChunkResult]


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


def connect() -> sqlite3.Connection:
    if not DB_PATH.exists():
        rebuild_index()
    connection = sqlite3.connect(DB_PATH)
    connection.row_factory = sqlite3.Row
    return connection


def row_to_dict(row: sqlite3.Row) -> dict:
    return {key: row[key] for key in row.keys()}


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


def with_file_urls(references: list[dict], base_url: str) -> list[dict]:
    payload = []
    for reference in references:
        item = dict(reference)
        item["url"] = f"{base_url}/files/{quote(item['path'])}"
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
    type: Annotated[Optional[str], Query(description="Optional document type filter, e.g. character, location, scene.")] = None,
) -> dict:
    cache_key = ("search", q, limit, type)
    cached = cache_get(cache_key)
    if cached is not None:
        return cached
    fts_query = make_fts_query(q)
    connection = connect()
    try:
        params: list[object] = [fts_query]
        type_clause = ""
        if type:
            type_clause = "AND c.entity_type = ?"
            params.append(type)
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


@app.get("/factions/{name}", response_model=SearchResponse)
def get_faction(name: str) -> dict:
    cache_key = ("faction", name)
    cached = cache_get(cache_key)
    if cached is not None:
        return cached
    query = f"{name} фракция государство дом орден"
    return cache_set(cache_key, search_lore(query, limit=10, type="lore"))


@app.get("/scene-context", response_model=SceneContextResponse)
def get_scene_context(
    request: Request,
    characters: Annotated[str, Query(description="Comma-separated character names.")],
    location: Annotated[Optional[str], Query(description="Optional location name.")] = None,
    limit: Annotated[int, Query(ge=1, le=20)] = 12,
) -> dict:
    base_url = request_base_url(request)
    cache_key = ("scene-context", base_url, characters, location, limit)
    cached = cache_get(cache_key)
    if cached is not None:
        return cached
    names = [part.strip() for part in characters.split(",") if part.strip()]
    connection = connect()
    try:
        character_payloads = []
        search_terms = []
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
                    "references": with_file_urls(
                        get_references_for_document(connection, int(document["id"])),
                        base_url,
                    ),
                    "context": get_chunks_for_document(connection, int(document["id"]), limit=3),
                }
            )
            search_terms.append(document["title"])

        if location:
            search_terms.append(location)
        combined_query = " ".join(search_terms) or characters
    finally:
        connection.close()

    try:
        search_results = search_lore(combined_query, limit=limit)
        related_context = search_results["results"]
    except HTTPException:
        related_context = []
    return cache_set(cache_key, {
        "characters": character_payloads,
        "location": location,
        "related_context": related_context,
    })


@app.get("/files/{path:path}", include_in_schema=False)
def get_file(path: str):
    target = (ROOT / path).resolve()
    if ROOT not in target.parents and target != ROOT:
        raise HTTPException(status_code=400, detail="Path escapes repository root")
    if not target.exists() or not target.is_file():
        raise HTTPException(status_code=404, detail="File not found")
    return FileResponse(target)
