#include "Tools.h"
#include <algorithm>
#include <climits>
#include <stdexcept>

namespace bspai {
namespace {

Json object(Json properties, Json required = Json::array()) {
	return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}

Json integer(const char* description, int maximum = INT_MAX, int minimum = 0) {
	return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}, {"description", description}};
}

Json string(const char* description, int maxLength, int minLength = 1) {
	return {{"type", "string"}, {"maxLength", maxLength}, {"minLength", minLength}, {"description", description}};
}

Json array(Json items, int maximum, const char* description) {
	return {{"type", "array"}, {"items", items}, {"maxItems", maximum}, {"description", description}};
}

Json documentSchema(Json extra, Json required, bool revisionRequired = false) {
	extra["map_id"] = string("Map ID returned by get_editor_state.", 64);
	extra["revision"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991LL},
		{"description", "Map revision returned by the latest read or successful edit."}};
	required.push_back("map_id");
	if (revisionRequired)
		required.push_back("revision");
	return object(extra, required);
}

const std::vector<Tool>& catalog() {
	static const auto index = integer("Zero-based BSP index.");
	static const auto offset = integer("Zero-based starting index. Defaults to 0.");
	static const auto limit = integer("Maximum records to return. Defaults to 50.", 100, 1);
	static const auto keys = Json{{"type", "object"}, {"maxProperties", 256},
		{"additionalProperties", string("BSP entity key value.", 4095, 0)}};
	static const auto ids = array(index, 100, "Distinct zero-based entity indices.");
	static const std::vector<Tool> tools = {
		{ToolId::State, "get_editor_state",
			"Get the active document ID and revision, BSP format, counts, camera, selection, and edit permissions. Call first.",
			Effect::Read, false, object(Json::object())},
		{ToolId::ListEntities, "list_entities",
			"List entity summaries. Filters are exact and case-sensitive. Use next_offset for the next page.",
			Effect::Read, false, documentSchema({{"offset", offset}, {"limit", limit},
				{"classname", string("Filter by classname.", 256)}, {"targetname", string("Filter by targetname.", 256)}}, Json::array())},
		{ToolId::Entity, "get_entity",
			"Read an entity's ordered key/value pairs and outgoing target names. Paginate key/value pairs with next_offset.",
			Effect::Read, false, documentSchema({{"index", index}, {"offset", offset}, {"limit", limit}}, {"index"})},
		{ToolId::Geometry, "list_geometry",
			"List faces, models, textures, or leaves from the loaded BSP. Use next_offset for the next page.",
			Effect::Read, false, documentSchema({{"kind", {{"type", "string"}, {"enum", {"faces", "models", "textures", "leaves"}}}},
				{"offset", offset}, {"limit", limit}}, {"kind"})},
		{ToolId::Face, "get_face",
			"Read one BSP face, its texture and a page of its vertices in native BSP coordinates.",
			Effect::Read, false, documentSchema({{"index", index}, {"offset", offset}, {"limit", limit}}, {"index"})},
		{ToolId::CaptureViewport, "capture_viewport",
			"Capture a PNG of the current 3D viewport and selection overlays, with camera and map revision metadata.",
			Effect::Read, false, documentSchema({{"max_dimension",
				integer("Longest image side in pixels. Defaults to 960 and never enlarges the viewport.", maxCaptureSize, 160)}},
				Json::array())},
		{ToolId::SelectEntities, "select_entities",
			"Replace the visible editor selection. Use an empty list to clear it and focus=true to frame the first entity.",
			Effect::View, false, documentSchema({{"indices", ids}, {"focus", {{"type", "boolean"}}}}, {"indices"}, true)},
		{ToolId::UpdateEntity, "update_entity",
			"Apply key/value additions, replacements, and removals to one entity in one undo step. Requires map edits enabled.",
			Effect::Document, true, documentSchema({{"index", index}, {"set", keys},
				{"remove", array(string("BSP entity key to remove.", 255), 256, "Keys to remove.")}}, {"index"}, true)},
		{ToolId::CreateEntity, "create_entity",
			"Create a point entity from key/value pairs including classname. Returns the new index. Requires map edits enabled.",
			Effect::Document, false, documentSchema({{"keyvalues", keys}}, {"keyvalues"}, true)},
		{ToolId::DeleteEntities, "delete_entities",
			"Delete entities in one undo step. The world entity is protected. Requires map edits enabled.",
			Effect::Document, true, documentSchema({{"indices", ids}}, {"indices"}, true)},
		{ToolId::Undo, "undo",
			"Undo the latest editor action, including actions taken manually. Requires map edits enabled.",
			Effect::Document, true, documentSchema(Json::object(), Json::array(), true)},
		{ToolId::Redo, "redo",
			"Redo the latest undone editor action. Requires map edits enabled.",
			Effect::Document, true, documentSchema(Json::object(), Json::array(), true)}
	};
	return tools;
}

