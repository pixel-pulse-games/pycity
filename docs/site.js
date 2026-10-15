/* PyCity site — theme toggle and latest-release lookup. */
(function () {
  var root = document.documentElement;

  // ---- theme toggle: follows the OS until the visitor picks one ----
  var btn = document.getElementById('theme-toggle');
  function current() {
    var t = root.getAttribute('data-theme');
    if (t) return t;
    return window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  }
  function label() { if (btn) btn.textContent = current() === 'dark' ? 'Light theme' : 'Dark theme'; }
  if (btn) {
    label();
    btn.addEventListener('click', function () {
      var next = current() === 'dark' ? 'light' : 'dark';
      root.setAttribute('data-theme', next);
      try { localStorage.setItem('site-theme', next); } catch (e) {}
      label();
    });
  }

  // ---- latest release (download page + version strings) ----
  var box = document.getElementById('latest-release-info');
  var versionEls = document.querySelectorAll('[data-latest-version]');
  if (!box && !versionEls.length) return;

  function esc(s) {
    return String(s).replace(/[&<>"']/g, function (ch) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[ch];
    });
  }
  // Minimal, safe markdown: headings, bullets, bold, paragraphs. Input is escaped first.
  function renderNotes(md) {
    var html = '', inList = false;
    esc(md).split(/\r?\n/).forEach(function (line) {
      var h = line.match(/^(#{1,3})\s+(.*)$/), li = line.match(/^\s*[-*]\s+(.*)$/);
      var inline = function (s) { return s.replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>').replace(/`(.+?)`/g, '<code>$1</code>'); };
      if (li) { if (!inList) { html += '<ul>'; inList = true; } html += '<li>' + inline(li[1]) + '</li>'; return; }
      if (inList) { html += '</ul>'; inList = false; }
      if (h) html += '<h4>' + inline(h[2]) + '</h4>';
      else if (line.trim()) html += '<p>' + inline(line) + '</p>';
    });
    if (inList) html += '</ul>';
    return html;
  }

  fetch('https://api.github.com/repos/pixel-pulse-games/pycity/releases/latest')
    .then(function (res) {
      if (res.status === 403) throw new Error('rate');
      if (!res.ok) throw new Error('fail');
      return res.json();
    })
    .then(function (data) {
      var tag = data.tag_name || data.name;
      versionEls.forEach(function (el) { el.textContent = tag; });
      if (box) {
        box.innerHTML = '<div class="release"><h3>Latest version: ' + esc(data.name || tag) + '</h3>' +
          renderNotes(data.body || 'No release notes were provided.') + '</div>';
      }
    })
    .catch(function (err) {
      if (!box) return;
      box.innerHTML = '<p>' + (err.message === 'rate'
        ? 'GitHub is limiting requests right now.'
        : 'The release notes could not be loaded.') +
        ' Read them on <a href="https://github.com/pixel-pulse-games/pycity/releases">GitHub Releases</a>.</p>';
    });
})();
