from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re

import httpx
from dotenv import load_dotenv
from fastapi import FastAPI, File, HTTPException, UploadFile, status
from fastapi.responses import FileResponse, Response
from pydantic import BaseModel


load_dotenv(Path(__file__).with_name(".env"))

PACKAGE_MAGIC = b"GSSPKG1"
MAX_PACKAGE_SIZE = 128 * 1024 * 1024
SAVE_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")
STORAGE_DIR = Path(os.getenv("GOTHICSAVE_STORAGE", "storage"))
SUPABASE_URL = os.getenv("SUPABASE_URL", "").rstrip("/")
SUPABASE_SERVICE_ROLE_KEY = os.getenv("SUPABASE_SERVICE_ROLE_KEY", "")
SUPABASE_STORAGE_BUCKET = os.getenv("SUPABASE_STORAGE_BUCKET", "gothic-saves")
USE_SUPABASE = (
    SUPABASE_URL.startswith(("https://", "http://"))
    and bool(SUPABASE_SERVICE_ROLE_KEY)
)


class SaveInfo(BaseModel):
    save_id: str
    filename: str
    size: int
    uploaded_at: datetime
    uploaded_at_epoch: int = 0


app = FastAPI(
    title="GothicSaveSync API",
    version="0.2.0",
    description="Upload, list and download GothicSaveSync .gss save packages.",
)


def validate_save_id(save_id: str) -> None:
    if SAVE_ID_PATTERN.fullmatch(save_id) is None:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="save_id must contain only letters, numbers, '-' or '_'.",
        )


def package_path(save_id: str) -> Path:
    return STORAGE_DIR / f"{save_id}.gss"


def metadata_path(save_id: str) -> Path:
    return STORAGE_DIR / f"{save_id}.json"


def read_metadata(save_id: str) -> SaveInfo:
    try:
        data = json.loads(metadata_path(save_id).read_text(encoding="utf-8"))
        return SaveInfo.model_validate(data)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        raise HTTPException(status_code=500, detail="Stored save metadata is invalid.") from error


def supabase_headers() -> dict[str, str]:
    return {
        "apikey": SUPABASE_SERVICE_ROLE_KEY,
        "Authorization": f"Bearer {SUPABASE_SERVICE_ROLE_KEY}",
    }


def database_url() -> str:
    return f"{SUPABASE_URL}/rest/v1/save_packages"


def object_url(save_id: str) -> str:
    return f"{SUPABASE_URL}/storage/v1/object/{SUPABASE_STORAGE_BUCKET}/{save_id}.gss"


async def supabase_request(
    method: str,
    url: str,
    *,
    content: bytes | None = None,
    params: dict[str, str] | None = None,
    headers: dict[str, str] | None = None,
) -> httpx.Response:
    request_headers = supabase_headers()
    if headers:
        request_headers.update(headers)
    try:
        async with httpx.AsyncClient(timeout=30.0) as client:
            response = await client.request(
                method, url, content=content, params=params, headers=request_headers
            )
    except httpx.RequestError as error:
        raise HTTPException(status_code=502, detail="Supabase is unreachable.") from error
    if response.is_error:
        raise HTTPException(status_code=502, detail="Supabase request failed.")
    return response


async def read_upload(save: UploadFile) -> bytes:
    try:
        package = bytearray()
        while chunk := await save.read(1024 * 1024):
            package.extend(chunk)
            if len(package) > MAX_PACKAGE_SIZE:
                raise HTTPException(status_code=413, detail="The save package is too large.")
    finally:
        await save.close()
    if len(package) < len(PACKAGE_MAGIC) or package[: len(PACKAGE_MAGIC)] != PACKAGE_MAGIC:
        raise HTTPException(status_code=400, detail="The file is not a GothicSaveSync package.")
    return bytes(package)


def save_info_from_row(row: dict[str, object]) -> SaveInfo:
    uploaded_at = datetime.fromisoformat(str(row["uploaded_at"]).replace("Z", "+00:00"))
    return SaveInfo(
        save_id=str(row["save_id"]),
        filename=str(row["filename"]),
        size=int(row["size"]),
        uploaded_at=uploaded_at,
        uploaded_at_epoch=int(uploaded_at.timestamp()),
    )


async def supabase_save_info(save_id: str) -> SaveInfo:
    response = await supabase_request(
        "GET",
        database_url(),
        params={"select": "*", "save_id": f"eq.{save_id}", "limit": "1"},
    )
    rows = response.json()
    if not rows:
        raise HTTPException(status_code=404, detail="Save package not found.")
    return save_info_from_row(rows[0])


