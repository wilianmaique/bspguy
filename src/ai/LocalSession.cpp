#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <wincrypt.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "LocalSession.h"
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace bspai {
namespace {

using Clock = std::chrono::steady_clock;
const auto timeout = std::chrono::seconds(5);
const char wirePrefix[] = "BSPGUY-1 ";

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalidSocket = INVALID_SOCKET;
constexpr int sendFlags = 0;
void closeSocket(Socket fd) { closesocket(fd); }
bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool nonblocking(Socket fd) {
	u_long flag = 1;
	return SetHandleInformation(reinterpret_cast<HANDLE>(fd), HANDLE_FLAG_INHERIT, 0) &&
		ioctlsocket(fd, FIONBIO, &flag) == 0;
}
struct Network {
	WSADATA data;
	bool ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
	~Network() { if (ready) WSACleanup(); }
};

std::wstring sessionPath() {
	wchar_t folder[MAX_PATH];
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, folder)))
		return {};
	return std::wstring(folder) + L"\\bspguy-ai-session-v1";
}

// Restrict the file containing the session credential to the current user.
PSECURITY_DESCRIPTOR userSecurityDescriptor() {
	HANDLE token;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return nullptr;
	DWORD size = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	std::vector<unsigned char> buffer(size);
	bool ok = size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
	CloseHandle(token);
	LPWSTR sid = nullptr;
	if (!ok || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
		return nullptr;
	std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
	LocalFree(sid);
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr);
	return descriptor;
}

struct Publication {
	HANDLE file = INVALID_HANDLE_VALUE;
	~Publication() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
	bool write(const std::string& value) {
		auto path = sessionPath();
		auto descriptor = userSecurityDescriptor();
		if (!descriptor)
			return false;
		SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
		if (!path.empty())
			file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
				&attributes, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
		LocalFree(descriptor);
		DWORD written = 0;
		return file != INVALID_HANDLE_VALUE && WriteFile(file, value.data(), static_cast<DWORD>(value.size()),
			&written, nullptr) && written == value.size();
	}
};

std::string readPublication() {
	auto path = sessionPath();
	if (path.empty())
		return {};
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return {};
	char data[128];
	DWORD size = 0;
	bool ok = ReadFile(file, data, sizeof(data), &size, nullptr);
	CloseHandle(file);
	return ok && size < sizeof(data) ? std::string(data, size) : std::string();
}

bool randomBytes(unsigned char* data, size_t size) {
	HCRYPTPROV provider;
	if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
		return false;
	bool ok = CryptGenRandom(provider, static_cast<DWORD>(size), data) != 0;
	CryptReleaseContext(provider, 0);
	return ok;
}
#else
using Socket = int;
constexpr Socket invalidSocket = -1;
constexpr int sendFlags = MSG_NOSIGNAL;
void closeSocket(Socket fd) { close(fd); }
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool nonblocking(Socket fd) {
	return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd, F_SETFL, O_NONBLOCK) == 0;
}
struct Network { bool ready = true; };

std::string sessionPath() {
	const char* runtime = getenv("XDG_RUNTIME_DIR");
	std::string folder = runtime && runtime[0] == '/' ? runtime :
		"/tmp/bspguy-" + std::to_string(getuid());
	if ((!runtime || runtime[0] != '/') && mkdir(folder.c_str(), 0700) != 0 && errno != EEXIST)
		return {};
	struct stat info;
	if (lstat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
		info.st_uid != getuid() || (info.st_mode & 0077))
		return {};
	return folder + "/bspguy-ai-session-v1";
}

bool privateFile(int fd) {
	struct stat info;
	return fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
		info.st_uid == getuid() && !(info.st_mode & 0077) && info.st_nlink == 1;
}

struct Publication {
	int file = -1;
	~Publication() {
		if (file != -1) {
			ftruncate(file, 0);
			close(file);
		}
	}
	bool write(const std::string& value) {
		auto path = sessionPath();
		if (path.empty())
			return false;
		int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
		if (fd == -1)
			return false;
		if (!privateFile(fd) || flock(fd, LOCK_EX | LOCK_NB) != 0) {
			close(fd);
			return false;
		}
		file = fd;
		return ftruncate(file, 0) == 0 && ::write(file, value.data(), value.size()) == ssize_t(value.size());
	}
};

