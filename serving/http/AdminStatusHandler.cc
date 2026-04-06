#include "HttpGateway.h"

#include "http_types.h"
#include "utils/json.hpp"

using json = nlohmann::json;

void HttpGateway::HandleAdminModelsStatus(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    const json out = admin_status_service_.BuildModelsStatus(model_catalog_service_.ListModels(),
                                                             BuildBackendRuntimeSnapshots());

    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump());
    res.End();
}

void HttpGateway::HandleAdminBackendsStatus(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    const json out = admin_status_service_.BuildBackendsStatus(
        model_catalog_service_.ListModels(),
        BuildBackendRuntimeSnapshots(),
        BuildPlatformRuntimeSnapshot());

    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump());
    res.End();
}
