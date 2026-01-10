#include "serving/core/Session.h"
#include "llama.h"

ModelContext::~ModelContext()
{
    if (sampler)
        llama_sampler_free(sampler);
    if (ctx)
        llama_free(ctx);
}