std::string readPublication() {
	auto path = sessionPath();
	if (path.empty())
		return {};
	int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd == -1)
		return {};
	char data[128];
	// A free lock means the editor exited without clearing the file.
	ssize_t size = privateFile(fd) && flock(fd, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK ?
		read(fd, data, sizeof(data)) : -1;
	close(fd);
	return size > 0 && size < ssize_t(sizeof(data)) ? std::string(data, size) : std::string();
}

bool randomBytes(unsigned char* data, size_t size) {
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		return false;
	auto count = read(fd, data, size);
	close(fd);
	return count == ssize_t(size);
}
#endif

struct Connection {
	Socket fd;
	Clock::time_point deadline = Clock::now() + timeout;
	std::string input, output;
	size_t sent = 0;
	bool authenticated = false;
	explicit Connection(Socket fd) : fd(fd) {}
	~Connection() { if (fd != invalidSocket) closeSocket(fd); }
	Connection(const Connection&) = delete;
	Connection& operator=(const Connection&) = delete;
};

sockaddr_in loopback(unsigned short port) {
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	return addr;
}

std::string hex(const unsigned char* data, size_t size) {
	static const char digits[] = "0123456789abcdef";
	std::string out;
	for (size_t i = 0; i < size; ++i) {
		out += digits[data[i] >> 4];
		out += digits[data[i] & 15];
	}
	return out;
}

bool sameCredential(const std::string& received, const std::string& expected) {
	if (received.size() != expected.size())
		return false;
	unsigned char differences = 0;
	for (size_t i = 0; i < received.size(); ++i)
		differences |= received[i] ^ expected[i];
	return differences == 0;
}

bool waitFor(Connection& connection, bool writing) {
	auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
		connection.deadline - Clock::now()).count();
	if (remaining <= 0)
		return false;
#ifdef _WIN32
	fd_set set, errors;
	FD_ZERO(&set);
	FD_ZERO(&errors);
	FD_SET(connection.fd, &set);
	FD_SET(connection.fd, &errors);
	timeval wait = {long(remaining / 1000), long(remaining % 1000 * 1000)};
	return select(0, writing ? nullptr : &set, writing ? &set : nullptr, &errors, &wait) > 0;
#else
	pollfd fd = {connection.fd, short(writing ? POLLOUT : POLLIN), 0};
	return ::poll(&fd, 1, int(remaining)) > 0;
#endif
}

}

struct LocalSession::State {
	Network network;
	Publication publication;
	Connection listener{invalidSocket};
	std::array<std::unique_ptr<Connection>, 8> clients;
	std::string credential, id;
};

LocalSession::LocalSession() = default;
LocalSession::~LocalSession() = default;
bool LocalSession::active() const { return bool(state); }
const std::string& LocalSession::id() const { return state->id; }
void LocalSession::stop() { state.reset(); }

bool LocalSession::start(std::string& error) {
	if (state)
		return true;
	std::unique_ptr<State> next(new State());
	unsigned char secret[48];
	if (!next->network.ready || !randomBytes(secret, sizeof(secret))) {
		error = "Could not initialize the local AI connection.";
		return false;
	}
	auto& fd = next->listener.fd;
	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	auto addr = loopback(0);
	int addrSize = sizeof(addr);
#ifndef _WIN32
	socklen_t addrSizeNative = addrSize;
#else
	int& addrSizeNative = addrSize;
#endif
	if (fd == invalidSocket || !nonblocking(fd) ||
		bind(fd, reinterpret_cast<sockaddr*>(&addr), addrSize) != 0 || listen(fd, 8) != 0 ||
		getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addrSizeNative) != 0) {
		error = "Could not listen on the local loopback interface.";
		return false;
	}
	next->credential = wirePrefix + hex(secret, 32);
	next->id = hex(secret + 32, 16);
	if (!next->publication.write(std::to_string(ntohs(addr.sin_port)) + "\n" + next->credential + "\n")) {
		error = "Could not publish the AI session. If another BSPGuy window has AI enabled, disconnect it first.";
		return false;
	}
	state = std::move(next);
	return true;
}

