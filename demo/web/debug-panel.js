(function (window) {
  const ns = window.EdgeLLMDemo = window.EdgeLLMDemo || {};

  function backendLabel(value) {
    return value === 'rpc' ? 'RPC / Worker' : 'Local llama.cpp';
  }

  function requestModeLabel(mode) {
    const labels = {
      chat: 'chat',
      embeddings: 'embeddings',
      rerank: 'rerank',
      models: 'models',
      healthz: 'healthz',
      admin_models_status: 'admin models status',
      admin_backends_status: 'admin backends status',
    };
    return labels[mode] || mode || 'chat';
  }

  function buildAgentHint(config) {
    const requestMode = config.requestMode || 'chat';
    if (requestMode !== 'chat') {
      return [
        'Current mode: ' + requestModeLabel(requestMode),
        'Inference backend: ' + backendLabel(config.inferenceBackend),
        'Primary route only. Agent and session semantics are bypassed for this endpoint.',
        'Use this view to inspect platform metadata, health, embeddings, or rerank behavior.',
      ].join('\n');
    }

    if (!config.agentEnabled) {
      return [
        'Current mode: normal chat',
        'Inference backend: ' + backendLabel(config.inferenceBackend),
        'Requests go directly to /v1/chat/completions.',
        'Use this for ordinary conversation, writing, translation, or quick model checks.',
      ].join('\n');
    }

    const tools = Array.isArray(config.agentTools) ? config.agentTools : [];
    const mode = config.agentMode || 'code_analysis';

    return [
      'Current mode: agent',
      'Inference backend: ' + backendLabel(config.inferenceBackend),
      'Agent mode: ' + ns.AgentConfig.agentModeLabel(mode),
      'Allowed tools: ' + (tools.length ? tools.join(', ') : '(none)'),
      mode === 'web_research'
        ? 'Supplemental cross-check mode. Prefer repo evidence first, then use controlled search_web and fetch_url for external verification.'
        : 'Primary public mode. Use this when the answer should come from repo files, docs, config, or live server state.',
      'Server-side tool calls are logged as: [agent] req=... step=... tool=...',
    ].join('\n');
  }

  function renderDebugPanel(elements, config) {
    elements.debugMode.textContent = (config.requestMode || 'chat').toUpperCase();
    elements.debugTarget.textContent = config.target || 'No request sent yet.';
    elements.agentHint.textContent = buildAgentHint(config);
    elements.payloadPreview.textContent = config.payload
      ? JSON.stringify(config.payload, null, 2)
      : 'No payload for this request.';
  }

  ns.DebugPanel = {
    backendLabel: backendLabel,
    buildAgentHint: buildAgentHint,
    renderDebugPanel: renderDebugPanel,
    requestModeLabel: requestModeLabel,
  };
})(window);
