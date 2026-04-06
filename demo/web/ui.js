(function (window) {
  const ns = window.EdgeLLMDemo = window.EdgeLLMDemo || {};

  function byId(id) {
    return document.getElementById(id);
  }

  function setHidden(element, hidden) {
    if (!element) return;
    element.hidden = !!hidden;
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
      agentSection: byId('agentSection'),
      agentStatus: byId('agentStatus'),
      agentTools: byId('agentTools'),
      agentToolsCount: byId('agentToolsCount'),
      backendSupportHint: byId('backendSupportHint'),
      baseUrl: byId('baseUrl'),
      debugMode: byId('debugMode'),
      debugTarget: byId('debugTarget'),
      documents: byId('documents'),
      documentsField: byId('documentsField'),
      dur: byId('dur'),
      enableAgent: byId('enableAgent'),
      finishReason: byId('finishReason'),
      inferenceBackend: byId('inferenceBackend'),
      maxTokens: byId('maxTokens'),
      maxTokensField: byId('maxTokensField'),
      model: byId('model'),
      newSessionEach: byId('newSessionEach'),
      output: byId('output'),
      payloadPreview: byId('payloadPreview'),
      prompt: byId('prompt'),
      promptHelp: byId('promptHelp'),
      promptLabel: byId('promptLabel'),
      referencesEmpty: byId('referencesEmpty'),
      referencesList: byId('referencesList'),
      refsCount: byId('refsCount'),
      requestMode: byId('requestMode'),
      rerankTopN: byId('rerankTopN'),
      rerankTopNField: byId('rerankTopNField'),
      sendBtn: byId('sendBtn'),
      sessionField: byId('sessionField'),
      sessionId: byId('sessionId'),
      status: byId('status'),
      stopBtn: byId('stopBtn'),
      streamMode: byId('streamMode'),
      streamModeField: byId('streamModeField'),
      system: byId('system'),
      systemField: byId('systemField'),
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

    function setBusy(isBusy, requestMode) {
      const isChat = (requestMode || elements.requestMode.value) === 'chat';
      elements.sendBtn.disabled = !!isBusy;
      elements.stopBtn.disabled = !isBusy || !isChat;
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
      let cleaned = state.rawResponseText || '';

      if (state.lastRequestMode === 'chat') {
        cleaned = ns.Continuation.sanitizeAssistantText(cleaned);
        if (state.lastFinishReason === 'length' && cleaned && !state.isAutoContinuing) {
          cleaned += '\n\n[truncated: reached max_tokens]';
        }
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

    function syncSendButton(mode, agentEnabled, requestMode) {
      const actualMode = requestMode || elements.requestMode.value;
      if (actualMode === 'chat') {
        if (mode === 'stream') {
          elements.sendBtn.textContent = agentEnabled ? 'Send Request [agent]' : 'Send Request';
          return;
        }
        elements.sendBtn.textContent = agentEnabled ? 'Send Once [agent]' : 'Send Once';
        return;
      }

      const labelMap = {
        embeddings: 'Run Embeddings',
        rerank: 'Run Rerank',
        models: 'Fetch Models',
        healthz: 'Fetch Healthz',
        admin_models_status: 'Fetch Model Status',
        admin_backends_status: 'Fetch Backend Status',
      };
      elements.sendBtn.textContent = labelMap[actualMode] || 'Send Request';
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

    function syncRequestMode(config) {
      const requestMode = config.requestMode || 'chat';
      const isChat = requestMode === 'chat';
      const isRerank = requestMode === 'rerank';
      const isEmbeddings = requestMode === 'embeddings';
      const needsPrompt = isChat || isRerank || isEmbeddings;
      const needsModel = requestMode !== 'healthz';

      setHidden(elements.streamModeField, !isChat);
      setHidden(elements.maxTokensField, !isChat);
      setHidden(elements.systemField, !isChat);
      setHidden(elements.sessionField, !isChat);
      setHidden(elements.documentsField, !isRerank);
      setHidden(elements.rerankTopNField, !isRerank);
      setHidden(elements.agentSection, !isChat);

      elements.model.disabled = !needsModel;
      elements.inferenceBackend.disabled = requestMode === 'healthz';
      elements.prompt.disabled = !needsPrompt;
      if (isRerank) {
        elements.promptLabel.textContent = 'Rerank Query';
        elements.prompt.placeholder = 'Enter the query used to score each document.';
        elements.promptHelp.textContent = 'This field maps to `query` in `POST /v1/rerank`.';
      } else if (isEmbeddings) {
        elements.promptLabel.textContent = 'Embeddings Input';
        elements.prompt.placeholder = 'Enter the input text for vectorization.';
        elements.promptHelp.textContent = 'This field maps to `input` in `POST /v1/embeddings`.';
      } else if (isChat) {
        elements.promptLabel.textContent = 'User Prompt';
        elements.prompt.placeholder = 'Ask the model something useful.';
        elements.promptHelp.textContent = 'Chat uses messages, embeddings use `input`, rerank uses this field as `query`, and GET endpoints ignore it.';
      } else {
        elements.promptLabel.textContent = 'Request Notes';
        elements.prompt.placeholder = 'Optional notes for your own context.';
        elements.promptHelp.textContent = 'This endpoint is a read-only GET request; prompt and system fields are ignored.';
      }
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
      syncRequestMode: syncRequestMode,
      syncSendButton: syncSendButton,
    };
  };
})(window);
