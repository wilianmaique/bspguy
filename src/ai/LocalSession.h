#pragma once

#include <functional>
#include <memory>
#include <string>

namespace bspai {

// One authenticated JSON line per loopback connection. The editor pumps the
// listener on its main thread, so the handler can use the live document.
class LocalSession {
public:
	LocalSession();
	~LocalSession();
	bool start(std::string& error);
	void stop();
	bool active() const;
	const std::string& id() const;
	void poll(const std::function<std::string(const std::string&)>& handle);

private:
	struct State;
	std::unique_ptr<State> state;
};

bool callEditor(const std::string& request, std::string& response, std::string& error);

constexpr size_t maxRequestBytes = 256 * 1024;
// A 1280 x 1280 RGB PNG can require ~6.6 MB when base64 encoded.
constexpr size_t maxResponseBytes = 8 * 1024 * 1024;

}
