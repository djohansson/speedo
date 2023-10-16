#include "capi.h"

#include <core/application.h>

#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

#include <grpc/support/log.h>
#include <grpcpp/grpcpp.h>

#include <cassert>
#include <cstdio>
#include <format>
#include <memory>
#include <thread>

namespace server
{

using namespace grpc;

class ServerApplication : public Application
{	
public:
	~ServerApplication() = default;

	bool tick() override
	{
		auto c = getchar();
		printf("getchar(): %c\n", c);
		return c != '\n';
	}

protected:
	ServerApplication(std::string_view name, Environment&& env)
	: Application(std::forward<std::string_view>(name), std::forward<Environment>(env))
	{
		constexpr int cx_port = 50051;
		static const std::string sc_serverAddress = std::format("0.0.0.0:%d", cx_port);

		ServerBuilder builder;
		builder.AddListeningPort(sc_serverAddress, grpc::InsecureServerCredentials());
		myCq = builder.AddCompletionQueue();
		myServer = builder.BuildAndStart();
		
		std::cout << "Server listening on " << sc_serverAddress << std::endl;
	}

private:
	std::unique_ptr<ServerCompletionQueue> myCq;
  	std::unique_ptr<Server> myServer;
};

static std::shared_ptr<ServerApplication> s_application{};

std::filesystem::path
getCanonicalPath(const char* pathStr, const char* defaultPathStr, bool createIfMissing = false)
{
	assert(defaultPathStr != nullptr);
	
	auto path = std::filesystem::path(pathStr ? pathStr : defaultPathStr);

	if (createIfMissing && !std::filesystem::exists(path))
		std::filesystem::create_directory(path);

	assert(std::filesystem::is_directory(path));

	return std::filesystem::canonical(path);
}

} // namespace server

void server_create(
	const char* rootPath,
	const char* resourcePath,
	const char* userProfilePath)
{
	using namespace server;

	s_application = Application::create<server::ServerApplication>(
		"server",
		Environment{
			getCanonicalPath(rootPath, "./"),
			getCanonicalPath(resourcePath, "./resources/"),
			getCanonicalPath(userProfilePath, "./.profile/", true)
		});

	assert(s_application);
}

bool server_tick()
{
	using namespace server;

	assert(s_application);

	return s_application->tick();
}

void server_destroy()
{
	using namespace server;

	assert(s_application);
	
	s_application.reset();
}

const char* server_getAppName(void)
{
	using namespace server;

	assert(s_application);

	return s_application->name().data();
}
