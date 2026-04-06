(function (window) {
  const ns = window.EdgeLLMDemo = window.EdgeLLMDemo || {};

  function getSuggestedBaseUrl() {
    const defaultUrl = 'http://127.0.0.1:8080';

    if (window.location.protocol !== 'http:' && window.location.protocol !== 'https:') {
      return defaultUrl;
    }

    const host = window.location.hostname;
    if (!host) {
      return defaultUrl;
    }

    const protocol = window.location.protocol === 'https:' ? 'https:' : 'http:';
    return protocol + '//' + host + ':8080';
  }

  function ensureBaseUrlDefault(input) {
    const current = String(input.value || '').trim();
    if (!current || current === 'http://127.0.0.1:8080') {
      input.value = getSuggestedBaseUrl();
    }
  }

  function formatFetchError(baseUrl, err) {
    if (err && err.name === 'AbortError') {
      return '[stopped] Request aborted.';
    }

    if (err && err.httpStatus) {
      return 'HTTP ' + err.httpStatus;
    }

    const raw = err && err.message ? err.message : String(err);
    if (raw.indexOf('Failed to fetch') !== -1) {
      return [
        '[network error] ' + raw,
        'Request target: ' + baseUrl,
        'If this page is opened from another machine, do not use 127.0.0.1 unless the API server is also running on that same machine.',
        'Use the Linux server IP or hostname instead, for example: http://<server-ip>:8080',
      ].join('\n');
    }

    return '[error] ' + raw;
  }

  function createHttpError(status) {
    const err = new Error('HTTP ' + status);
    err.httpStatus = status;
    return err;
  }

  async function loadModels(baseUrl) {
    const resp = await fetch(baseUrl + '/v1/models', { cache: 'no-store' });
    if (!resp.ok) {
      throw createHttpError(resp.status);
    }

    const json = await resp.json();
    const items = Array.isArray(json && json.data) ? json.data : [];
    return items;
  }

  function getModelInfo(modelCatalog, modelId) {
    return (modelCatalog || []).find(function (item) {
      return item && item.id === modelId;
    }) || null;
  }

  function getSupportedBackends(modelCatalog, modelId) {
    const info = getModelInfo(modelCatalog, modelId);
    const backends = Array.isArray(info && info.backends)
      ? info.backends.filter(function (value) {
          return value === 'local' || value === 'rpc';
        })
      : [];
    if (!backends.length) return [];
    return Array.from(new Set(backends));
  }

  function getGatewayBackends(modelCatalog, modelId) {
    const info = getModelInfo(modelCatalog, modelId);
    const backends = Array.isArray(info && info.gateway_backends)
      ? info.gateway_backends.filter(function (value) {
          return value === 'local' || value === 'rpc';
        })
      : [];
    if (!backends.length) return ['local', 'rpc'];
    return Array.from(new Set(backends));
  }

  function buildPayload(options) {
    const messages = [];

    if (options.system) {
      messages.push({ role: 'system', content: options.system });
    }
    messages.push({ role: 'user', content: options.prompt });

    const payload = {
      model: options.model,
      inference_backend: options.inferenceBackend,
      max_tokens: options.maxTokens,
      messages: messages,
      session_id: options.sessionId,
      request_id: ns.genId('req'),
    };

    if (options.agentEnabled) {
      payload.agent = true;
      payload.agent_mode = options.agentMode || 'code_analysis';
      payload.max_steps = parseInt(options.agentMaxSteps, 10) || 4;
      payload.tools = options.agentTools || [];
    }

    return payload;
  }

  async function sendStreamChat(options) {
    const start = performance.now();
    let firstByteAt = null;

    const resp = await fetch(options.baseUrl + '/v1/chat/completions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(options.payload),
      cache: 'no-store',
      signal: options.signal,
    });

    if (!resp.ok || !resp.body) {
      throw createHttpError(resp.status);
    }

    const reader = resp.body.getReader();
    const decoder = new TextDecoder('utf-8');
    let buffer = '';
    let sawDone = false;

    while (true) {
      const chunk = await reader.read();
      if (chunk.done) break;

      if (!firstByteAt) {
        firstByteAt = performance.now();
        if (typeof options.onFirstByte === 'function') {
          options.onFirstByte(Math.round(firstByteAt - start));
        }
      }

      buffer += decoder.decode(chunk.value, { stream: true });
      const parts = buffer.split('\n\n');
      buffer = parts.pop() || '';

      for (let index = 0; index < parts.length; index += 1) {
        const lines = parts[index].split('\n');

        for (let lineIndex = 0; lineIndex < lines.length; lineIndex += 1) {
          const line = lines[lineIndex];
          if (line.indexOf('data:') !== 0) continue;

          const data = line.slice(5).trim();
          if (data === '[DONE]') {
            sawDone = true;
            break;
          }

          try {
            const json = JSON.parse(data);
            if (Array.isArray(json && json.metadata && json.metadata.references) && typeof options.onReferences === 'function') {
              options.onReferences(json.metadata.references);
            }

            const choice = json && json.choices && json.choices[0] ? json.choices[0] : null;
            const delta = choice && choice.delta ? choice.delta.content || '' : '';
            const finishReason = choice ? choice.finish_reason : '';

            if (finishReason && typeof options.onFinishReason === 'function') {
              options.onFinishReason(finishReason);
            }
            if (delta && typeof options.onDelta === 'function') {
              options.onDelta(delta);
            }
          } catch (err) {
          }
        }

        if (sawDone) break;
      }

      if (sawDone) break;
    }
  }

  async function sendNonStreamChat(options) {
    const start = performance.now();
    const resp = await fetch(options.baseUrl + '/v1/chat/completions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(options.payload),
      cache: 'no-store',
      signal: options.signal,
    });
    const responseAt = performance.now();

    if (!resp.ok) {
      throw createHttpError(resp.status);
    }

    const json = await resp.json();
    const choice = json && json.choices && json.choices[0] ? json.choices[0] : null;

    return {
      ttfbMs: Math.round(responseAt - start),
      json: json,
      content: choice && choice.message ? choice.message.content || '' : '',
      finishReason: choice ? choice.finish_reason || '-' : '-',
      references: ns.References.extractReferencesFromResponse(json),
    };
  }

  ns.Api = {
    buildPayload: buildPayload,
    ensureBaseUrlDefault: ensureBaseUrlDefault,
    formatFetchError: formatFetchError,
    getGatewayBackends: getGatewayBackends,
    getSuggestedBaseUrl: getSuggestedBaseUrl,
    getSupportedBackends: getSupportedBackends,
    loadModels: loadModels,
    sendNonStreamChat: sendNonStreamChat,
    sendStreamChat: sendStreamChat,
  };
})(window);
