(function (window) {
  const ns = window.EdgeLLMDemo = window.EdgeLLMDemo || {};

  function backendLabel(value) {
    return value === 'rpc' ? 'RPC / Worker' : 'Local llama.cpp';
  }

  function buildAgentHint(config) {
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
    elements.debugMode.textContent = config.agentEnabled ? 'AGENT' : 'CHAT';
    elements.debugTarget.textContent = config.target || 'No request sent yet.';
    elements.agentHint.textContent = buildAgentHint(config);
    elements.payloadPreview.textContent = config.payload
      ? JSON.stringify(config.payload, null, 2)
      : 'No request sent yet.';
  }

  ns.DebugPanel = {
    backendLabel: backendLabel,
    buildAgentHint: buildAgentHint,
    renderDebugPanel: renderDebugPanel,
  };
})(window);
