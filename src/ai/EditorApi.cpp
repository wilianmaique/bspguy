#include "Tools.h"
#include "Editor.h"
#include "Command.h"
#include "Entity.h"
#include "Gui.h"
#include "globals.h"
#include "lodepng.h"
#include "resample.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>

namespace {

using bspai::Json;
using bspai::failure;

Json xyz(const vec3& value) { return Json::array({value.x, value.y, value.z}); }

Json bounds(const vec3& mins, const vec3& maxs) {
	return {{"min", xyz(mins)}, {"max", xyz(maxs)}};
}

template<class T>
Json selection(const std::vector<T>& values) {
	return {{"count", values.size()}, {"indices", std::vector<T>(values.begin(),
		values.begin() + std::min<size_t>(128, values.size()))}};
}

Json entitySummary(Entity& entity, int index) {
	return {{"index", index}, {"classname", entity.getClassname().substr(0, MAX_KEY_LEN)},
		{"targetname", entity.getTargetname().substr(0, MAX_KEY_LEN)}, {"origin", xyz(entity.getOrigin())},
		{"bsp_model_index", entity.getBspModelIdx()}, {"hidden_in_editor", entity.hidden}};
}

Json page(const Json& args, size_t count, const char* key, Json values, size_t next) {
	return {{key, std::move(values)}, {"offset", args.value("offset", 0)}, {"total", count},
		{"next_offset", next < count ? Json(next) : Json()}};
}

Json textureMetadata(Bsp& map, int index) {
	BSPMIPTEX* texture = map.get_texture(index);
	if (!texture)
		return {{"index", index}, {"available", false}};
	auto end = std::find(texture->szName, texture->szName + sizeof(texture->szName), '\0');
	return {{"index", index}, {"available", true}, {"name", std::string(texture->szName, end)},
		{"width", texture->nWidth}, {"height", texture->nHeight}, {"embedded", texture->nOffsets[0] != 0}};
}

Json faceMetadata(Bsp& map, int index) {
	const auto& face = map.faces[index];
	Json texture;
	if (face.iTextureInfo < static_cast<unsigned>(map.texinfoCount)) {
		auto mip = map.texinfos[face.iTextureInfo].iMiptex;
		if (mip < static_cast<unsigned>(map.textureCount))
			texture = textureMetadata(map, mip);
	}
	return {{"index", index}, {"plane_index", face.iPlane}, {"plane_back_side", face.nPlaneSide != 0},
		{"vertex_count", face.nEdges}, {"texture", texture},
		{"lightmap_offset", face.nLightmapOffset == uint32_t(-1) ? Json() : Json(face.nLightmapOffset)}};
}

Json geometryRecord(Bsp& map, const std::string& kind, int index) {
	if (kind == "textures")
		return textureMetadata(map, index);
	if (kind == "faces")
		return faceMetadata(map, index);
	if (kind == "models") {
		const auto& model = map.models[index];
		return {{"index", index}, {"bounds", bounds(model.nMins, model.nMaxs)}, {"origin", xyz(model.vOrigin)},
			{"first_face", model.iFirstFace}, {"face_count", model.nFaces}, {"hull_headnodes", model.iHeadnodes}};
	}
	const auto& leaf = map.leaves[index];
	return {{"index", index}, {"bounds", bounds(leaf.nMins, leaf.nMaxs)}, {"contents", leaf.nContents},
		{"contents_name", map.getLeafContentsName(leaf.nContents)}, {"marksurface_count", leaf.nMarkSurfaces}};
}

bool validText(const std::string& value, size_t maxBytes) {
	return value.size() <= maxBytes && std::none_of(value.begin(), value.end(), [](unsigned char c) {
		return c < 32 || c == 127 || c == '"';
	});
}

bool validCoordinates(const std::string& value) {
	std::istringstream input(value);
	input.imbue(std::locale::classic());
	double x, y, z;
	if (!(input >> x >> y >> z))
		return false;
	input >> std::ws;
	return input.eof() && std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
		std::abs(x) <= MAX_MAP_COORD && std::abs(y) <= MAX_MAP_COORD && std::abs(z) <= MAX_MAP_COORD;
}

bool validAssetPath(std::string value) {
	std::replace(value.begin(), value.end(), '\\', '/');
	return value.empty() || (value.front() != '/' && value.find(':') == std::string::npos &&
		("/" + value + "/").find("/../") == std::string::npos);
}

std::string editRestriction(const std::string& key, int index, Entity& original) {
	if (index == 0 && (key == "classname" || key == "origin" || key == "model" || key == "wad"))
		return "The world entity's classname, origin, model, and WAD list are managed by the editor.";
	if (key == "model" && original.getBspModelIdx() >= 0)
		return "Changing an entity's BSP brush binding requires geometry editing in the editor.";
	return {};
}

std::string applyKeyvalues(Entity& proposed, Entity& original, int index, const Json& set, const Json& remove) {
	for (const auto& item : remove) {
		auto key = item.get<std::string>();
		auto restricted = editRestriction(key, index, original);
		if (!validText(key, MAX_KEY_LEN - 1) || key == "classname" || set.contains(key))
			return "A removal contains an invalid key, classname, or a key also present in set.";
		if (!restricted.empty())
			return restricted;
		proposed.removeKeyvalue(key);
	}
	for (auto it = set.begin(); it != set.end(); ++it) {
		const auto& key = it.key();
		const auto& value = it->get_ref<const std::string&>();
		if (key.empty() || !validText(key, MAX_KEY_LEN - 1) || !validText(value, MAX_VAL_LEN - 1))
			return "Keys and values must fit the BSP editor limits and exclude control characters and quotes.";
		if (original.hasKey(key) && original.getKeyvalue(key) == value)
			continue;
		auto restricted = editRestriction(key, index, original);
		if (!restricted.empty())
			return restricted;
		if (key == "classname" && (value.empty() || value == "worldspawn"))
			return "A point or brush entity requires a non-empty classname other than worldspawn.";
		if (key == "origin" && !validCoordinates(value))
			return "origin must contain three finite space-separated coordinates within the BSP coordinate limit.";
		if (key == "model" && (!validAssetPath(value) || (!value.empty() && value.front() == '*')))
			return "model must be an asset-relative path. BSP brush binding is managed in the editor.";
		proposed.setOrAddKeyvalue(key, value);
	}
	if (proposed.keyOrder.size() > std::max<size_t>(MAX_KEYS_PER_ENT, original.keyOrder.size()))
		return "The edit would exceed the entity's key limit.";
	if (proposed.getClassname().empty())
		return "An entity requires a classname.";
	return {};
}

}

