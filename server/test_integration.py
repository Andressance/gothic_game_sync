"""
GothicSaveSync Server Integration Tests
========================================
Validates the API endpoints and the full upload/download cycle.

Run with:
    cd server
    python -m pytest test_integration.py -v

Or standalone (starts the server automatically):
    python test_integration.py
"""

import io
import json
import struct
import os
import sys
import time

# ---------------------------------------------------------------------------
# GSSPKG1 package builder — mirrors SaveSync.cpp's PackDirectory()
# ---------------------------------------------------------------------------

PACKAGE_MAGIC = b"GSSPKG1\x00"  # 8 bytes including null terminator
PACKAGE_VERSION = 1


def build_test_package(slot_id: int = 1, gothic_hash: int = 0xDEADBEEF,
                       files: dict[str, bytes] | None = None) -> bytes:
    """
    Build a minimal .gss package in the same binary format as
    SaveSync::PackDirectory() produces.

    Header layout (all little-endian):
      - magic:      8 bytes  "GSSPKG1\\0"
      - version:    4 bytes  uint32
      - slot_id:    4 bytes  int32
      - hash:       4 bytes  uint32
      - file_count: 4 bytes  uint32
      - reserved:   4 bytes  uint32

    Per file:
      - path_len:   4 bytes  uint32
      - file_size:  8 bytes  uint64
      - path:       path_len bytes (relative path, no null terminator)
      - data:       file_size bytes
    """
    if files is None:
        files = {
            "SAVEDAT.SAV": b"fake-save-data-1234567890",
            "manifest.txt": (
                f"format=1\nslot={slot_id}\ngothic_hash={gothic_hash}\n"
            ).encode(),
        }

    buf = io.BytesIO()

    # Header
    buf.write(PACKAGE_MAGIC)
    buf.write(struct.pack("<I", PACKAGE_VERSION))
    buf.write(struct.pack("<i", slot_id))
    buf.write(struct.pack("<I", gothic_hash))
    file_count_pos = buf.tell()
    buf.write(struct.pack("<I", 0))  # placeholder
    buf.write(struct.pack("<I", 0))  # reserved

    # Files
    count = 0
    for rel_path, data in files.items():
        path_bytes = rel_path.encode("utf-8")
        buf.write(struct.pack("<I", len(path_bytes)))
        buf.write(struct.pack("<Q", len(data)))
        buf.write(path_bytes)
        buf.write(data)
        count += 1

    # Patch file count
    buf.seek(file_count_pos)
    buf.write(struct.pack("<I", count))

    return buf.getvalue()


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

import pytest


def server_base_url() -> str:
    return os.getenv("TEST_SERVER_URL", "http://127.0.0.1:8000")


@pytest.fixture(scope="session")
def base_url():
    return server_base_url()


@pytest.fixture(scope="session")
def http():
    import httpx
    with httpx.Client(timeout=15.0) as client:
        yield client


class TestHealth:
    """Basic server liveness checks."""

    def test_health_endpoint(self, http, base_url):
        r = http.get(f"{base_url}/health")
        assert r.status_code == 200
        body = r.json()
        assert body["status"] == "ok"

    def test_status_endpoint(self, http, base_url):
        r = http.get(f"{base_url}/status")
        assert r.status_code == 200
        body = r.json()
        assert body["status"] == "ok"
        assert "backend" in body


class TestUploadDownloadCycle:
    """Full upload → list → info → download → delete cycle."""

    SAVE_ID = "test_savegame1"

    def test_01_upload(self, http, base_url):
        """Upload a valid .gss package via multipart POST."""
        package = build_test_package(slot_id=1)
        files = {"save": (f"{self.SAVE_ID}.gss", io.BytesIO(package),
                          "application/octet-stream")}
        r = http.post(f"{base_url}/saves/{self.SAVE_ID}", files=files)
        assert r.status_code == 201, f"Upload failed: {r.text}"
        body = r.json()
        assert body["save_id"] == self.SAVE_ID
        assert body["size"] == len(package)
        assert body["uploaded_at_epoch"] > 0
        print(f"  ✓ Uploaded {body['size']} bytes, epoch={body['uploaded_at_epoch']}")

    def test_02_list_saves(self, http, base_url):
        """GET /saves must include the uploaded save."""
        r = http.get(f"{base_url}/saves")
        assert r.status_code == 200
        saves = r.json()
        assert isinstance(saves, list)
        ids = [s["save_id"] for s in saves]
        assert self.SAVE_ID in ids, f"Save not in list: {ids}"
        # Verify JSON fields match what the C++ mod parses
        save = next(s for s in saves if s["save_id"] == self.SAVE_ID)
        assert "uploaded_at_epoch" in save
        assert "filename" in save
        print(f"  ✓ Found {len(saves)} save(s), ours is present")

    def test_03_get_info(self, http, base_url):
        """GET /saves/{id} returns metadata."""
        r = http.get(f"{base_url}/saves/{self.SAVE_ID}")
        assert r.status_code == 200
        body = r.json()
        assert body["save_id"] == self.SAVE_ID
        print(f"  ✓ Info: size={body['size']}, epoch={body['uploaded_at_epoch']}")

    def test_04_download(self, http, base_url):
        """GET /saves/{id}/download returns the binary .gss with correct magic."""
        r = http.get(f"{base_url}/saves/{self.SAVE_ID}/download")
        assert r.status_code == 200
        data = r.content
        assert data[:7] == b"GSSPKG1", f"Bad magic: {data[:8]!r}"
        print(f"  ✓ Downloaded {len(data)} bytes, magic OK")

    def test_05_reupload_overwrites(self, http, base_url):
        """Uploading the same save_id again should overwrite (upsert)."""
        package = build_test_package(slot_id=1, files={
            "SAVEDAT.SAV": b"updated-save-data",
            "manifest.txt": b"format=1\nslot=1\ngothic_hash=3735928559\n",
        })
        files = {"save": (f"{self.SAVE_ID}.gss", io.BytesIO(package),
                          "application/octet-stream")}
        r = http.post(f"{base_url}/saves/{self.SAVE_ID}", files=files)
        assert r.status_code == 201
        assert r.json()["size"] == len(package)
        print(f"  ✓ Re-upload OK, new size={len(package)}")

    def test_06_delete(self, http, base_url):
        """DELETE /saves/{id} removes the save."""
        r = http.delete(f"{base_url}/saves/{self.SAVE_ID}")
        assert r.status_code == 204
        # Verify gone
        r = http.get(f"{base_url}/saves/{self.SAVE_ID}")
        assert r.status_code == 404
        print("  ✓ Deleted and confirmed 404")