@app.on_event("startup")
async def create_storage_directory() -> None:
    if not USE_SUPABASE:
        STORAGE_DIR.mkdir(parents=True, exist_ok=True)


@app.get("/health")
async def health() -> dict[str, str]:
    return {"status": "ok"}


@app.post("/saves/{save_id}", response_model=SaveInfo, status_code=status.HTTP_201_CREATED)
async def upload_save(save_id: str, save: UploadFile = File(...)) -> SaveInfo:
    validate_save_id(save_id)
    if not save.filename or not save.filename.lower().endswith(".gss"):
        raise HTTPException(status_code=400, detail="The uploaded file must use the .gss extension.")
    package = await read_upload(save)
    uploaded_at = datetime.now(timezone.utc)
    info = SaveInfo(
        save_id=save_id,
        filename=f"{save_id}.gss",
        size=len(package),
        uploaded_at=uploaded_at,
        uploaded_at_epoch=int(uploaded_at.timestamp()),
    )

    if USE_SUPABASE:
        await supabase_request(
            "POST",
            object_url(save_id),
            content=package,
            headers={"Content-Type": "application/octet-stream", "x-upsert": "true"},
        )
        try:
            await supabase_request(
                "POST",
                database_url(),
                content=json.dumps({
                    **info.model_dump(mode="json"),
                    "storage_path": f"{save_id}.gss",
                }).encode(),
                headers={"Content-Type": "application/json", "Prefer": "resolution=merge-duplicates"},
            )
        except HTTPException:
            await supabase_request("DELETE", object_url(save_id))
            raise
        return info

    STORAGE_DIR.mkdir(parents=True, exist_ok=True)
    temporary = package_path(save_id).with_suffix(".uploading")
    temporary.write_bytes(package)
    os.replace(temporary, package_path(save_id))
    metadata_path(save_id).write_text(info.model_dump_json(), encoding="utf-8")
    return info


@app.get("/saves", response_model=list[SaveInfo])
async def list_saves() -> list[SaveInfo]:
    if USE_SUPABASE:
        response = await supabase_request(
            "GET",
            database_url(),
            params={"select": "*", "order": "uploaded_at.desc"},
        )
        return [save_info_from_row(row) for row in response.json()]

    STORAGE_DIR.mkdir(parents=True, exist_ok=True)
    saves = []
    for metadata in STORAGE_DIR.glob("*.json"):
        try:
            saves.append(read_metadata(metadata.stem))
        except HTTPException:
            continue
    return sorted(saves, key=lambda save: save.uploaded_at, reverse=True)


@app.get("/saves/{save_id}", response_model=SaveInfo)
async def get_save_info(save_id: str) -> SaveInfo:
    validate_save_id(save_id)
    if USE_SUPABASE:
        return await supabase_save_info(save_id)
    if not package_path(save_id).is_file() or not metadata_path(save_id).is_file():
        raise HTTPException(status_code=404, detail="Save package not found.")
    return read_metadata(save_id)


@app.get("/saves/{save_id}/download", response_model=None)
async def download_save(save_id: str) -> Response | FileResponse:
    validate_save_id(save_id)
    if USE_SUPABASE:
        response = await supabase_request("GET", object_url(save_id))
        return Response(
            content=response.content,
            media_type="application/octet-stream",
            headers={"Content-Disposition": f'attachment; filename="{save_id}.gss"'},
        )
    destination = package_path(save_id)
    if not destination.is_file():
        raise HTTPException(status_code=404, detail="Save package not found.")
    return FileResponse(destination, media_type="application/octet-stream", filename=f"{save_id}.gss")


@app.delete("/saves/{save_id}", status_code=status.HTTP_204_NO_CONTENT)
async def delete_save(save_id: str) -> None:
    validate_save_id(save_id)
    if USE_SUPABASE:
        await supabase_request("DELETE", object_url(save_id))
        await supabase_request(
            "DELETE", database_url(), params={"save_id": f"eq.{save_id}"}
        )
        return
    package_path(save_id).unlink(missing_ok=True)
    metadata_path(save_id).unlink(missing_ok=True)


@app.get("/status")
async def service_status() -> dict[str, str]:
    if not USE_SUPABASE:
        STORAGE_DIR.mkdir(parents=True, exist_ok=True)
        return {"status": "ok", "backend": "filesystem"}
    await supabase_request(
        "GET",
        database_url(),
        params={"select": "save_id", "limit": "1"},
    )
    return {"status": "ok", "backend": "supabase"}
