#include "HttpGateway.h"

#include "http_types.h"
#include "utils/json.hpp"

using json = nlohmann::json;

void HttpGateway::HandleModels(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    json items = json::array();
    const auto models = model_catalog_service_.ListModels();

    for (const auto &model : models)
    {
        json configured_backends = json::array();
        if (model.has_local)
            configured_backends.push_back("local");
        if (model.has_rpc)
            configured_backends.push_back("rpc");

        items.push_back({
            {"id", model.id},
            {"object", "model"},
            {"owned_by", "edge-llm-serving"},
            {"default", model.is_default},
            {"default_backend", model.default_backend},
            {"capabilities", model.capabilities},
            {"declared_backends", model.backends},
            {"backends", configured_backends},
            {"gateway_backends", json::array({"local", "rpc"})}
        });
    }

    json out = {
        {"object", "list"},
        {"data", items}
    };

    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump());
    res.End();
}