class EditorApi {
public:
	EditorApi(Editor& app, bool viewportReady)
		: app(app), map(*app.mapRenderer->map), viewportReady(viewportReady) {}

	std::string handle(const std::string& line) {
		try {
			auto request = bspai::parseJson(line);
			if (!request.is_object() || !request.value("name", Json()).is_string() ||
				!request.contains("arguments"))
				return bspai::encodeJson(failure("invalid_request", "Expected a tool name and arguments."));
			auto tool = bspai::findTool(request["name"].get<std::string>());
			if (!tool)
				return bspai::encodeJson(failure("unknown_tool", "Unknown editor tool."));
			const auto& arguments = request["arguments"];
			auto invalid = bspai::validateArguments(*tool, arguments);
			if (!invalid.empty())
				return bspai::encodeJson(failure("invalid_arguments", invalid));
			auto response = bspai::encodeJson(run(*tool, arguments));
			if (response.size() > bspai::maxResponseBytes)
				return bspai::encodeJson(failure("response_too_large", "Request a smaller page."));
			return response;
		}
		catch (const nlohmann::json::parse_error&) {
			return bspai::encodeJson(failure("invalid_request", "Invalid JSON request."));
		}
		catch (const std::exception& error) {
			logf("AI tool error: %s\n", error.what());
			return bspai::encodeJson(failure("editor_error", "The editor could not complete the request."));
		}
	}

private:
	Editor& app;
	Bsp& map;
	bool viewportReady;

	bool hasMap() const { return !app.emptyMapLoaded && !app.mapArrangeMode && map.valid; }

	bool busy() const {
		return app.isLoading || app.reloading || !app.openMapAfterLoad.empty() ||
			app.movingEnt || app.draggingAxis >= 0 ||
			ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
	}

	std::string mapId() const {
		return app.aiSession.id() + "-" + std::to_string(app.aiMapGeneration);
	}

	Json identity() const { return {{"map_id", mapId()}, {"revision", app.aiRevision}}; }

	Json camera() const { return {{"origin", xyz(app.cameraOrigin)}, {"angles", xyz(app.cameraAngles)}}; }

	Json viewportSelection() const {
		const char* mode = app.pickMode == PICK_FACE ? "face" : app.pickMode == PICK_LEAF ?
			"leaf" : app.pickMode == PICK_CLIPNODE ? "collision" : "object";
		return {{"mode", mode}, {"entities", selection(app.pickInfo.ents)},
			{"faces", selection(app.pickInfo.faces)}, {"leaves", selection(app.pickInfo.leaves)},
			{"collision_volumes", selection(app.pickInfo.clipnodes)}, {"collision_hull", app.clipnodeRenderHull}};
	}