// Validate the small JSON Schema subset used by this catalog in both processes.
std::string validate(const Json& schema, const Json& value, const std::string& path) {
	const auto type = schema.value("type", "");
	bool matches = (type == "object" && value.is_object()) || (type == "array" && value.is_array()) ||
		(type == "string" && value.is_string()) || (type == "integer" && value.is_number_integer()) ||
		(type == "boolean" && value.is_boolean());
	if (!matches)
		return path + " must be of type " + type + ".";
	if (schema.contains("enum") && std::find(schema["enum"].begin(), schema["enum"].end(), value) == schema["enum"].end())
		return path + " has an unsupported value.";
	if (value.is_number_integer()) {
		// Check the range before any narrowing conversion, including unsigned JSON integers.
		double number = value.get<double>();
		if (number < schema["minimum"].get<double>() || number > schema["maximum"].get<double>())
			return path + " is outside its allowed range.";
	}
	if (value.is_string() && schema.contains("maxLength")) {
		const auto& str = value.get_ref<const std::string&>();
		size_t characters = std::count_if(str.begin(), str.end(), [](unsigned char byte) {
			return (byte & 0xc0) != 0x80;
		});
		if (characters < schema["minLength"].get<size_t>() || characters > schema["maxLength"].get<size_t>())
			return path + " has an invalid length.";
	}
	if (value.is_array()) {
		if (value.size() > schema["maxItems"].get<size_t>())
			return path + " has too many items.";
		for (size_t i = 0; i < value.size(); ++i) {
			auto error = validate(schema["items"], value[i], path + "[" + std::to_string(i) + "]");
			if (!error.empty())
				return error;
		}
	}
	if (value.is_object()) {
		if (schema.contains("maxProperties") && value.size() > schema["maxProperties"].get<size_t>())
			return path + " contains too many properties.";
		if (schema.contains("required")) {
			for (const auto& key : schema["required"])
				if (!value.contains(key.get<std::string>()))
					return path + "." + key.get<std::string>() + " is required.";
		}
		const auto empty = Json::object();
		const auto& fields = schema.contains("properties") ? schema["properties"] : empty;
		const auto& additional = schema["additionalProperties"];
		for (auto it = value.begin(); it != value.end(); ++it) {
			if (!fields.contains(it.key()) && !additional.is_object())
				return path + "." + it.key() + " is not a supported argument.";
			auto error = validate(fields.contains(it.key()) ? fields[it.key()] : additional,
				it.value(), path + "." + it.key());
			if (!error.empty())
				return error;
		}
	}
	return {};
}

}

const Tool* findTool(const std::string& name) {
	for (const auto& tool : catalog())
		if (tool.name == name)
			return &tool;
	return nullptr;
}

Json describeTools() {
	Json tools = Json::array();
	for (const auto& tool : catalog()) {
		tools.push_back({{"name", tool.name}, {"description", tool.description}, {"inputSchema", tool.schema},
			{"annotations", {{"readOnlyHint", tool.effect == Effect::Read}, {"destructiveHint", tool.destructive},
				{"idempotentHint", tool.effect != Effect::Document}, {"openWorldHint", false}}}});
	}
	return tools;
}

std::string validateArguments(const Tool& tool, const Json& arguments) {
	return validate(tool.schema, arguments, "arguments");
}

Json parseJson(const std::string& line) {
	return Json::parse(line, [](int depth, Json::parse_event_t, Json&) {
		if (depth > 32)
			throw std::runtime_error("JSON nesting limit exceeded");
		return true;
	});
}

std::string encodeJson(const Json& data) {
	return data.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Json failure(const char* code, const std::string& message) {
	return {{"error", {{"code", code}, {"message", message}}}};
}

}
