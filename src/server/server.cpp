#include "capi.h"

#include <core/application.h>

#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

#include <hv/HttpServer.h>

#include <cassert>
#include <cstdio>
#include <thread>

namespace server
{

using namespace hv;

class ServerApplication : public Application
{	
public:
	virtual ~ServerApplication() = default;

	bool tick()
	{
		auto c = getchar();
		printf("getchar(): %c\n", c);
		return c != '\n';
	}

protected:
	ServerApplication(Application::State&& state)
	 : Application(std::forward<Application::State>(state))
	 , myRouter([]
	 {
		HttpService router;
		router.GET("/ping", [](HttpRequest* req, HttpResponse* resp) {
			return resp->String("pong");
		});

		router.GET("/data", [](HttpRequest* req, HttpResponse* resp) {
			static char data[] = "0123456789";
			return resp->Data(data, 10);
		});

		router.GET("/paths", [&router](HttpRequest* req, HttpResponse* resp) {
			return resp->Json(router.Paths());
		});

		router.GET("/get", [](HttpRequest* req, HttpResponse* resp) {
			resp->json["origin"] = req->client_addr.ip;
			resp->json["url"] = req->url;
			resp->json["args"] = req->query_params;
			resp->json["headers"] = req->headers;
			return 200;
		});

		router.POST("/echo", [](const HttpContextPtr& ctx) {
			return ctx->send(ctx->body(), ctx->type());
		});

		return router;
	 }())
	 , myServer(&myRouter)
	{
		myServer.setPort(8080);
		myServer.setThreadNum(std::max(1u, std::thread::hardware_concurrency() - 1));
		myServer.run(false);
	}

private:
	HttpService myRouter;
	HttpServer myServer;
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
		Application::State{
			"server",
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

	return s_application->state().name.data();
}