	Json state() const {
		Json document;
		if (hasMap()) {
			document = identity();
			document["name"] = map.name;
			document["path"] = map.path;
			document["format"] = map.lastLoadformat >= 0 && map.lastLoadformat < BSP_FORMAT_TYPES ?
				g_bsp_format_names[map.lastLoadformat] : "BSP";
			document["counts"] = {{"entities", map.ents.size()}, {"faces", map.faceCount},
				{"models", map.modelCount}, {"textures", map.textureCount}, {"leaves", map.leafCount},
				{"vertices", map.vertCount}, {"clipnodes", map.clipnodeCount}};
			if (map.modelCount > 0)
				document["world_model_bounds"] = bounds(map.models[0].nMins, map.models[0].nMaxs);
		}
		return {{"map", document}, {"busy", busy()}, {"allow_map_edits", app.aiEditsEnabled},
			{"can_undo", !app.undoHistory.empty()}, {"can_redo", !app.redoHistory.empty()},
			{"ripent_safe_mode", g_settings.ripent_safe_mode}, {"preview_mode", app.previewMode},
			{"camera", camera()}, {"selection", viewportSelection()}};
	}

	Json run(const bspai::Tool& tool, const Json& args) {
		using bspai::ToolId;
		if (tool.id == ToolId::State)
			return state();
		if (!hasMap())
			return failure("no_active_map", "Open a BSP map in the editor first.");
		if (args["map_id"] != mapId())
			return failure("map_changed", "The active map changed. Call get_editor_state and inspect it again.");
		if (busy())
			return failure("editor_busy", "Wait for the editor to finish loading or completing the current operation.");
		if (args.contains("revision") && args["revision"] != app.aiRevision)
			return failure("revision_changed", "The map was edited. Inspect the entity or geometry again before retrying.");
		if (tool.effect == bspai::Effect::Document && !app.aiEditsEnabled)
			return failure("read_only", "Enable Allow map edits in Tools > AI Integration (MCP) in the editor.");
		if (tool.effect != bspai::Effect::Read && app.previewMode)
			return failure("preview_mode", "Exit preview mode before changing selection or editing the map.");

		Json result;
		switch (tool.id) {
		case ToolId::ListEntities: result = listEntities(args); break;
		case ToolId::Entity: result = getEntity(args); break;
		case ToolId::Geometry: result = listGeometry(args); break;
		case ToolId::Face: result = getFace(args); break;
		case ToolId::CaptureViewport: result = captureViewport(args); break;
		case ToolId::SelectEntities: result = selectEntities(args); break;
		case ToolId::UpdateEntity: result = editEntity(args, false); break;
		case ToolId::CreateEntity: result = editEntity(args, true); break;
		case ToolId::DeleteEntities: result = deleteEntities(args); break;
		case ToolId::Undo: result = history(false); break;
		case ToolId::Redo: result = history(true); break;
		case ToolId::State: return state();
		}
		if (!result.contains("error")) {
			result.update(identity());
			if (tool.effect == bspai::Effect::Document && result.value("changed", false))
				logf("AI edited %s (%s).\n", map.name.c_str(), tool.name);
		}
		return result;
	}

