"""Exercise the stdio server and a live BSPGuy session on a disposable map copy."""

import argparse
import base64
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
from queue import Queue
import shutil
import subprocess
import struct
import tempfile
from threading import Thread
import time


class Client:
    def __init__(self, process):
        self.process = process
        self.responses = Queue()
        self.request_id = 0

        def read_responses():
            for line in process.stdout:
                self.responses.put(line)
            self.responses.put(None)

        Thread(target=read_responses, daemon=True).start()

    def send(self, request):
        self.process.stdin.write(json.dumps({"jsonrpc": "2.0", **request}) + "\n")
        self.process.stdin.flush()

    def request(self, method, params=None):
        self.request_id += 1
        self.send({"id": self.request_id, "method": method, "params": params or {}})
        line = self.responses.get(timeout=10)
        assert line is not None, f"MCP server exited during {method}"
        response = json.loads(line)
        assert response["id"] == self.request_id, response
        return response

    def tool(self, name, error=None, **arguments):
        response = self.request("tools/call", {"name": name, "arguments": arguments})
        result = response["result"]
        data = result["structuredContent"]
        assert json.loads(result["content"][0]["text"]) == data
        assert result["isError"] == bool(error), result
        if error:
            assert data["error"]["code"] == error, data
        return data


@contextmanager
def running(command, **kwargs):
    with subprocess.Popen(command, **kwargs) as process:
        try:
            yield process
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


def wait_ready(client):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        response = client.request("tools/call", {"name": "get_editor_state"})["result"]
        state = response["structuredContent"]
        if not response["isError"] and state["map"] and not state["busy"]:
            return state
        time.sleep(0.2)
    raise AssertionError(f"Editor did not become ready: {state!r}")


def connect(client):
    assert client.request("tools/list")["error"]["code"] == -32002
    result = client.request("initialize", {
        "protocolVersion": "2025-11-25",
        "capabilities": {},
        "clientInfo": {"name": "bspguy-smoke", "version": "1.0"},
    })["result"]
    assert result["protocolVersion"] == "2025-11-25"
    client.send({"method": "notifications/initialized"})
    assert client.request("ping")["result"] == {}
    tools = client.request("tools/list")["result"]["tools"]
    assert len({tool["name"] for tool in tools}) == 12
    assert client.request("tools/call", {"name": "made_up"})["error"]["code"] == -32602
    client.tool("get_editor_state", error="invalid_arguments", extraneous=True)


def test_modern(client):
    meta = {"io.modelcontextprotocol/protocolVersion": "2026-07-28"}
    result = client.request("server/discover", {"_meta": meta})["result"]
    assert "2026-07-28" in result["supportedVersions"]
    assert result["_meta"]["io.modelcontextprotocol/serverInfo"]["name"] == "bspguy"
    assert result["resultType"] == "complete" and result["cacheScope"] == "public" and result["ttlMs"] > 0
    result = client.request("tools/list", {"_meta": meta})["result"]
    assert any(tool["name"] == "get_editor_state" for tool in result["tools"])
    assert result["resultType"] == "complete" and result["cacheScope"] == "public" and result["ttlMs"] > 0
    params = {"_meta": meta, "name": "get_editor_state", "arguments": {"unexpected": True}}
    result = client.request("tools/call", params)["result"]
    assert result["resultType"] == "complete" and result["isError"]
    assert client.request("tools/list")["error"]["code"] == -32022


def test_document(client, state):
    document = state["map"]
    context = {key: document[key] for key in ("map_id", "revision")}
    entities = client.tool("list_entities", limit=1, **context)
    assert entities["entities"][0]["index"] == 0
    assert client.tool("get_entity", index=0, **context)["entity"]["classname"] == "worldspawn"
    assert client.tool("list_geometry", kind="models", limit=1, **context)["records"][0]["index"] == 0
    for kind in ("faces", "textures", "leaves"):
        page = client.tool("list_geometry", kind=kind, limit=1, **context)
        assert page["total"] == document["counts"][kind]
        if page["total"] > 1:
            assert page["next_offset"] == 1
    if document["counts"]["faces"]:
        face = client.tool("get_face", index=0, limit=1, **context)
        assert len(face["vertices"][0]) == 3
    return context


