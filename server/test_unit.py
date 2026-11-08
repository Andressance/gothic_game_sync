"""
Unit tests for server/app.py.

Runs in filesystem mode against a temporary storage directory. The Supabase
rollback test swaps the shared HTTP client for a mock transport.

Run with:
    python -m pytest server/test_unit.py -q
"""

import os
import tempfile

# Must be set before importing app: storage and Supabase settings are read at import time.
# Empty strings (not unset) stop load_dotenv() from filling them from server/.env.
os.environ["GOTHICSAVE_STORAGE"] = tempfile.mkdtemp(prefix="gothicsave_unit_")
os.environ["SUPABASE_URL"] = ""
os.environ["SUPABASE_SERVICE_ROLE_KEY"] = ""
os.environ["GOTHICSAVE_API_TOKEN"] = ""

import httpx
import pytest
from fastapi.testclient import TestClient

import app as app_module

PACKAGE = app_module.PACKAGE_MAGIC + b"fake-save-payload"


@pytest.fixture
def client():
    with TestClient(app_module.app) as c:
        yield c


def test_token_required_when_configured(client, monkeypatch):
    monkeypatch.setenv("GOTHICSAVE_API_TOKEN", "secret-token")
    upload = {"save": ("auth_test.gss", PACKAGE)}

    assert client.post("/saves/auth_test", files=upload).status_code == 401
    assert client.post("/saves/auth_test", files=upload,
                       headers={"X-API-Key": "wrong"}).status_code == 401
    assert client.get("/health").status_code == 200
    assert client.post("/saves/auth_test", files=upload,
                       headers={"X-API-Key": "secret-token"}).status_code == 201


def test_base_epoch_conflict(client):
    upload = {"save": ("conflict_test.gss", PACKAGE)}
    first = client.post("/saves/conflict_test", files=upload)
    assert first.status_code == 201
    epoch = first.json()["uploaded_at_epoch"]

    stale = client.post("/saves/conflict_test", files=upload,
                        headers={"X-Base-Epoch": str(epoch - 60)})
    assert stale.status_code == 409
    assert stale.json()["detail"] == "Remote save is newer."

    invalid = client.post("/saves/conflict_test", files=upload,
                          headers={"X-Base-Epoch": "not-a-number"})
    assert invalid.status_code == 400

    assert client.post("/saves/conflict_test", files=upload).status_code == 201


def test_supabase_db_failure_returns_502_without_delete(client, monkeypatch):
    seen: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append(request)
        if request.method == "POST" and request.url.path.startswith("/rest/v1/"):
            return httpx.Response(500, text="database down")
        return httpx.Response(200, json={})

    monkeypatch.setattr(app_module, "USE_SUPABASE", True)
    monkeypatch.setattr(app_module, "SUPABASE_URL", "https://fake-project.supabase.test")
    monkeypatch.setattr(app_module, "SUPABASE_SERVICE_ROLE_KEY", "fake-service-key")
    monkeypatch.setattr(app_module, "client",
                        httpx.AsyncClient(transport=httpx.MockTransport(handler)))

    response = client.post("/saves/rollback_test", files={"save": ("rollback_test.gss", PACKAGE)})

    assert response.status_code == 502
    assert [request.method for request in seen] == ["POST", "POST"]
    assert not any(request.method == "DELETE" for request in seen)
