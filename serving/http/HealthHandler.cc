#include "HttpGateway.h"

#include "http_types.h"
#include "utils/json.hpp"

using json = nlohmann::json;

void HttpGateway::HandleHealthz(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    const json out = health_service_.BuildHealth(BuildPlatformRuntimeSnapshot());

    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump());
    res.End();
}
