# BSPGuy MCP integration

The MCP server works with the live BSP inside the editor, including unsaved
changes. The editor owns the map and executes each operation on its main thread.
The companion `bspguy-mcp` executable provides tool discovery and JSON-RPC over
[MCP stdio](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports).
It finds the editor automatically under the same OS user account.

## Connect a client

1. Build BSPGuy using the [repository build instructions](../README.md#building-the-source).
   The build produces `bspguy` and `bspguy-mcp` in the same output directory
   (`build/Release` with the Windows Visual Studio generator).
2. Open a BSP in the editor and choose **Tools → AI Integration (MCP) → Enable
   connection**.
3. Register the companion executable as a **stdio** server in your MCP client
   using its absolute path. For example, in Codex on Windows:

   ```powershell
   codex mcp add bspguy -- "C:\path\to\bspguy\build\Release\bspguy-mcp.exe"
   ```

   For clients that use an `mcpServers` JSON file, the entry is:

   ```json
   {
     "mcpServers": {
       "bspguy": {
         "command": "C:\\path\\to\\bspguy\\build\\Release\\bspguy-mcp.exe"
       }
     }
   }
   ```

4. Start or reload the client and ask it to inspect the current map. To let it
   modify entities, enable **Allow map edits** in the same editor menu. Use
   **File → Save** or **Save As** when you want to write the edits to disk.

The client configuration survives editor restarts. Enable the connection for
each editor session. Only one BSPGuy process per OS user can publish an active
connection. Toggle **Enable connection** off to disconnect immediately. The
local connection uses a temporary loopback port and a per-session credential
discovered through a user-private file.

To opt in at launch, run `bspguy --ai [map.bsp]`. The explicit
`bspguy --ai-edit [map.bsp]` variant starts with map edits enabled.

## Available tools

| Tool | Operation |
| --- | --- |
| `get_editor_state` | Read the active map ID and revision, format, path, lump counts, camera, selection, and edit permissions. |
| `list_entities` | List entity summaries with optional exact classname and targetname filters. |
| `get_entity` | Read ordered key/value pairs and outgoing target names for one entity. |
| `list_geometry` | List faces, textures, BSP models, or leaves with type-specific metadata. |
| `get_face` | Read one face and its vertex coordinates. |
| `capture_viewport` | Return a PNG of the 3D viewport with selection overlays and camera metadata. |
| `select_entities` | Replace or clear the editor selection and optionally frame the first entity. |
| `create_entity` | Create a point entity from key/value pairs. |
| `update_entity` | Set and remove entity keys in a single undoable action. |
| `delete_entities` | Delete one or more entities in a single undoable action. |
| `undo`, `redo` | Use the editor's history, including manual actions. |

Start with `get_editor_state` and use `map.map_id` as the `map_id` argument for
the other tools. Use the returned `revision` with selections and edits. Pass it
on paginated reads for a consistent view across pages. Read results and
successful edits include the current revision. Indices are zero-based and can
shift after entity deletion or undo. Continue pagination with `next_offset`
until it is null.

`capture_viewport` returns an `image/png` content block. The structured result
includes image dimensions, camera, selection, map ID and revision. The
optional `max_dimension` sets the longest side from 160 through 1280 pixels
and defaults to 960. The image keeps the viewport's aspect ratio. Restore the
editor window before capturing if it is minimized.

For example, ask the client to find `info_player_start` entities and select the
first result, or create an `info_target` with a specific `origin` and
`targetname` and then verify it through `get_entity`.

Document changes are limited to entities. Existing BSP brush bindings and
worldspawn's structural keys are protected. Use the editor for opening and
saving maps or modifying geometry, textures, lighting, visibility, and collision.

## Troubleshooting

- **Editor unavailable**. Check that the companion and editor run as the same
  OS user and that the editor has the connection enabled.
- **Editor busy**. Wait for loading, or close the editor's active dialog and
  finish the current editing gesture.
- **Map or revision changed**. Read fresh state and verify the entity or face
  again before retrying.
- **Read only**. Enable **Allow map edits** in the editor for document changes.

The companion speaks MCP stdio revision `2026-07-28` and the handshake-based
revisions `2025-11-25` and `2025-06-18`.

## Verify a build

The smoke test exercises discovery, a live editor session, permission checks,
PNG capture, selection, edits, undo and redo, and the stale document guards. It requires
Python 3, a graphical session, and a valid BSP. Complete the editor's startup
dialog if it appears. Disable any other BSPGuy connection before running it.
The test edits a disposable copy and checks that its contents on disk stay intact.

```powershell
python tests/mcp_smoke.py build/Release/bspguy.exe build/Release/bspguy-mcp.exe "C:\maps\example.bsp"
```
