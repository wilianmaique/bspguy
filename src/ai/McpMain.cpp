#include "LocalSession.h"
#include "Tools.h"
#include <iostream>

namespace {

using bspai::Json;
const char modernVersion[] = "2026-07-28";
const char legacyVersion[] = "2025-11-25";
const char structuredVersion[] = "2025-06-18";
const char versionKey[] = "io.modelcontextprotocol/protocolVersion";
const char serverInfoKey[] = "io.modelcontextprotocol/serverInfo";
const Json serverInfo = {{"name", "bspguy"}, {"version", "1.0.0"}};
const Json capabilities = {{"tools", Json::object()}};
constexpr int catalogTtlMs = 60000;
const char instructions[] =
	"Use get_editor_state first. Tools operate on the BSP currently open in BSPGuy. "
	"Use the returned map_id and revision to avoid editing a stale document. "
	"Entity edits are undoable and must be saved in the editor.";

Json rpcError(const Json& id, int code, const char* message, Json data = nullptr) {
	Json error = {{"code", code}, {"message", message}};
	if (!data.is_null())
		error["data"] = std::move(data);
	return {{"jsonrpc", "2.0"}, {"id", id}, {"error", error}};
}

Json rpcResult(const Json& id, Json result, bool modern = false) {
	if (modern) {
		result["resultType"] = "complete";
		result["_meta"][serverInfoKey] = serverInfo;
	}
	return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json toolResult(Json result, bool includeViewport = false) {
	bool failed = result.contains("error");
	Json content = Json::array();
	if (includeViewport && !failed) {
		auto png = result.find("png_base64");
		if (png == result.end() || !png->is_string() || png->get_ref<const std::string&>().empty())
			return toolResult(bspai::failure("invalid_editor_response", "The editor did not return a viewport image."));
		content.push_back({{"type", "image"}, {"mimeType", "image/png"}, {"data", std::move(*png)}});
		result.erase(png);
	}
	content.push_back({{"type", "text"}, {"text", bspai::encodeJson(result)}});
	return {{"isError", failed}, {"structuredContent", std::move(result)}, {"content", std::move(content)}};
}

Json callTool(const bspai::Tool& tool, const Json& arguments) {
	auto invalid = bspai::validateArguments(tool, arguments);
	if (!invalid.empty())
		return toolResult(bspai::failure("invalid_arguments", invalid));
	std::string request = bspai::encodeJson({{"name", tool.name}, {"arguments", arguments}});
	if (request.size() > bspai::maxRequestBytes)
		return toolResult(bspai::failure("request_too_large", "Reduce the size of the tool arguments."));
	std::string response, error;
	if (!bspai::callEditor(request, response, error))
		return toolResult(bspai::failure("editor_unavailable", error));
	try {
		auto result = bspai::parseJson(response);
		if (result.is_object())
			return toolResult(std::move(result), tool.id == bspai::ToolId::CaptureViewport);
	}
	catch (const std::exception&) {
	}
	return toolResult(bspai::failure("invalid_editor_response", "The editor returned an invalid response."));
}

class McpServer {
public:
	Json handle(const Json& request) {
		const Json empty = Json::object();
		if (!request.is_object() || request.value("jsonrpc", Json()) != "2.0" ||
			!request.value("method", Json()).is_string())
			return rpcError(nullptr, -32600, "Invalid Request");
		const auto method = request["method"].get<std::string>();
		if (!request.contains("id")) {
			if (method == "notifications/initialized" && era == Era::Initializing)
				era = Era::Legacy;
			return nullptr;
		}
		const auto& id = request["id"];
		if (!id.is_number_integer() && !id.is_string())
			return rpcError(nullptr, -32600, "Invalid request ID");
		const auto& params = request.contains("params") ? request["params"] : empty;
		if (!params.is_object())
			return rpcError(id, -32602, "Expected an object for params");
		const auto& meta = params.contains("_meta") ? params["_meta"] : empty;
		if (!meta.is_object())
			return rpcError(id, -32602, "Expected an object for _meta");
		auto requested = meta.value(versionKey, Json());
		bool modern = era == Era::Modern || (era == Era::Unselected &&
			(method == "server/discover" || !requested.is_null()));
		if (modern) {
			if (!requested.is_null() && !requested.is_string())
				return rpcError(id, -32602, "Expected a string protocol version");
			const auto supported = Json::array({modernVersion});
			if (requested != modernVersion)
				return rpcError(id, -32022, "Unsupported protocol version",
					{{"supported", supported}, {"requested", requested}});
			era = Era::Modern;
			if (method == "server/discover")
				return rpcResult(id, {{"supportedVersions", supported}, {"capabilities", capabilities},
					{"instructions", instructions}, {"ttlMs", catalogTtlMs}, {"cacheScope", "public"}}, true);
		}
		else {
			if (method == "initialize" && era == Era::Unselected) {
				auto version = params.value("protocolVersion", Json());
				if (!version.is_string() || !params.value("capabilities", Json()).is_object() ||
					!params.value("clientInfo", Json()).is_object())
					return rpcError(id, -32602, "Expected protocolVersion, capabilities and clientInfo");
				era = Era::Initializing;
				return rpcResult(id, {{"protocolVersion", version == structuredVersion ? structuredVersion : legacyVersion},
					{"serverInfo", serverInfo}, {"capabilities", capabilities}, {"instructions", instructions}});
			}
			if (method == "ping")
				return rpcResult(id, empty);
			if (era != Era::Legacy)
				return rpcError(id, -32002, "Server not initialized");
		}
		if (method == "tools/list") {
			if (params.contains("cursor"))
				return rpcError(id, -32602, "This tool list has a single page");
			Json result = {{"tools", bspai::describeTools()}};
			if (modern) {
				result["ttlMs"] = catalogTtlMs;
				result["cacheScope"] = "public";
			}
			return rpcResult(id, std::move(result), modern);
		}
		if (method == "tools/call") {
			auto name = params.value("name", Json());
			if (!name.is_string())
				return rpcError(id, -32602, "Tool name is required");
			const auto* tool = bspai::findTool(name.get<std::string>());
			if (!tool)
				return rpcError(id, -32602, "Unknown tool");
			return rpcResult(id, callTool(*tool, params.value("arguments", empty)), modern);
		}
		return rpcError(id, -32601, "Method not found");
	}

private:
	enum class Era { Unselected, Initializing, Legacy, Modern };
	Era era = Era::Unselected;
};

bool readLine(std::string& line, bool& tooLarge) {
	line.clear();
	tooLarge = false;
	char next;
	while (std::cin.get(next)) {
		if (next == '\n')
			return true;
		if (line.size() < bspai::maxRequestBytes)
			line += next;
		else
			tooLarge = true;
	}
	return false;
}

}

int main() {
	std::ios::sync_with_stdio(false);
	std::cin.tie(nullptr);
	McpServer server;
	std::string line;
	bool tooLarge;
	while (readLine(line, tooLarge)) {
		Json response;
		if (tooLarge) {
			response = rpcError(nullptr, -32600, "Request exceeds the size limit");
		}
		else {
			try {
				auto request = bspai::parseJson(line);
				response = server.handle(request);
			}
			catch (const std::exception&) {
				response = rpcError(nullptr, -32700, "Invalid JSON or nesting limit exceeded");
			}
		}
		if (!response.is_null())
			std::cout << bspai::encodeJson(response) << '\n' << std::flush;
	}
	return 0;
}
