#include "engine/ModelContext.h"
#include "llama.h"

ModelContext::~ModelContext()
{
    if (sampler)
    {
        llama_sampler_free(sampler);
        sampler = nullptr;
    }
    if (ctx)
    {
        llama_free(ctx);
        ctx = nullptr;
    }
}
