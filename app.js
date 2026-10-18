/* Network console UI. All displayed network data is sourced from the REST API. */
(function () {
  'use strict';

  const API_ROOT = '/sim-api/v1';
  const POLL_INTERVAL_MS = 3000;
  const $ = (selector) => document.querySelector(selector);
  const svgNS = 'http://www.w3.org/2000/svg';
  let currentState = null;
  let requestBusy = false;
  let pollInFlight = false;

  const setLoading = (loading) => {
    $('#loading-cover').classList.toggle('visible', loading);
  };

  function showError(message) {
    $('#error-message').textContent = message;
    $('#error-banner').classList.add('visible');
    $('#api-dot').classList.add('offline');
    $('#api-status').textContent = 'API UNAVAILABLE';
    if (!currentState) $('#diagram-empty').hidden = false;
  }

  function clearError() {
    $('#error-banner').classList.remove('visible');
    $('#diagram-empty').hidden = true;
    $('#api-dot').classList.remove('offline');
    $('#api-status').textContent = 'API CONNECTED';
  }

  async function readState(options = {}) {
    const { initial = false, quiet = false } = options;
    if (pollInFlight) return;
    pollInFlight = true;
    if (initial) setLoading(true);
    try {
      const response = await fetch(`${API_ROOT}/state`, {
        method: 'GET',
        headers: { Accept: 'application/json' },
        cache: 'no-store'
      });
      if (!response.ok) throw new Error(`State request returned HTTP ${response.status}.`);
      const data = await response.json();
      if (!data || !Array.isArray(data.routers) || !Array.isArray(data.links)) {
        throw new Error('The API returned an unexpected network state.');
      }
      currentState = data;
      render(data);
      clearError();
    } catch (error) {
      showError(error.message || 'Unable to reach the simulator API.');
      if (!quiet) console.error('Network state refresh failed:', error);
    } finally {
      setLoading(false);
      pollInFlight = false;
    }
  }

  async function postAction(path, body) {
    if (requestBusy) return;
    requestBusy = true;
    document.querySelectorAll('button, select').forEach((element) => { element.disabled = true; });
    try {
      const response = await fetch(`${API_ROOT}${path}`, {
        method: 'POST',
        headers: { Accept: 'application/json', ...(body ? { 'Content-Type': 'application/json' } : {}) },
        ...(body ? { body: JSON.stringify(body) } : {})
      });
      if (!response.ok) {
        let details = '';
        try {
          const payload = await response.json();
          details = payload.error || payload.message || '';
        } catch (_) { /* The API may return an empty response body. */ }
        throw new Error(details || `Operation failed with HTTP ${response.status}.`);
      }
      // Do not let an older poll response race with the post-mutation refresh.
      while (pollInFlight) await new Promise((resolve) => window.setTimeout(resolve, 25));
      await readState({ quiet: false });
    } catch (error) {
      showError(error.message || 'The requested operation could not be completed.');
    } finally {
      requestBusy = false;
      document.querySelectorAll('button, select').forEach((element) => { element.disabled = false; });
      syncTopologyControls();
    }
  }

  function formatTopology(topology) {
    const value = String(topology || 'unknown');
    return value.charAt(0).toUpperCase() + value.slice(1);
  }

  function formatTime(value) {
    if (value === undefined || value === null || value === '') return '—';
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return String(value).slice(0, 8);
    return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false });
  }

  function setMetricValue(id, value, suffix) {
    const element = $(id);
    element.replaceChildren(document.createTextNode(value ?? '—'));
    if (suffix) {
      const small = document.createElement('small');
      small.textContent = suffix;
      element.append(small);
    }
  }

  function renderMetrics(state) {
    const metrics = state.metrics || {};
    const availability = Number(metrics.availability);
    setMetricValue('#metric-availability', Number.isFinite(availability) ? `${availability}` : '—', '%');
    setMetricValue('#metric-routers', metrics.onlineRouters ?? '—', ` / ${metrics.totalRouters ?? '—'}`);
    setMetricValue('#metric-failed', metrics.failedRouters ?? '—', '');
    setMetricValue('#metric-links', metrics.activeLinks ?? '—', ` / ${metrics.totalLinks ?? '—'}`);
    $('#metric-topology').textContent = formatTopology(state.topology);
    $('#metric-failed').classList.toggle('bad', Number(metrics.failedRouters) > 0);
    $('#metric-failed').classList.toggle('good', Number(metrics.failedRouters) === 0);
    $('#metric-availability').classList.toggle('bad', Number.isFinite(availability) && availability < 100);
    $('#link-count-label').textContent = `${metrics.activeLinks ?? 0} / ${metrics.totalLinks ?? 0} LINKS ACTIVE`;
    $('#router-summary').textContent = `${metrics.totalRouters ?? state.routers.length} NODES · ${metrics.onlineRouters ?? 0} ONLINE`;
    $('#topology-select').value = ['star', 'ring', 'mesh'].includes(state.topology) ? state.topology : 'star';
    $('#router-count').value = String(metrics.totalRouters ?? state.routers.length);
    syncTopologyControls();
  }

  function syncTopologyControls() {
    const topology = currentState && currentState.topology;
    document.querySelectorAll('[data-topology]').forEach((button) => {
      button.classList.toggle('selected', button.dataset.topology === topology);
      button.setAttribute('aria-pressed', String(button.dataset.topology === topology));
    });
    if (topology && ['star', 'ring', 'mesh'].includes(topology)) $('#topology-select').value = topology;
  }

  function positionsFor(routers, topology, width, height) {
    const count = routers.length;
    const cx = width / 2;
    const cy = height / 2;
    const radiusX = Math.max(90, width / 2 - 52);
    const radiusY = Math.max(70, height / 2 - 70);
    if (count === 0) return new Map();
    const positions = new Map();
    if (topology === 'star' && count > 1) {
      positions.set(String(routers[0].id), { x: cx, y: cy });
      routers.slice(1).forEach((router, index) => {
        const angle = (-Math.PI / 2) + (index * (Math.PI * 2 / (count - 1)));
        positions.set(String(router.id), { x: cx + Math.cos(angle) * radiusX, y: cy + Math.sin(angle) * radiusY });
      });
      return positions;
    }
    routers.forEach((router, index) => {
      const angle = (-Math.PI / 2) + (index * (Math.PI * 2 / count));
      positions.set(String(router.id), { x: cx + Math.cos(angle) * radiusX, y: cy + Math.sin(angle) * radiusY });
    });
    return positions;
  }

  function svgElement(name, attrs) {
    const element = document.createElementNS(svgNS, name);
    Object.entries(attrs || {}).forEach(([key, value]) => element.setAttribute(key, value));
    return element;
  }

  function renderTopology(state) {
    const svg = $('#topology-svg');
    svg.replaceChildren();
    const width = Math.round(svg.getBoundingClientRect().width) || 800;
    const height = Math.round(svg.getBoundingClientRect().height) || 390;
    svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
    const routers = state.routers || [];
    const byId = new Map(routers.map((router) => [String(router.id), router]));
    const positions = positionsFor(routers, state.topology, width, height);
    const edgesLayer = svgElement('g', { 'aria-hidden': 'true' });

    (state.links || []).forEach((link) => {
      const start = positions.get(String(link.source));
      const end = positions.get(String(link.target));
      if (!start || !end) return;
      const active = Boolean(link.active);
      const line = svgElement('line', {
        x1: start.x, y1: start.y, x2: end.x, y2: end.y,
        class: `edge ${active ? 'active' : 'failed'}`
      });
      edgesLayer.append(line);
      if (active) {
        edgesLayer.append(svgElement('line', {
          x1: start.x, y1: start.y, x2: end.x, y2: end.y, class: 'edge-glint'
        }));
      }
    });
    svg.append(edgesLayer);

    routers.forEach((router) => {
      const point = positions.get(String(router.id));
      const online = Boolean(router.online);
      const group = svgElement('g', {
        class: `node-group${online ? '' : ' failed'}`,
        transform: `translate(${point.x} ${point.y})`,
        tabindex: '0',
        role: 'button',
        'aria-label': `${router.name || `Router ${router.id}`}, ${online ? 'online' : 'failed'}. Activate to ${online ? 'fail' : 'recover'} router.`,
        'data-router-id': router.id
      });
      group.append(svgElement('circle', { r: 36, class: 'node-halo' }));
      group.append(svgElement('circle', { r: 27, class: 'node-core' }));
      const icon = svgElement('g', { class: 'node-icon', transform: 'translate(-9 -8)', fill: 'none', stroke: 'currentColor', 'stroke-width': '1.25', 'stroke-linecap': 'round' });
      icon.append(svgElement('rect', { x: 1, y: 1, width: 16, height: 14, rx: 2 }));
      icon.append(svgElement('path', { d: 'M4 5h10M4 8h10M4 11h6' }));
      group.append(icon);
      const label = svgElement('text', { x: 0, y: 48, class: 'node-label' });
      label.textContent = router.name || `Router ${router.id}`;
      group.append(label);
      const status = svgElement('text', { x: 0, y: 62, class: 'node-state' });
      status.textContent = online ? 'ONLINE' : 'FAILED';
      group.append(status);
      group.addEventListener('click', () => toggleRouter(router));
      group.addEventListener('keydown', (event) => {
        if (event.key === 'Enter' || event.key === ' ') {
          event.preventDefault();
          toggleRouter(router);
        }
      });
      svg.append(group);
    });
    svg.setAttribute('aria-label', `${formatTopology(state.topology)} topology with ${routers.length} routers and ${(state.links || []).length} links.`);
  }

  function renderRouterList(routers) {
    const list = $('#router-list');
    list.replaceChildren();
    routers.forEach((router) => {
      const online = Boolean(router.online);
      const row = document.createElement('div');
      row.className = `router-row${online ? '' : ' failed'}`;
      row.dataset.testid = `row-router-${router.id}`;
      const led = document.createElement('i');
      led.className = 'router-led';
      const copy = document.createElement('div');
      copy.className = 'router-copy';
      const name = document.createElement('div');
      name.className = 'router-name';
      name.textContent = router.name || `Router ${router.id}`;
      name.dataset.testid = `text-router-name-${router.id}`;
      const id = document.createElement('div');
      id.className = 'router-id';
      id.textContent = `ID · ${router.id}`;
      copy.append(name, id);
      const status = document.createElement('span');
      status.className = 'router-status';
      status.textContent = online ? 'UP' : 'DOWN';
      const action = document.createElement('button');
      action.className = 'router-action';
      action.type = 'button';
      action.textContent = online ? 'FAIL' : 'RECOVER';
      action.setAttribute('aria-label', `${online ? 'Fail' : 'Recover'} ${router.name || `router ${router.id}`}`);
      action.dataset.testid = `${online ? 'button-fail' : 'button-recover'}-router-${router.id}`;
      action.addEventListener('click', () => toggleRouter(router));
      row.append(led, copy, status, action);
      list.append(row);
    });
  }

  function renderChips(target, values, unreachable) {
    const container = $(target);
    container.replaceChildren();
    if (!Array.isArray(values) || values.length === 0) {
      const empty = document.createElement('span');
      empty.className = 'empty-chip';
      empty.textContent = 'NONE';
      container.append(empty);
      return;
    }
    values.forEach((id) => {
      const router = currentState.routers.find((item) => String(item.id) === String(id));
      const chip = document.createElement('span');
      chip.className = `chip${unreachable ? ' unreachable' : ''}`;
      chip.textContent = router ? router.name : `R${id}`;
      container.append(chip);
    });
  }

  function eventClass(event) {
    const type = `${event.type || ''} ${event.message || ''}`.toLowerCase();
    if (/fail|down|offline/.test(type)) return 'failure';
    if (/recover|online|up/.test(type)) return 'recovery';
    return '';
  }

  function renderEvents(events) {
    const container = $('#event-list');
    container.replaceChildren();
    if (!Array.isArray(events) || events.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'event-row';
      empty.innerHTML = '<span class="event-time">—</span><i class="event-dot"></i><span class="event-message">No events recorded yet.</span>';
      container.append(empty);
      return;
    }
    events.slice(0, 8).forEach((event) => {
      const row = document.createElement('div');
      row.className = `event-row ${eventClass(event)}`;
      const time = document.createElement('span');
      time.className = 'event-time';
      time.textContent = formatTime(event.timestamp);
      const dot = document.createElement('i');
      dot.className = 'event-dot';
      const message = document.createElement('span');
      message.className = 'event-message';
      message.textContent = event.message || `${event.type || 'Network event'}${event.routerId ? ` · Router ${event.routerId}` : ''}`;
      const type = document.createElement('span');
      type.className = 'event-type';
      type.textContent = event.type || 'EVENT';
      message.append(type);
      row.append(time, dot, message);
      container.append(row);
    });
  }

  function render(state) {
    renderMetrics(state);
    renderTopology(state);
    renderRouterList(state.routers);
    renderChips('#reachable-list', state.reachableRouters, false);
    renderChips('#unreachable-list', state.unreachableRouters, true);
    renderEvents(state.events);
  }

  function toggleRouter(router) {
    const route = router.online ? `/routers/${encodeURIComponent(router.id)}/fail` : `/routers/${encodeURIComponent(router.id)}/recover`;
    postAction(route);
  }

  document.querySelectorAll('[data-topology]').forEach((button) => {
    button.addEventListener('click', () => postAction('/topology', { topology: button.dataset.topology }));
  });
  $('#topology-select').addEventListener('change', (event) => {
    postAction('/topology', { topology: event.target.value });
  });
  $('#router-count').addEventListener('change', (event) => {
    postAction('/router-count', { count: Number(event.target.value) });
  });
  $('#fail-all').addEventListener('click', () => postAction('/routers/fail-all'));
  $('#recover-all').addEventListener('click', () => postAction('/routers/recover-all'));
  $('#reset-network').addEventListener('click', () => postAction('/reset'));
  $('#retry-button').addEventListener('click', () => readState({ initial: true }));

  function updateClock() {
    $('#clock').textContent = `${new Date().toISOString().slice(11, 19)} UTC`;
  }
  updateClock();
  window.setInterval(updateClock, 1000);
  readState({ initial: true });
  window.setInterval(() => readState({ quiet: true }), POLL_INTERVAL_MS);
  let resizeTimer = 0;
  window.addEventListener('resize', () => {
    window.clearTimeout(resizeTimer);
    resizeTimer = window.setTimeout(() => {
      if (currentState) renderTopology(currentState);
    }, 100);
  });
})();
