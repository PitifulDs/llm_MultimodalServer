(function (window) {
  const ns = window.EdgeLLMDemo = window.EdgeLLMDemo || {};

  const DEFAULT_AGENT_TOOLS = {
    code_analysis: ['search_kb', 'open_chunk', 'search_code', 'read_file', 'list_files', 'search_docs', 'get_config', 'get_server_status'],
    web_research: ['search_kb', 'open_chunk', 'search_code', 'read_file', 'list_files', 'search_docs', 'search_web', 'fetch_url', 'get_config', 'get_server_status'],
  };

  const AGENT_PRESETS = {
    analysis: {
      enableAgent: true,
      mode: 'code_analysis',
      maxTokens: 768,
      maxSteps: 4,
      tools: DEFAULT_AGENT_TOOLS.code_analysis,
      system: 'You are a concise read-only code-analysis agent for EdgeLLM-Serving. Prefer repository and local evidence first.',
      prompt: 'HttpGateway 里 agent 请求是怎么进入 AgentExecutor 的？请基于仓库代码和本地证据回答，并指出相关文件。',
    },
    research: {
      enableAgent: true,
      mode: 'web_research',
      maxTokens: 896,
      maxSteps: 8,
      tools: DEFAULT_AGENT_TOOLS.web_research,
      system: 'You are a concise read-only web-research agent for EdgeLLM-Serving. Use repository evidence first and web evidence only for cross-checking.',
      prompt: '结合仓库和外部网页资料，交叉验证 EdgeLLM-Serving 的 references 输出链路，并区分 repo_code 与 web 证据。',
    },
    status: {
      enableAgent: true,
      mode: 'code_analysis',
      maxTokens: 384,
      maxSteps: 3,
      tools: ['get_server_status'],
      system: 'You are a concise operations assistant for EdgeLLM-Serving.',
      prompt: '当前服务健康状态怎么样？有多少请求正在处理中？请基于真实状态回答。',
    },
    config: {
      enableAgent: true,
      mode: 'code_analysis',
      maxTokens: 384,
      maxSteps: 4,
      tools: ['get_config'],
      system: 'You are a concise configuration assistant for EdgeLLM-Serving.',
      prompt: '默认模型是什么？默认 max_tokens 是多少？如果能查到，也说明当前后端类型。',
    },
    docs: {
      enableAgent: true,
      mode: 'code_analysis',
      maxTokens: 512,
      maxSteps: 4,
      tools: ['search_docs'],
      system: 'You are a concise documentation assistant for EdgeLLM-Serving.',
      prompt: '这个项目是否支持流式输出？接口怎么调用？请基于项目文档回答。',
    },
  };

  const MIN_CHAT_MAX_TOKENS = 512;
  const MIN_AGENT_MAX_TOKENS = 768;

  function agentModeLabel(mode) {
    return mode === 'web_research' ? 'web_research' : 'code_analysis';
  }

  function getAgentModeHint(mode) {
    if (mode === 'web_research') {
      return 'Supplemental cross-check mode. Start from repo/docs evidence, then use controlled search_web and fetch_url only when external verification is needed.';
    }
    return 'Primary recommended mode. Read-only, evidence-first analysis that prioritizes repository and local evidence.';
  }

  function parseToolsInput(value) {
    return String(value || '')
      .split(',')
      .map(function (item) {
        return item.trim();
      })
      .filter(Boolean);
  }

  function normalizeToolsValue(value) {
    const tools = Array.isArray(value) ? value : parseToolsInput(value);
    return tools.join(',');
  }

  function getDefaultAgentTools(mode) {
    return DEFAULT_AGENT_TOOLS[mode] || DEFAULT_AGENT_TOOLS.code_analysis;
  }

  function getKnownDefaultToolSets() {
    return Object.keys(DEFAULT_AGENT_TOOLS).map(function (key) {
      return normalizeToolsValue(DEFAULT_AGENT_TOOLS[key]);
    });
  }

  function getEffectiveMaxTokens(rawValue, agentEnabled) {
    const parsed = parseInt(rawValue, 10) || 0;
    if (agentEnabled) {
      return Math.max(parsed || MIN_AGENT_MAX_TOKENS, MIN_AGENT_MAX_TOKENS);
    }
    return Math.max(parsed || MIN_CHAT_MAX_TOKENS, MIN_CHAT_MAX_TOKENS);
  }

  ns.AgentConfig = {
    AGENT_PRESETS: AGENT_PRESETS,
    DEFAULT_AGENT_TOOLS: DEFAULT_AGENT_TOOLS,
    MIN_AGENT_MAX_TOKENS: MIN_AGENT_MAX_TOKENS,
    MIN_CHAT_MAX_TOKENS: MIN_CHAT_MAX_TOKENS,
    agentModeLabel: agentModeLabel,
    getAgentModeHint: getAgentModeHint,
    getDefaultAgentTools: getDefaultAgentTools,
    getEffectiveMaxTokens: getEffectiveMaxTokens,
    getKnownDefaultToolSets: getKnownDefaultToolSets,
    normalizeToolsValue: normalizeToolsValue,
    parseToolsInput: parseToolsInput,
  };
})(window);
