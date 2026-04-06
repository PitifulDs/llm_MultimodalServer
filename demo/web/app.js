(function (window) {
  const ns = window.EdgeLLMDemo = window.EdgeLLMDemo || {};

  function init() {
    const state = ns.createState();
    const ui = ns.createUI();
    const els = ui.elements;

    function parseAgentTools() {
      return ns.AgentConfig.parseToolsInput(els.agentTools.value);
    }

    function renderReferences() {
      ns.References.renderReferences(els, state.lastReferences);
    }

    function renderDebugPanel() {
      ns.DebugPanel.renderDebugPanel(els, {
        agentEnabled: els.enableAgent.checked,
        agentMode: els.agentMode.value,
        agentTools: parseAgentTools(),
        inferenceBackend: els.inferenceBackend.value,
        payload: state.lastPayload,
        target: state.lastDebugTarget,
      });
    }

    function syncBackendOptions() {
      const configuredBackends = ns.Api.getSupportedBackends(state.modelCatalog, els.model.value);
      const gatewayBackends = ns.Api.getGatewayBackends(state.modelCatalog, els.model.value);
      let nextValue = els.inferenceBackend.value;

      if (nextValue !== 'local' && nextValue !== 'rpc') {
        nextValue = 'local';
      }
      els.inferenceBackend.value = nextValue;

      ui.syncBackendHint({
        configuredBackends: configuredBackends,
        currentBackend: nextValue,
        gatewayBackends: gatewayBackends,
      });
    }

    function populateModelSelect(items) {
      if (!items.length) return;

      const currentValue = els.model.value;
      const previousOptions = items.map(function (item) {
        return item && item.id;
      }).filter(Boolean);
      let selectedValue = currentValue;

      els.model.innerHTML = '';
      items.forEach(function (item) {
        if (!item || !item.id) return;

        const option = document.createElement('option');
        option.value = item.id;
        option.textContent = item.default ? item.id + ' (default)' : item.id;
        if (item.default) {
          selectedValue = item.id;
        }
        els.model.appendChild(option);
      });

      if (currentValue && previousOptions.indexOf(currentValue) !== -1) {
        selectedValue = currentValue;
      }
      els.model.value = selectedValue;
    }

    async function loadModels() {
      const baseUrl = String(els.baseUrl.value || '').trim();
      if (!baseUrl) {
        syncBackendOptions();
        return;
      }

      try {
        const items = await ns.Api.loadModels(baseUrl);
        if (items.length) {
          state.modelCatalog = items;
          populateModelSelect(items);
        }
      } catch (err) {
      }

      syncBackendOptions();
    }

    function syncAgentModeDefaults(force) {
      const selectedMode = els.agentMode.value || 'code_analysis';
      const currentValue = ns.AgentConfig.normalizeToolsValue(parseAgentTools());
      const knownDefaults = ns.AgentConfig.getKnownDefaultToolSets();

      if (force || !currentValue || knownDefaults.indexOf(currentValue) !== -1) {
        els.agentTools.value = ns.AgentConfig.getDefaultAgentTools(selectedMode).join(',');
      }

      if (selectedMode === 'web_research' && (parseInt(els.agentMaxSteps.value, 10) || 0) < 8) {
        els.agentMaxSteps.value = '8';
      }
    }

    function updateAgentUI() {
      const enabled = els.enableAgent.checked;

      if (!enabled && els.system.value.trim() && els.system.value.trim() === state.lastPresetSystemPrompt) {
        els.system.value = '';
      }

      syncAgentModeDefaults(false);
      ui.syncSendButton(els.streamMode.value, enabled);
      ui.syncAgentSection({
        enabled: enabled,
        maxSteps: parseInt(els.agentMaxSteps.value, 10) || 4,
        mode: els.agentMode.value || 'code_analysis',
        toolsCount: parseAgentTools().length,
      });
      renderDebugPanel();
    }

    function applyPreset(name) {
      const preset = ns.AgentConfig.AGENT_PRESETS[name];
      if (!preset) {
        state.lastPresetSystemPrompt = '';
        updateAgentUI();
        return;
      }

      els.enableAgent.checked = !!preset.enableAgent;
      if (preset.mode) {
        els.agentMode.value = preset.mode;
      }
      if (preset.maxTokens) {
        els.maxTokens.value = String(preset.maxTokens);
      }
      els.agentMaxSteps.value = String(preset.maxSteps || 4);
      els.agentTools.value = (preset.tools || []).join(',');
      els.system.value = preset.system || '';
      els.prompt.value = preset.prompt || '';
      state.lastPresetSystemPrompt = preset.system || '';
      syncAgentModeDefaults(true);
      updateAgentUI();
    }

    function resetRequestState() {
      state.rawResponseText = '';
      state.lastReferences = [];
      state.lastFinishReason = '-';
      state.shouldStickOutputToBottom = true;
      state.isAutoContinuing = false;
      renderReferences();
      ui.resetResponse();
      ui.scrollOutputToBottom();
    }

    function buildRequestContext() {
      const baseUrl = String(els.baseUrl.value || '').trim();
      const prompt = String(els.prompt.value || '').trim();
      let sessionId = String(els.sessionId.value || '').trim();

      if (!baseUrl || !prompt) {
        return null;
      }

      if (els.newSessionEach.checked || !sessionId) {
        sessionId = ns.genId('sess');
        els.sessionId.value = sessionId;
      }

      const context = {
        agentEnabled: els.enableAgent.checked,
        agentMaxSteps: els.agentMaxSteps.value,
        agentMode: els.agentMode.value || 'code_analysis',
        agentTools: parseAgentTools(),
        baseUrl: baseUrl,
        inferenceBackend: els.inferenceBackend.value,
        maxTokens: ns.AgentConfig.getEffectiveMaxTokens(els.maxTokens.value, els.enableAgent.checked),
        model: String(els.model.value || '').trim(),
        prompt: prompt,
        sessionId: sessionId,
        system: String(els.system.value || '').trim(),
      };

      context.payload = ns.Api.buildPayload(context);
      return context;
    }

    async function runStreamRequest(context, startedAt) {
      await ns.Api.sendStreamChat({
        baseUrl: context.baseUrl,
        onDelta: function (delta) {
          state.rawResponseText += delta;
          ui.renderAssistantText(state);
        },
        onFinishReason: function (finishReason) {
          state.lastFinishReason = finishReason;
          ui.renderAssistantText(state);
        },
        onFirstByte: function (ttfbMs) {
          ui.setMetrics({ ttfb: String(ttfbMs) + ' ms' });
        },
        onReferences: function (items) {
          state.lastReferences = ns.References.mergeReferences(state.lastReferences, items);
          renderReferences();
        },
        payload: context.payload,
        signal: state.aborter.signal,
      });

      await ns.Continuation.autoContinueResponse({
        baseUrl: context.baseUrl,
        inferenceBackend: context.inferenceBackend,
        maxTokens: context.maxTokens,
        model: context.model,
        onRender: function () {
          ui.renderAssistantText(state);
        },
        onStatus: ui.setStatus,
        prompt: context.prompt,
        signal: state.aborter.signal,
        state: state,
        system: context.system,
      });

      ui.setStatus('DONE');
      ui.setMetrics({
        duration: String(Math.round(performance.now() - startedAt)) + ' ms',
      });
    }

    async function runNonStreamRequest(context, startedAt) {
      const result = await ns.Api.sendNonStreamChat({
        baseUrl: context.baseUrl,
        payload: context.payload,
        signal: state.aborter.signal,
      });

      state.lastReferences = ns.References.mergeReferences(state.lastReferences, result.references);
      state.lastFinishReason = result.finishReason;
      state.rawResponseText = result.content || '';
      renderReferences();
      ui.setMetrics({
        ttfb: String(result.ttfbMs) + ' ms',
      });
      ui.renderAssistantText(state);

      await ns.Continuation.autoContinueResponse({
        baseUrl: context.baseUrl,
        inferenceBackend: context.inferenceBackend,
        maxTokens: context.maxTokens,
        model: context.model,
        onRender: function () {
          ui.renderAssistantText(state);
        },
        onStatus: ui.setStatus,
        prompt: context.prompt,
        signal: state.aborter.signal,
        state: state,
        system: context.system,
      });

      ui.setStatus('DONE');
      ui.setMetrics({
        duration: String(Math.round(performance.now() - startedAt)) + ' ms',
      });
    }

    async function handleSend() {
      const context = buildRequestContext();
      if (!context) {
        return;
      }

      resetRequestState();

      if (els.streamMode.value === 'stream') {
        context.payload.stream = true;
      }

      state.lastDebugTarget = context.baseUrl + '/v1/chat/completions';
      state.lastPayload = context.payload;
      renderDebugPanel();

      ui.setBusy(true);
      ui.setStatus(els.streamMode.value === 'stream' ? 'STREAMING' : 'SENDING');
      state.aborter = new AbortController();

      const startedAt = performance.now();

      try {
        if (els.streamMode.value === 'stream') {
          await runStreamRequest(context, startedAt);
        } else {
          await runNonStreamRequest(context, startedAt);
        }
      } catch (err) {
        const aborted = err && err.name === 'AbortError';
        ui.setStatus(aborted ? 'STOPPED' : (err && err.httpStatus ? 'ERROR' : 'STOPPED'));
        state.rawResponseText = state.rawResponseText
          ? state.rawResponseText + '\n' + ns.Api.formatFetchError(context.baseUrl, err)
          : ns.Api.formatFetchError(context.baseUrl, err);
        ui.renderAssistantText(state);
      } finally {
        ui.setBusy(false);
        state.aborter = null;
      }
    }

    function bindEvents() {
      els.sendBtn.addEventListener('click', handleSend);
      els.stopBtn.addEventListener('click', function () {
        if (state.aborter) {
          state.aborter.abort();
        }
      });
      els.output.addEventListener('scroll', function () {
        state.shouldStickOutputToBottom = ui.isOutputNearBottom();
      });
      els.streamMode.addEventListener('change', function () {
        ui.syncSendButton(els.streamMode.value, els.enableAgent.checked);
        renderDebugPanel();
      });
      els.model.addEventListener('change', function () {
        syncBackendOptions();
        renderDebugPanel();
      });
      els.baseUrl.addEventListener('change', loadModels);
      els.inferenceBackend.addEventListener('change', function () {
        syncBackendOptions();
        renderDebugPanel();
      });
      els.enableAgent.addEventListener('change', updateAgentUI);
      els.agentMode.addEventListener('change', function () {
        syncAgentModeDefaults(false);
        updateAgentUI();
      });
      els.agentMaxSteps.addEventListener('input', updateAgentUI);
      els.agentPreset.addEventListener('change', function () {
        applyPreset(els.agentPreset.value);
      });
      els.agentTools.addEventListener('input', updateAgentUI);
      els.system.addEventListener('input', renderDebugPanel);
      els.prompt.addEventListener('input', renderDebugPanel);
      els.sessionId.addEventListener('input', renderDebugPanel);
      els.newSessionEach.addEventListener('change', renderDebugPanel);
    }

    function initialize() {
      ns.Api.ensureBaseUrlDefault(els.baseUrl);
      bindEvents();
      renderReferences();
      updateAgentUI();
      ui.setStatus('IDLE');
      renderDebugPanel();
      loadModels();
    }

    initialize();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
    return;
  }

  init();
})(window);
