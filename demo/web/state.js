(function (window) {
  const ns = window.EdgeLLMDemo = window.EdgeLLMDemo || {};

  ns.createState = function createState() {
    return {
      aborter: null,
      modelCatalog: [],
      rawResponseText: '',
      lastReferences: [],
      lastPresetSystemPrompt: '',
      lastFinishReason: '-',
      lastDebugTarget: '',
      lastPayload: null,
      shouldStickOutputToBottom: true,
      isAutoContinuing: false,
    };
  };

  ns.genId = function genId(prefix) {
    const safePrefix = prefix || 'id';
    if (window.crypto && typeof window.crypto.randomUUID === 'function') {
      return safePrefix + '-' + window.crypto.randomUUID();
    }
    return safePrefix + '-' + Date.now().toString(36) + '-' + Math.random().toString(36).slice(2, 8);
  };
})(window);
