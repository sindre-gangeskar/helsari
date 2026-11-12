// The launcher window: shows what the Rust side reports (games, what the build needs, build
// progress) and turns the player's clicks into its commands.
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const el = (id) => document.getElementById(id);
// art: per game, the menu background read from the player's own dump ({ dump, background }).
const state = { overview: null, selected: null, build: null, art: {} };

// Loads a game's artwork from its dump once (again when the dump folder changes).
async function loadArt(g) {
  if (!g.dump || state.art[g.id]?.dump === g.dump) return;
  const art = await invoke('art', { game: g.id }).catch(() => null);
  state.art[g.id] = { dump: g.dump, ...(art || {}) };
  renderCards();
  renderBar();
}

function titleOf(g) {
  return g.title;   // the launcher's names; the disc's own title has trademark signs and no space
}

const STATUS_TEXT = {
  built: 'Ready',
  ready: 'Not built',
  missing: 'Game not found',
  unsupported: 'Coming later',
};

function selectedGame() {
  return state.overview?.games.find((g) => g.id === state.selected) ?? null;
}

function missingPrereqs() {
  return (state.overview?.prereqs ?? []).filter((p) => !p.ok);
}

function renderPrereqs() {
  const box = el('prereqs');
  box.replaceChildren();
  for (const p of state.overview.prereqs) {
    const chip = document.createElement('div');
    chip.className = 'chip' + (p.ok ? '' : ' missing');
    chip.title = p.detail;
    chip.innerHTML = '<span class="dot"></span>';
    chip.append(p.name);
    if (!p.ok) {
      const fix = document.createElement('span');
      fix.className = 'fix';
      fix.textContent = p.action === 'pick-rpcs3' ? '· choose' : '· get it';
      chip.append(fix);
      chip.addEventListener('click', () => fixPrereq(p));
    }
    box.append(chip);
  }
}

function renderCards() {
  const box = el('cards');
  box.replaceChildren();
  for (const g of state.overview.games) {
    const card = document.createElement('article');
    card.className = 'card' + (g.id === state.selected ? ' selected' : '') + (g.supported ? '' : ' unsupported');
    card.dataset.game = g.id;
    card.tabIndex = 0;
    const numeral = g.title.replace(/\D/g, '');
    const badgeText = state.build?.game === g.id ? 'Building' : STATUS_TEXT[g.status];
    const art = state.art[g.id];
    card.innerHTML = `
      <div class="art"><span class="numeral">${numeral}</span><span class="band"></span></div>
      <div class="card-body">
        <h2 class="card-title"></h2>
        <div class="card-meta">
          <span class="badge ${g.status}">${badgeText}</span>
          <span>${g.title_id ? 'Disc ' + g.title_id : ''}</span>
        </div>
      </div>`;
    card.querySelector('.card-title').textContent = titleOf(g);
    if (art?.background) {
      card.classList.add('has-art');
      card.querySelector('.art').style.backgroundImage = `url("${art.background}")`;
    }
    card.addEventListener('click', () => select(g.id));
    card.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') select(g.id); });
    box.append(card);
  }
}

function renderBar() {
  const g = selectedGame();
  const primary = el('primary');
  const secondary = el('secondary');
  secondary.replaceChildren();
  primary.className = 'play';
  if (!g) return;

  const building = state.build && state.build.game === g.id;
  const otherBuilding = state.build && state.build.game !== g.id;
  el('status-title').textContent = titleOf(g);
  el('status-detail').textContent = building ? '' : (g.dump && g.status !== 'missing' ? g.dump : g.detail);

  el('progress').hidden = !building;
  if (building) {
    const pct = Math.max(0, Math.min(100, state.build.percent));
    el('progress-fill').style.width = pct.toFixed(1) + '%';
    el('progress-stage').textContent = state.build.stage;
    el('progress-percent').textContent = Math.floor(pct) + '%';
    el('progress-line').textContent = state.build.line || '';
  }

  const blockers = missingPrereqs();
  primary.disabled = false;
  if (!g.supported) {
    primary.textContent = 'Coming later';
    primary.disabled = true;
  } else if (building) {
    primary.textContent = 'Building…';
    primary.disabled = true;
  } else if (g.status === 'missing') {
    primary.textContent = 'Find game';
    primary.onclick = () => pickDump(g.id);
  } else if (g.status === 'ready') {
    primary.textContent = 'Build';
    primary.disabled = blockers.length > 0 || otherBuilding;
    primary.onclick = () => startBuild(g.id);
    if (blockers.length) el('status-detail').textContent = 'Needs: ' + blockers.map((p) => p.name).join(', ');
  } else {
    primary.textContent = 'Play';
    primary.onclick = () => play(g.id);
  }

  if (g.supported && !building) {
    if (g.dump) addLink(secondary, 'Change game folder', () => pickDump(g.id));
    if (g.status === 'built' && !otherBuilding && blockers.length === 0) addLink(secondary, 'Rebuild', () => startBuild(g.id));
  }
}

function addLink(box, text, onClick) {
  const b = document.createElement('button');
  b.type = 'button';
  b.className = 'link';
  b.textContent = text;
  b.addEventListener('click', onClick);
  box.append(b);
}

function render() {
  if (!state.overview) return;
  renderPrereqs();
  renderCards();
  renderBar();
}

function select(id) {
  state.selected = id;
  render();
}

function showFailure(title, lines) {
  el('failure-title').textContent = title;
  el('failure-log').textContent = (lines || []).join('\n');
  el('failure').hidden = false;
}

async function refresh() {
  state.overview = await invoke('overview');
  if (!state.selected) {
    state.selected = (state.overview.games.find((g) => g.supported) ?? state.overview.games[0]).id;
  }
  render();
  for (const g of state.overview.games) loadArt(g);
}

async function pickDump(id) {
  try {
    state.overview = await invoke('pick_dump', { game: id });
    render();
    for (const g of state.overview.games) loadArt(g);
  } catch (e) {
    showFailure('That folder is not a usable dump', [String(e)]);
  }
}

async function fixPrereq(p) {
  try {
    if (p.action === 'pick-rpcs3') {
      state.overview = await invoke('pick_rpcs3');
      render();
    } else if (p.action) {
      await invoke('open_link', { url: p.action });
    }
  } catch (e) {
    showFailure(p.name, [String(e)]);
  }
}

async function startBuild(id) {
  el('failure').hidden = true;
  try {
    state.build = { game: id, stage: 'Starting', percent: 0, line: '' };
    render();
    await invoke('build', { game: id });
  } catch (e) {
    state.build = null;
    render();
    showFailure('The build could not start', [String(e)]);
  }
}

async function play(id) {
  try {
    await invoke('play', { game: id });
  } catch (e) {
    showFailure('The game could not start', [String(e)]);
  }
}

listen('build-progress', (e) => {
  const started = !state.build || state.build.game !== e.payload.game;
  state.build = e.payload;
  if (started) renderCards();   // the card's badge says "Building"
  renderBar();
});

listen('build-finished', async (e) => {
  state.build = null;
  if (!e.payload.ok) showFailure(e.payload.message, e.payload.log);
  await refresh();
});

el('failure-close').addEventListener('click', () => { el('failure').hidden = true; });

(async () => {
  const running = await invoke('building');
  if (running) state.build = { game: running, stage: 'Building', percent: 0, line: '' };
  await refresh();
})();
