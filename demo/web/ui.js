(function (window) {
  const ns = window.EdgeLLMDemo = window.EdgeLLMDemo || {};

  function byId(id) {
    return document.getElementById(id);
  }

  ns.createUI = function createUI() {
    const elements = {
      agentConfig: byId('agentConfig'),
      agentDisabledState: byId('agentDisabledState'),
      agentHint: byId('agentHint'),
      agentMaxSteps: byId('agentMaxSteps'),
      agentMode: byId('agentMode'),
      agentModeHint: byId('agentModeHint'),
      agentPreset: byId('agentPreset'),
      agentStatus: byId('agentStatus'),
      agentTools: byId('agentTools'),
      agentToolsCount: byId('agentToolsCount'),
      baseUrl: byId('baseUrl'),
      backendSupportHint: byId('backendSupportHint'),
      debugMode: byId('debugMode'),
      debugTarget: byId('debugTarget'),
      dur: byId('dur'),
      enableAgent: byId('enableAgent'),
      finishReason: byId('finishReason'),
      inferenceBackend: byId('inferenceBackend'),
      maxTokens: byId('maxTokens'),
      model: byId('model'),
      newSessionEach: byId('newSessionEach'),
      output: byId('output'),
      payloadPreview: byId('payloadPreview'),
      prompt: byId('prompt'),
      referencesEmpty: byId('referencesEmpty'),
      referencesList: byId('referencesList'),
      refsCount: byId('refsCount'),
      sendBtn: byId('sendBtn'),
      sessionId: byId('sessionId'),
      status: byId('status'),
      stopBtn: byId('stopBtn'),
      streamMode: byId('streamMode'),
      system: byId('system'),
      tokCount: byId('tokCount'),
      ttfb: byId('ttfb'),
    };

    function normalizeStateLabel(value) {
      return String(value || 'idle').toLowerCase().replace(/\s+/g, '-');
    }

    function setStatus(value) {
      elements.status.textContent = value;
      elements.status.dataset.state = normalizeStateLabel(value);
    }

    function setBusy(isBusy) {
      elements.sendBtn.disabled = !!isBusy;
      elements.stopBtn.disabled = !isBusy;
    }

    function setMetrics(values) {
      if (Object.prototype.hasOwnProperty.call(values, 'ttfb')) {
        elements.ttfb.textContent = values.ttfb;
      }
      if (Object.prototype.hasOwnProperty.call(values, 'tokens')) {
        elements.tokCount.textContent = values.tokens;
      }
      if (Object.prototype.hasOwnProperty.call(values, 'finishReason')) {
        elements.finishReason.textContent = values.finishReason;
      }
      if (Object.prototype.hasOwnProperty.call(values, 'duration')) {
        elements.dur.textContent = values.duration;
      }
    }

    function resetResponse() {
      elements.output.textContent = '';
      setMetrics({
        ttfb: '-',
        tokens: '0',
        finishReason: '-',
        duration: '-',
      });
    }

    function isOutputNearBottom() {
      const remaining = elements.output.scrollHeight - elements.output.scrollTop - elements.output.clientHeight;
      return remaining < 32;
    }

    function scrollOutputToBottom() {
      elements.output.scrollTop = elements.output.scrollHeight;
    }

    function renderAssistantText(state) {
      const keepBottom = state.shouldStickOutputToBottom || isOutputNearBottom();
      let cleaned = ns.Continuation.sanitizeAssistantText(state.rawResponseText);

      if (state.lastFinishReason === 'length' && cleaned && !state.isAutoContinuing) {
        cleaned += '\n\n[truncated: reached max_tokens]';
      }

      elements.output.textContent = cleaned;
      setMetrics({
        tokens: String(ns.Continuation.approxTokens(cleaned)),
        finishReason: state.lastFinishReason,
      });

      if (keepBottom) {
        scrollOutputToBottom();
      }
    }

    function syncSendButton(mode, agentEnabled) {
      if (mode === 'stream') {
        elements.sendBtn.textContent = agentEnabled ? 'Send Request [agent]' : 'Send Request';
        return;
      }
      elements.sendBtn.textContent = agentEnabled ? 'Send Once [agent]' : 'Send Once';
    }

    function syncBackendHint(config) {
      const configuredText = config.configuredBackends.length ? config.configuredBackends.join(', ') : '(none)';
      const gatewayText = config.gatewayBackends.join(', ');
      if (config.currentBackend === 'rpc') {
        elements.backendSupportHint.textContent =
          'Backend is request-level on the same logical model. configured_backends=[' + configuredText
          + '], gateway_backends=[' + gatewayText
          + ']. RPC requires unit-manager and node/test worker.';
        return;
      }

      elements.backendSupportHint.textContent =
        'Backend is request-level on the same logical model. configured_backends=[' + configuredText
        + '], gateway_backends=[' + gatewayText + '].';
    }

    function syncAgentSection(config) {
      const enabled = !!config.enabled;
      elements.agentConfig.hidden = !enabled;
      elements.agentDisabledState.hidden = enabled;
      elements.agentStatus.textContent = enabled
        ? 'Enabled · ' + ns.AgentConfig.agentModeLabel(config.mode) + ' · ' + config.maxSteps + ' steps'
        : 'Disabled. Requests go directly to the model.';
      elements.agentModeHint.textContent = ns.AgentConfig.getAgentModeHint(config.mode);
      elements.agentToolsCount.textContent = '(' + String(config.toolsCount) + ')';
    }

    return {
      elements: elements,
      isOutputNearBottom: isOutputNearBottom,
      renderAssistantText: renderAssistantText,
      resetResponse: resetResponse,
      scrollOutputToBottom: scrollOutputToBottom,
      setBusy: setBusy,
      setMetrics: setMetrics,
      setStatus: setStatus,
      syncAgentSection: syncAgentSection,
      syncBackendHint: syncBackendHint,
      syncSendButton: syncSendButton,
    };
  };
})(window);
