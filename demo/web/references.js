(function (window) {
  const ns = window.EdgeLLMDemo = window.EdgeLLMDemo || {};

  function escapeHtml(value) {
    return String(value == null ? '' : value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function normalizeReferences(items) {
    if (!Array.isArray(items)) return [];

    const out = [];
    const seen = new Set();

    items.forEach(function (item) {
      if (!item || typeof item !== 'object') return;

      const source = item.source || item.reference_source || item.kb || '';
      const path = item.path || '';
      const url = item.url || '';
      const chunkId = item.chunk_id || '';
      const title = item.title || item.symbol || path || url || chunkId || '(unknown)';
      const key = [
        source,
        path,
        url,
        chunkId,
        item.start_line || 0,
        item.end_line || 0,
      ].join('|');

      if (seen.has(key)) return;
      seen.add(key);

      out.push({
        source: source,
        title: title,
        path: path,
        url: url,
        chunkId: chunkId,
        symbol: item.symbol || '',
        startLine: item.start_line || 0,
        endLine: item.end_line || 0,
        snippet: item.snippet || '',
      });
    });

    return out;
  }

  function mergeReferences(existing, incoming) {
    return normalizeReferences([].concat(existing || [], incoming || []));
  }

  function extractReferencesFromResponse(json) {
    if (Array.isArray(json && json.references)) return normalizeReferences(json.references);
    if (Array.isArray(json && json.agent_result && json.agent_result.references)) {
      return normalizeReferences(json.agent_result.references);
    }
    return [];
  }

  function renderReferences(elements, refs) {
    const normalized = normalizeReferences(refs);
    elements.refsCount.textContent = '(' + String(normalized.length) + ')';

    if (!normalized.length) {
      elements.referencesEmpty.hidden = false;
      elements.referencesList.hidden = true;
      elements.referencesList.innerHTML = '';
      return;
    }

    elements.referencesEmpty.hidden = true;
    elements.referencesList.hidden = false;
    elements.referencesList.innerHTML = normalized.map(function (item) {
      const source = escapeHtml(item.source || 'unknown');
      const title = escapeHtml(item.title);
      const symbol = item.symbol ? ' · ' + escapeHtml(item.symbol) : '';
      const range = item.startLine
        ? ':' + item.startLine + (item.endLine && item.endLine > item.startLine ? '-' + item.endLine : '')
        : '';
      const location = item.url
        ? '<a class="reference-link" href="' + escapeHtml(item.url) + '" target="_blank" rel="noreferrer">' + escapeHtml(item.url) + '</a>'
        : escapeHtml(item.path || item.chunkId || '') + range;
      const snippet = item.snippet
        ? '<div class="reference-snippet">' + escapeHtml(item.snippet) + '</div>'
        : '';

      return ''
        + '<article class="reference-item">'
        + '  <div class="reference-top">'
        + '    <div class="reference-title">' + title + '</div>'
        + '    <span class="reference-source">' + source + '</span>'
        + '  </div>'
        + '  <div class="reference-meta">' + location + symbol + '</div>'
        + snippet
        + '</article>';
    }).join('');
  }

  ns.References = {
    extractReferencesFromResponse: extractReferencesFromResponse,
    mergeReferences: mergeReferences,
    normalizeReferences: normalizeReferences,
    renderReferences: renderReferences,
  };
})(window);
