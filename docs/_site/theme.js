(function() {
  // 1. Define and inject CSS rules into the page head
  const css = `
    :root {
      --bg-color: #ffffff;
      --text-color: #24292f;
      --btn-bg: #f6f8fa;
      --border-color: #d0d7de;
    }
    body.dark-theme {
      --bg-color: #0d1117;
      --text-color: #c9d1d9;
      --btn-bg: #21262d;
      --border-color: #30363d;
    }
    body {
      background-color: var(--bg-color) !important;
      color: var(--text-color) !important;
      transition: background-color 0.15s ease, color 0.15s ease;
    }
    
    /* MANDATORY FIXES FOR GENERATED MARKDOWN TABLES */
    .theme-table-container table {
      background-color: var(--bg-color) !important;
      color: var(--text-color) !important;
      border-collapse: collapse !important;
      border: 1px solid var(--border-color) !important;
      width: 100%;
      margin: 20px 0;
    }
    .theme-table-container th, 
    .theme-table-container td {
      color: var(--text-color) !important;
      background-color: transparent !important;
      border: 1px solid var(--border-color) !important;
      padding: 8px 12px;
      text-align: left;
    }
    .theme-table-container th {
      background-color: var(--btn-bg) !important;
      font-weight: 600;
    }

    .theme-btn {
      background-color: var(--btn-bg);
      color: var(--text-color);
      border: 1px solid rgba(27, 31, 36, 0.15);
      padding: 6px 12px;
      font-size: 13px;
      font-weight: 500;
      border-radius: 6px;
      cursor: pointer;
      margin-bottom: 15px;
    }
    body.dark-theme .theme-btn {
      border-color: rgba(240, 246, 252, 0.1);
    }
    body.dark-theme .icon-dark {
      display: none;
    }
    body:not(.dark-theme) .icon-light {
      display: none;
    }
  `;

  const styleTag = document.createElement('style');
  styleTag.appendChild(document.createTextNode(css));
  document.head.appendChild(styleTag);

  // 2. Check local storage and apply dark mode state instantly
  const savedTheme = localStorage.getItem('site-theme') || (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
  if (savedTheme === 'dark') {
    document.body.classList.add('dark-theme');
  }

  // 3. Automatically inject the button HTML exactly where the script tag sits
  const currentScript = document.currentScript;
  const btn = document.createElement('button');
  btn.id = 'theme-toggle';
  btn.className = 'theme-btn';
  btn.setAttribute('aria-label', 'Toggle dark mode');
  btn.innerHTML = `
    <span class="icon-light">☀️ Light Mode</span>
    <span class="icon-dark">🌙 Dark Mode</span>
  `;

  // Inserts the button right into the document flow
  currentScript.parentNode.insertBefore(btn, currentScript);

  // 4. Handle button clicks
  btn.addEventListener('click', () => {
    const isDark = document.body.classList.toggle('dark-theme');
    localStorage.setItem('site-theme', isDark ? 'dark' : 'light');
  });
})();