	Json captureViewport(const Json& args) {
		if (!viewportReady)
			return failure("viewport_unavailable", "Restore the editor window to capture its viewport.");

		GLint viewport[4];
		glGetIntegerv(GL_VIEWPORT, viewport);
		const int width = viewport[2];
		const int height = viewport[3];
		const size_t maxFramebufferPixels = 64 * 1024 * 1024;
		if (width <= 0 || height <= 0 || size_t(width) * height > maxFramebufferPixels)
			return failure("viewport_unavailable", "The viewport dimensions are outside the capture limits.");

		std::vector<unsigned char> pixels(size_t(width) * height * 3);
		GLint readBuffer, packAlignment;
		glGetIntegerv(GL_READ_BUFFER, &readBuffer);
		glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
		glReadBuffer(GL_BACK);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glCheckError("MCP viewport capture setup");
		glReadPixels(viewport[0], viewport[1], width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
		GLenum readError = glGetError();
		glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
		glReadBuffer(readBuffer);
		if (readError != GL_NO_ERROR)
			return failure("viewport_read_failed", "OpenGL could not read the rendered viewport.");

		int imageWidth = width;
		int imageHeight = height;
		int maxDimension = args.value("max_dimension", bspai::defaultCaptureSize);
		const auto longestSide = std::max(width, height);
		if (longestSide > maxDimension) {
			imageWidth = std::max(1, int(int64_t(width) * maxDimension / longestSide));
			imageHeight = std::max(1, int(int64_t(height) * maxDimension / longestSide));
			std::vector<unsigned char> scaled(size_t(imageWidth) * imageHeight * 3);
			if (!resample24(pixels.data(), width, height, scaled.data(), imageWidth, imageHeight, KernelTypeBilinear))
				return failure("image_resize_failed", "The viewport image could not be resized.");
			pixels.swap(scaled);
		}

		// OpenGL stores the bottom row first. PNG stores the top row first.
		const size_t stride = size_t(imageWidth) * 3;
		for (int row = 0; row < imageHeight / 2; ++row) {
			auto top = pixels.begin() + row * stride;
			auto bottom = pixels.begin() + (imageHeight - 1 - row) * stride;
			std::swap_ranges(top, top + stride, bottom);
		}

		std::vector<unsigned char> png;
		if (lodepng::encode(png, pixels, imageWidth, imageHeight, LCT_RGB))
			return failure("image_encode_failed", "The viewport image could not be encoded as a PNG.");
		return {{"width", imageWidth}, {"height", imageHeight}, {"camera", camera()},
			{"selection", viewportSelection()}, {"png_base64", base64encode(png.data(), png.size())}};
	}

	Json listEntities(const Json& args) {
		const auto classname = args.value("classname", "");
		const auto targetname = args.value("targetname", "");
		size_t offset = args.value("offset", 0);
		size_t limit = args.value("limit", 50);
		size_t count = 0;
		Json entities = Json::array();
		for (size_t i = 0; i < map.ents.size(); ++i) {
			auto& entity = *map.ents[i];
			if ((!classname.empty() && entity.getClassname() != classname) ||
				(!targetname.empty() && entity.getTargetname() != targetname))
				continue;
			if (count++ >= offset && entities.size() < limit)
				entities.push_back(entitySummary(entity, i));
		}
		size_t next = offset + entities.size();
		return page(args, count, "entities", std::move(entities), next);
	}

	Json getEntity(const Json& args) {
		size_t index = args["index"].get<size_t>();
		if (index >= map.ents.size())
			return failure("invalid_index", "Entity index is out of range.");
		auto& entity = *map.ents[index];
		size_t offset = args.value("offset", 0);
		size_t next = offset;
		auto count = entity.keyOrder.size();
		auto end = std::min(count, offset + args.value("limit", 50));
		Json keys = Json::array();
		for (; next < end; ++next) {
			const auto& key = entity.keyOrder[next];
			keys.push_back({{"key", key}, {"value", entity.getKeyvalue(key)}});
		}
		auto result = page(args, count, "keyvalues", std::move(keys), next);
		result["entity"] = entitySummary(entity, index);
		const auto& targets = entity.getTargets();
		std::vector<std::string> sortedTargets;
		StringSet::iterator_t target;
		while (targets.iterate(target))
			sortedTargets.emplace_back(target.key);
		std::sort(sortedTargets.begin(), sortedTargets.end());
		result["targets"] = sortedTargets;
		return result;
	}

	Json listGeometry(const Json& args) {
		const auto kind = args["kind"].get<std::string>();
		int count = kind == "textures" ? map.textureCount : kind == "faces" ? map.faceCount :
			kind == "models" ? map.modelCount : map.leafCount;
		size_t next = args.value("offset", 0);
		auto end = std::min<size_t>(count, next + args.value("limit", 50));
		Json records = Json::array();
		for (; next < end; ++next)
			records.push_back(geometryRecord(map, kind, next));
		auto result = page(args, count, "records", std::move(records), next);
		result["kind"] = kind;
		return result;
	}

	Json getFace(const Json& args) {
		int index = args["index"].get<int>();
		if (index >= map.faceCount)
			return failure("invalid_index", "Face index is out of range.");
		const auto& face = map.faces[index];
		if (uint64_t(face.iFirstEdge) + face.nEdges > uint64_t(map.surfedgeCount))
			return failure("invalid_face", "The face references surfedges outside the BSP.");
		size_t next = args.value("offset", 0);
		auto end = std::min<size_t>(face.nEdges, next + args.value("limit", 50));
		Json vertices = Json::array();
		for (; next < end; ++next) {
			int64_t edge = map.surfedges[face.iFirstEdge + next];
			size_t edgeIndex = edge >= 0 ? edge : -edge;
			if (edgeIndex >= size_t(map.edgeCount))
				return failure("invalid_face", "The face references an edge outside the BSP.");
			auto vertex = map.edges[edgeIndex].iVertex[edge >= 0 ? 1 : 0];
			if (vertex >= unsigned(map.vertCount))
				return failure("invalid_face", "The face references a vertex outside the BSP.");
			vertices.push_back(xyz(map.verts[vertex]));
		}
		auto result = page(args, face.nEdges, "vertices", std::move(vertices), next);
		result["face"] = faceMetadata(map, index);
		return result;
	}

	std::string validateIndices(const std::vector<int>& indices, bool deleting) const {
		std::unordered_set<int> seen;
		for (int index : indices) {
			if (index >= int(map.ents.size()) || !seen.insert(index).second)
				return "Entity indices must be distinct and in range.";
			if (deleting && (index == 0 || map.ents[index]->getClassname() == "worldspawn"))
				return "The world entity cannot be deleted.";
		}
		return {};
	}

	Json selectEntities(const Json& args) {
		auto indices = args["indices"].get<std::vector<int>>();
		auto invalid = validateIndices(indices, false);
		if (!invalid.empty())
			return failure("invalid_index", invalid);
		for (int index : indices) {
			const auto model = map.ents[index]->getBspModelIdx();
			if (map.ents[index]->hidden || model >= map.modelCount)
				return failure("entity_not_selectable", "An entity is hidden or references an invalid model.");
		}
		app.mapRenderer->highlightPickedFaces(false);
		app.deselectObject();
		app.hiddenLeaves.clear();
		app.pickMode = PICK_OBJECT;
		app.transformTarget = TRANSFORM_OBJECT;
		app.gui->showWidget(WIDGET_FACE_EDITOR, false);
		for (int index : indices)
			app.pickInfo.selectEnt(index);
		app.postSelectEnt();
		app.gui->entityReportReselectNeeded = true;
		if (args.value("focus", false) && !indices.empty())
			app.goToEnt(&map, indices.front());
		return {{"selected", indices}};
	}

	Json editEntity(const Json& args, bool create) {
		size_t index = create ? map.ents.size() : args["index"].get<size_t>();
		if (!create && index >= map.ents.size())
			return failure("invalid_index", "Entity index is out of range.");
		if (create && map.ents.empty())
			return failure("invalid_map", "Create a world entity in the editor before adding point entities.");
		Entity empty;
		auto& original = create ? empty : *map.ents[index];
		Entity proposed(original);
		auto set = create ? args["keyvalues"] : args.value("set", Json::object());
		auto remove = args.value("remove", Json::array());
		auto invalid = applyKeyvalues(proposed, original, index, set, remove);
		if (!invalid.empty())
			return failure("invalid_keyvalues", invalid);
		if (!create && proposed.getFullKvString() == original.getFullKvString())
			return {{"changed", false}, {"index", index}};
		std::unique_ptr<Command> command;
		if (create)
			command.reset(new CreateEntitiesCommand("AI: Create Entity", {&proposed}));
		else
			command.reset(new EditEntitiesCommand("AI: Edit Entity", index, original, proposed));
		command->execute();
		app.pushUndoCommand(command.release());
		return {{"changed", true}, {"index", index}};
	}

	Json deleteEntities(const Json& args) {
		auto indices = args["indices"].get<std::vector<int>>();
		auto invalid = validateIndices(indices, true);
		if (!invalid.empty())
			return failure("invalid_index", invalid);
		if (indices.empty())
			return {{"changed", false}, {"deleted_count", 0}};
		std::unique_ptr<Command> command(new DeleteEntitiesCommand("AI: Delete Entities", indices));
		app.mapRenderer->highlightPickedFaces(false);
		command->execute();
		app.pushUndoCommand(command.release());
		return {{"changed", true}, {"deleted_count", indices.size()}};
	}

	Json history(bool redo) {
		const auto& commands = redo ? app.redoHistory : app.undoHistory;
		if (commands.empty())
			return failure("empty_history", redo ? "There is no action to redo." : "There is no action to undo.");
		auto description = commands.back()->desc;
		if (redo)
			app.redo();
		else
			app.undo();
		return {{"changed", true}, {"action", description}};
	}
};

void Editor::pollAi(bool viewportReady) {
	aiSession.poll([this, viewportReady](const std::string& line) {
		return EditorApi(*this, viewportReady).handle(line);
	});
}

bool Editor::enableAi(bool allowEdits) {
	std::string error;
	if (!aiSession.start(error)) {
		logf("%s\n", error.c_str());
		return false;
	}
	aiEditsEnabled = allowEdits;
	logf(allowEdits ? "AI connection enabled with map edits.\n" : "AI connection enabled in read-only mode.\n");
	return true;
}