def test_capture(client, context):
    request = {"name": "capture_viewport", "arguments": {**context, "max_dimension": 160}}
    result = client.request("tools/call", request)["result"]
    assert not result["isError"], result
    metadata = result["structuredContent"]
    image = next(block for block in result["content"] if block["type"] == "image")
    assert image["mimeType"] == "image/png"
    png = base64.b64decode(image["data"], validate=True)
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    width, height = struct.unpack(">II", png[16:24])
    assert (width, height) == (metadata["width"], metadata["height"])
    assert 0 < min(width, height) <= max(width, height) <= 160
    assert "png_base64" not in metadata and metadata["map_id"] == context["map_id"]
    assert metadata["revision"] == context["revision"]
    assert len(metadata["camera"]["origin"]) == 3


def test_edits(client, context):
    def edit(name, **arguments):
        result = client.tool(name, **context, **arguments)
        context["revision"] = result["revision"]
        return result

    client.tool("delete_entities", error="invalid_index", indices=[0], **context)
    created = edit("create_entity", keyvalues={"classname": "info_target", "origin": "0 0 0"})
    index = created["index"]
    client.tool("update_entity", error="invalid_keyvalues", index=index,
                set={"targetname": "partial", "zzz": 'invalid"quote'}, **context)
    entity = client.tool("get_entity", index=index, **context)
    assert not entity["entity"]["targetname"]
    edit("select_entities", indices=[index], focus=True)
    assert client.tool("get_editor_state")["selection"]["entities"]["indices"] == [index]
    stale = context.copy()
    edit("update_entity", index=index, set={"targetname": "mcp_smoke"})
    client.tool("update_entity", error="revision_changed", index=index,
                set={"targetname": "stale"}, **stale)
    assert client.tool("get_entity", index=index, **context)["entity"]["targetname"] == "mcp_smoke"
    edit("undo")
    assert not client.tool("get_entity", index=index, **context)["entity"]["targetname"]
    edit("redo")
    assert client.tool("get_entity", index=index, **context)["entity"]["targetname"] == "mcp_smoke"
    edit("update_entity", index=index, remove=["targetname"])
    assert not client.tool("get_entity", index=index, **context)["entity"]["targetname"]
    edit("delete_entities", indices=[index])
    client.tool("get_entity", error="invalid_index", index=index, **context)
    edit("undo")
    assert client.tool("get_entity", index=index, **context)["entity"]["classname"] == "info_target"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("editor", type=Path)
    parser.add_argument("mcp", type=Path)
    parser.add_argument("map", type=Path)
    args = parser.parse_args()
    editor, mcp, map_path = (path.resolve(strict=True) for path in (args.editor, args.mcp, args.map))
    pipes = {"stdin": subprocess.PIPE, "stdout": subprocess.PIPE, "text": True, "encoding": "utf-8"}
    with running([mcp], **pipes) as process:
        test_modern(Client(process))
    with tempfile.TemporaryDirectory() as folder, running([mcp], **pipes) as process:
        client = Client(process)
        connect(client)
        client.tool("get_editor_state", error="editor_unavailable")
        test_map = Path(folder) / map_path.name
        shutil.copyfile(map_path, test_map)
        original_hash = hashlib.sha256(test_map.read_bytes()).digest()

        with running([editor, "--ai", test_map], cwd=editor.parent):
            state = wait_ready(client)
            assert not state["allow_map_edits"]
            previous = test_document(client, state)
            test_capture(client, previous)
            client.tool("create_entity", error="read_only", keyvalues={"classname": "info_target"}, **previous)

        client.tool("get_editor_state", error="editor_unavailable")
        with running([editor, "--ai-edit", test_map], cwd=editor.parent):
            state = wait_ready(client)
            assert state["allow_map_edits"]
            client.tool("list_entities", error="map_changed", **previous)
            test_edits(client, test_document(client, state))
        client.tool("get_editor_state", error="editor_unavailable")
        assert hashlib.sha256(test_map.read_bytes()).digest() == original_hash
    print("MCP discovery, live reads, PNG capture, selection, edits, undo/redo, stale guards and disconnect passed.")


if __name__ == "__main__":
    main()