class TestValidation:
    """Server-side input validation."""

    def test_invalid_save_id(self, http, base_url):
        """IDs with special characters should be rejected."""
        r = http.post(f"{base_url}/saves/../../etc/passwd",
                      files={"save": ("bad.gss", io.BytesIO(b"x"), "application/octet-stream")})
        # FastAPI's router may return 404 (path not matched) or 400/422 (validation)
        assert r.status_code in (400, 404, 422), f"Expected 400/404/422, got {r.status_code}"

    def test_wrong_extension(self, http, base_url):
        """Files without .gss extension should be rejected."""
        package = build_test_package()
        files = {"save": ("savegame.zip", io.BytesIO(package),
                          "application/octet-stream")}
        r = http.post(f"{base_url}/saves/test_ext", files=files)
        assert r.status_code == 400, f"Expected 400, got {r.status_code}"

    def test_bad_magic(self, http, base_url):
        """Files without GSSPKG1 header should be rejected."""
        files = {"save": ("bad.gss", io.BytesIO(b"NOT_A_PACKAGE_AT_ALL"),
                          "application/octet-stream")}
        r = http.post(f"{base_url}/saves/test_magic", files=files)
        assert r.status_code == 400

    def test_nonexistent_download(self, http, base_url):
        """Downloading a save that doesn't exist should 404."""
        r = http.get(f"{base_url}/saves/does_not_exist_xyz/download")
        assert r.status_code == 404


class TestModJsonParsing:
    """
    Verify the JSON from /saves can be parsed by the same logic
    the C++ mod uses (string-search for "save_id":"..." and
    "uploaded_at_epoch":...).
    """

    SAVE_ID = "test_json_parse"

    def test_json_field_offsets(self, http, base_url):
        """Upload, list, and simulate the C++ JSON parser."""
        # Upload
        package = build_test_package()
        files = {"save": (f"{self.SAVE_ID}.gss", io.BytesIO(package),
                          "application/octet-stream")}
        http.post(f"{base_url}/saves/{self.SAVE_ID}", files=files)

        # Get list
        r = http.get(f"{base_url}/saves")
        raw = r.text

        # Simulate C++ parser (RemoteSync.cpp lines 170-185)
        saves_found = []
        cursor = 0
        while True:
            marker = '"save_id":"'
            idx = raw.find(marker, cursor)
            if idx < 0:
                break
            idx += len(marker)
            end = raw.find('"', idx)
            save_id = raw[idx:end]

            epoch_marker = '"uploaded_at_epoch":'
            epoch_idx = raw.find(epoch_marker, end)
            if epoch_idx < 0:
                break
            value_start = epoch_idx + len(epoch_marker)
            # The C++ parser does _atoi64(timestamp + 20), which is the same
            # as reading from right after the colon
            value_end = value_start
            while value_end < len(raw) and (raw[value_end].isdigit() or raw[value_end] in ' '):
                value_end += 1
            epoch_str = raw[value_start:value_end].strip()
            epoch = int(epoch_str) if epoch_str else 0

            saves_found.append({"save_id": save_id, "uploaded_at_epoch": epoch})
            cursor = end + 1

        print(f"  C++ parser simulation found {len(saves_found)} save(s)")
        our_save = [s for s in saves_found if s["save_id"] == self.SAVE_ID]
        assert len(our_save) == 1, f"Not found: {saves_found}"
        assert our_save[0]["uploaded_at_epoch"] > 1700000000  # sanity: after 2023

        # Cleanup
        http.delete(f"{base_url}/saves/{self.SAVE_ID}")
        print(f"  ✓ JSON parsing simulation OK: epoch={our_save[0]['uploaded_at_epoch']}")


# ---------------------------------------------------------------------------
# Standalone runner — starts the server if needed
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import subprocess

    url = server_base_url()
    server_proc = None

    # Try to reach the server
    try:
        import httpx
        httpx.get(f"{url}/health", timeout=3.0)
        print(f"Server already running at {url}")
    except Exception:
        print(f"Starting server at {url}...")
        server_proc = subprocess.Popen(
            [sys.executable, "-m", "uvicorn", "app:app",
             "--host", "127.0.0.1", "--port", "8000"],
            cwd=os.path.dirname(__file__),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        # Wait for startup
        for attempt in range(20):
            time.sleep(0.5)
            try:
                import httpx
                httpx.get(f"{url}/health", timeout=2.0)
                print(f"Server started (attempt {attempt + 1})")
                break
            except Exception:
                pass
        else:
            print("ERROR: Server did not start in time")
            if server_proc:
                server_proc.kill()
            sys.exit(1)

    try:
        exit_code = pytest.main([__file__, "-v", "--tb=short"])
    finally:
        if server_proc:
            server_proc.kill()
            server_proc.wait()
            print("Server stopped")

    sys.exit(exit_code)
