(function (window) {
  const ns = window.EdgeLLMDemo = window.EdgeLLMDemo || {};

  const MAX_AUTO_CONTINUES = 4;
  const MAX_CONTINUATION_TOKENS = 2048;

  function approxTokens(text) {
    return Math.max(0, Math.round(String(text || '').length / 4));
  }

  function stripThinkBlocks(text) {
    let remaining = text || '';
    let out = '';

    while (remaining) {
      const start = remaining.indexOf('<think>');
      if (start === -1) {
        out += remaining;
        break;
      }

      out += remaining.slice(0, start);
      const afterStart = remaining.slice(start + '<think>'.length);
      const end = afterStart.indexOf('</think>');
      if (end === -1) {
        out += afterStart;
        break;
      }
      remaining = afterStart.slice(end + '</think>'.length);
    }

    return out.replace(/<\/?think>/g, '');
  }

  function decodeJsonStringFragment(value) {
    try {
      return JSON.parse('"' + value + '"');
    } catch (err) {
      return value
        .replace(/\\n/g, '\n')
        .replace(/\\t/g, '\t')
        .replace(/\\r/g, '\r')
        .replace(/\\"/g, '"')
        .replace(/\\\\/g, '\\');
    }
  }

  function formatAnswerValue(value) {
    if (typeof value === 'string') return value;
    if (Array.isArray(value)) {
      return value
        .map(function (item) {
          return formatAnswerValue(item);
        })
        .filter(Boolean)
        .join('\n');
    }
    if (value && typeof value === 'object') {
      if (typeof value.answer === 'string') return value.answer;
      try {
        return JSON.stringify(value, null, 2);
      } catch (err) {
        return String(value);
      }
    }
    if (value == null) return '';
    return String(value);
  }

  function extractJsonObject(text) {
    const first = text.indexOf('{');
    const last = text.lastIndexOf('}');
    if (first === -1 || last === -1 || first >= last) {
      return '';
    }
    return text.slice(first, last + 1);
  }

  function extractWrappedAnswer(text) {
    const candidate = extractJsonObject(String(text || '').trim());
    if (candidate) {
      try {
        const parsed = JSON.parse(candidate);
        if (Object.prototype.hasOwnProperty.call(parsed, 'answer')) {
          return formatAnswerValue(parsed.answer);
        }
      } catch (err) {
      }
    }

    const match = String(text || '').match(/"answer"\s*:\s*"((?:\\.|[^"\\])*)"/s);
    if (match) {
      return decodeJsonStringFragment(match[1]);
    }

    return null;
  }

  function sanitizeAssistantText(rawText) {
    const withoutThink = stripThinkBlocks(rawText || '').trim();
    if (!withoutThink) {
      return '';
    }

    const wrappedAnswer = extractWrappedAnswer(withoutThink);
    if (wrappedAnswer !== null) {
      return wrappedAnswer.trim();
    }

    return withoutThink;
  }

  function countMatches(text, pattern) {
    const matches = String(text || '').match(pattern);
    return matches ? matches.length : 0;
  }

  function looksIncompleteAnswer(text) {
    const value = String(text || '').trimEnd();
    if (!value) return false;

    const fenceCount = countMatches(value, /```/g);
    if (fenceCount % 2 === 1) {
      return true;
    }

    const hasCodeSignals = /#include|class\s+\w+|int\s+\w+\s*\(|using namespace|vector<|```(?:cpp|c\+\+)?/i.test(value);
    if (!hasCodeSignals) {
      return false;
    }

    const braceBalance = countMatches(value, /\{/g) - countMatches(value, /\}/g);
    if (braceBalance > 0) {
      return true;
    }

    const lines = value.split('\n').map(function (line) {
      return line.trim();
    }).filter(Boolean);
    const lastLine = lines.length ? lines[lines.length - 1] : '';
    if (!lastLine) {
      return false;
    }

    if (/^[A-Za-z_][A-Za-z0-9_]*$/.test(lastLine)) {
      return true;
    }

    if (/[=,+\-*/(]$/.test(lastLine)) {
      return true;
    }

    return false;
  }

  function shouldAutoContinue(reason, text) {
    if (!text) return false;
    if (reason === 'length') return true;
    if (reason === 'stop') return looksIncompleteAnswer(text);
    return false;
  }

  function buildContinuationPayload(options) {
    const continuationMaxTokens = Math.max(
      Math.min(options.maxTokens * 2, MAX_CONTINUATION_TOKENS),
      options.maxTokens
    );
    const messages = [];

    if (options.system) {
      messages.push({ role: 'system', content: options.system });
    }

    messages.push({ role: 'user', content: options.prompt });
    messages.push({ role: 'assistant', content: options.currentAnswer });
    messages.push({
      role: 'user',
      content: 'Continue exactly from where you stopped. Do not repeat previous content. Finish the remaining answer only.',
    });

    return {
      model: options.model,
      inference_backend: options.inferenceBackend,
      max_tokens: continuationMaxTokens,
      messages: messages,
      session_id: ns.genId('sess-continue'),
      request_id: ns.genId('req'),
    };
  }

  async function autoContinueResponse(options) {
    const onRender = typeof options.onRender === 'function' ? options.onRender : function () {};
    const onStatus = typeof options.onStatus === 'function' ? options.onStatus : function () {};
    let attempts = 0;

    try {
      while (attempts < MAX_AUTO_CONTINUES) {
        const currentAnswer = sanitizeAssistantText(options.state.rawResponseText);
        if (!shouldAutoContinue(options.state.lastFinishReason, currentAnswer)) {
          return;
        }

        attempts += 1;
        options.state.isAutoContinuing = true;
        onStatus('CONTINUING ' + attempts + '/' + MAX_AUTO_CONTINUES);
        onRender();

        const payload = buildContinuationPayload({
          model: options.model,
          maxTokens: options.maxTokens,
          system: options.system,
          prompt: options.prompt,
          currentAnswer: currentAnswer,
          inferenceBackend: options.inferenceBackend,
        });

        const resp = await fetch(options.baseUrl + '/v1/chat/completions', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(payload),
          cache: 'no-store',
          signal: options.signal,
        });

        if (!resp.ok) {
          return;
        }

        const json = await resp.json();
        const content = json && json.choices && json.choices[0] && json.choices[0].message
          ? json.choices[0].message.content || ''
          : '';

        if (!content) {
          return;
        }

        options.state.lastFinishReason =
          (json && json.choices && json.choices[0] && json.choices[0].finish_reason) || options.state.lastFinishReason;
        options.state.rawResponseText += content;
        options.state.isAutoContinuing = false;
        onRender();
      }
    } finally {
      if (options.state.isAutoContinuing) {
        options.state.isAutoContinuing = false;
        onRender();
      }
    }
  }

  ns.Continuation = {
    approxTokens: approxTokens,
    autoContinueResponse: autoContinueResponse,
    sanitizeAssistantText: sanitizeAssistantText,
  };
})(window);