void LocalSession::poll(const std::function<std::string(const std::string&)>& handle) {
	if (!state)
		return;
	bool handled = false;
	for (auto& client : state->clients) {
		if (!client) {
			Socket fd = accept(state->listener.fd, nullptr, nullptr);
			if (fd == invalidSocket)
				continue;
			client.reset(new Connection(fd));
			if (!nonblocking(fd)) {
				client.reset();
				continue;
			}
		}
		if (Clock::now() >= client->deadline) {
			client.reset();
			continue;
		}
		if (client->output.empty()) {
			char bytes[8192];
			int received = recv(client->fd, bytes, sizeof(bytes), 0);
			if (received > 0)
				client->input.append(bytes, received);
			if (received == 0 || (received < 0 && !wouldBlock()) || client->input.size() > maxRequestBytes + 1) {
				client.reset();
				continue;
			}
			auto end = client->input.find('\n');
			if (!client->authenticated) {
				if (end == std::string::npos && client->input.size() <= state->credential.size())
					continue;
				if (end == std::string::npos || !sameCredential(client->input.substr(0, end), state->credential)) {
					client.reset();
					continue;
				}
				client->authenticated = true;
				client->input.erase(0, end + 1);
				end = client->input.find('\n');
			}
			if (handled || end == std::string::npos)
				continue;
			// Skip requests abandoned while the GUI was busy and reject extra data.
			char next;
			int pending = recv(client->fd, &next, 1, MSG_PEEK);
			if (end + 1 != client->input.size() || pending >= 0 || !wouldBlock()) {
				client.reset();
				continue;
			}
			client->output = handle(client->input.substr(0, end)) + "\n";
			client->input.clear();
			client->deadline = Clock::now() + timeout;
			handled = true;
		}
		const auto left = client->output.size() - client->sent;
		int sent = send(client->fd, client->output.data() + client->sent, int(left), sendFlags);
		if (sent > 0)
			client->sent += sent;
		if (client->sent == client->output.size() || sent == 0 || (sent < 0 && !wouldBlock()))
			client.reset();
	}
}

bool callEditor(const std::string& request, std::string& response, std::string& error) {
	error = "Open BSPGuy and enable Tools > AI Integration (MCP) > Enable connection.";
	std::istringstream publication(readPublication());
	unsigned port = 0;
	std::string credential;
	if (!(publication >> port) || port == 0 || port > 65535)
		return false;
	publication.ignore(1, '\n');
	std::getline(publication, credential);
	if (credential.compare(0, sizeof(wirePrefix) - 1, wirePrefix) != 0 ||
		credential.size() != sizeof(wirePrefix) - 1 + 64 ||
		credential.find_first_not_of("0123456789abcdef", sizeof(wirePrefix) - 1) != std::string::npos)
		return false;

	Network network;
	if (!network.ready)
		return false;
	Connection connection(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
	if (connection.fd == invalidSocket || !nonblocking(connection.fd))
		return false;
	auto addr = loopback(static_cast<unsigned short>(port));
	if (connect(connection.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
		if (!waitFor(connection, true))
			return false;
		int code = 0;
#ifdef _WIN32
		int size = sizeof(code);
#else
		socklen_t size = sizeof(code);
#endif
		if (getsockopt(connection.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &size) != 0 || code)
			return false;
	}

	error = "The editor did not return a response. Check for open dialogs and inspect the editor state before retrying an edit.";
	std::string message = credential + "\n" + request + "\n";
	while (connection.sent < message.size() && waitFor(connection, true)) {
		int count = send(connection.fd, message.data() + connection.sent,
			int(message.size() - connection.sent), sendFlags);
		if (count > 0)
			connection.sent += count;
		else if (count == 0 || !wouldBlock())
			return false;
	}
	if (connection.sent != message.size())
		return false;
	char bytes[8192];
	while (waitFor(connection, false)) {
		int count = recv(connection.fd, bytes, sizeof(bytes), 0);
		if (count < 0 && wouldBlock())
			continue;
		if (count <= 0)
			break;
		response.append(bytes, count);
		if (response.size() > maxResponseBytes + 1)
			break;
		auto end = response.find('\n');
		if (end != std::string::npos) {
			response.resize(end);
			return true;
		}
	}
	return false;
}

}
