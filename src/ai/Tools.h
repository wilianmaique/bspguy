#pragma once

#include "nlohmann/json.hpp"

namespace bspai {

using Json = nlohmann::json;

enum class ToolId {
	State, ListEntities, Entity, Geometry, Face, CaptureViewport, SelectEntities, UpdateEntity,
	CreateEntity, DeleteEntities, Undo, Redo
};

constexpr int defaultCaptureSize = 960;
constexpr int maxCaptureSize = 1280;

enum class Effect { Read, View, Document };

struct Tool {
	ToolId id;
	const char* name;
	const char* description;
	Effect effect;
	bool destructive;
	Json schema;
};

const Tool* findTool(const std::string& name);
Json describeTools();
std::string validateArguments(const Tool& tool, const Json& arguments);
Json parseJson(const std::string& line);
std::string encodeJson(const Json& data);
Json failure(const char* code, const std::string& message);

}
