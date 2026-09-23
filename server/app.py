from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re

from fastapi import FastAPI, File, HTTPException, UploadFile, status
from fastapi.responses import FileResponse
from pydantic import BaseModel


PACKAGE_MAGIC = b"GSSPKG1"
MAX_PACKAGE_SIZE = 128 * 1024 * 1024
SAVE_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")
STORAGE_DIR = Path(os.getenv("GOTHICSAVE_STORAGE", "storage"))


class SaveInfo(BaseModel):
    save_id: str
    filename: str
    size: int
    uploaded_at: datetime
    uploaded_at_epoch: int = 0


app = FastAPI(
    title="GothicSaveSync API",
    version="0.1.0",
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


@app.on_event("startup")
async def create_storage_directory() -> None:
    STORAGE_DIR.mkdir(parents=True, exist_ok=True)


@app.get("/health")
async def health() -> dict[str, str]:
    return {"status": "ok"}


@app.post("/saves/{save_id}", response_model=SaveInfo, status_code=status.HTTP_201_CREATED)
async def upload_save(save_id: str, save: UploadFile = File(...)) -> SaveInfo:
    validate_save_id(save_id)
    if not save.filename or not save.filename.lower().endswith(".gss"):
        raise HTTPException(status_code=400, detail="The uploaded file must use the .gss extension.")

    STORAGE_DIR.mkdir(parents=True, exist_ok=True)
    destination = package_path(save_id)
    temporary = destination.with_suffix(".uploading")
    total_size = 0

    try:
        with temporary.open("wb") as output:
            magic = await save.read(len(PACKAGE_MAGIC))
            if magic != PACKAGE_MAGIC:
                raise HTTPException(status_code=400, detail="The file is not a GothicSaveSync package.")
            output.write(magic)

            while chunk := await save.read(1024 * 1024):
                total_size += len(chunk)
                if total_size + len(magic) > MAX_PACKAGE_SIZE:
                    raise HTTPException(status_code=413, detail="The save package is too large.")
                output.write(chunk)
    except HTTPException:
        temporary.unlink(missing_ok=True)
        raise
    except OSError as error:
        temporary.unlink(missing_ok=True)
        raise HTTPException(status_code=500, detail="Could not store the save package.") from error
    finally:
        await save.close()

    os.replace(temporary, destination)
    uploaded_at = datetime.now(timezone.utc)
    info = SaveInfo(
        save_id=save_id,
        filename=f"{save_id}.gss",
        size=destination.stat().st_size,
        uploaded_at=uploaded_at,
        uploaded_at_epoch=int(uploaded_at.timestamp()),
    )
    metadata_path(save_id).write_text(info.model_dump_json(), encoding="utf-8")
    return info


@app.get("/saves", response_model=list[SaveInfo])
async def list_saves() -> list[SaveInfo]:
    STORAGE_DIR.mkdir(parents=True, exist_ok=True)
    saves = []
    for metadata in STORAGE_DIR.glob("*.json"):
        save_id = metadata.stem
        try:
            saves.append(read_metadata(save_id))
        except HTTPException:
            continue
    return sorted(saves, key=lambda save: save.uploaded_at, reverse=True)


@app.get("/saves/{save_id}", response_model=SaveInfo)
async def get_save_info(save_id: str) -> SaveInfo:
    validate_save_id(save_id)
    if not package_path(save_id).is_file() or not metadata_path(save_id).is_file():
        raise HTTPException(status_code=404, detail="Save package not found.")
    return read_metadata(save_id)


@app.get("/saves/{save_id}/download")
async def download_save(save_id: str) -> FileResponse:
    validate_save_id(save_id)
    destination = package_path(save_id)
    if not destination.is_file():
        raise HTTPException(status_code=404, detail="Save package not found.")
    return FileResponse(destination, media_type="application/octet-stream", filename=f"{save_id}.gss")


@app.delete("/saves/{save_id}", status_code=status.HTTP_204_NO_CONTENT)
async def delete_save(save_id: str) -> None:
    validate_save_id(save_id)
    package_path(save_id).unlink(missing_ok=True)
    metadata_path(save_id).unlink(missing_ok=True)


@app.get("/status")
async def status() -> dict[str, str]:
    if not STORAGE_DIR.is_dir():
        raise HTTPException(status_code=500, detail="Storage directory is not available.")
    return {"status": "ok"}