/* Hero city: a tiny live version of the game, drawn from the game's own tiles.
   Farms send grain/timber to factories, factories send goods to houses, trucks
   follow roads (BFS), and clicking a tile lays or removes a road. */
(function () {
  var canvas = document.getElementById('city');
  if (!canvas || !canvas.getContext) return;

  var T = 16;
  var MAP = [
    '..............................',
    '.A..H..F..H..A..F..H..H..A.F..',
    '##############################',
    '.H..A..F.#..H..H.#.A....F..H..',
    '.........#.......#............',
    '.........#.......#............',
    '##############################',
    '.F..H..A..H..F..H....A..H..F..',
    '..............................'
  ];
  var ROWS = MAP.length, COLS = MAP[0].length;
  canvas.width = COLS * T;
  canvas.height = ROWS * T;
  var ctx = canvas.getContext('2d');
  ctx.imageSmoothingEnabled = false;

  var reduceMotion = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  var CHAIN_COLORS = ['#dec15a', '#50c8be'];       // grain, timber (same as the game)
  var TRUCK_FARM = '#6ebe5a', TRUCK_FACTORY = '#ff6b35'; // same as the game

  var names = ['grass-a', 'grass-b', 'grass-c', 'road', 'house', 'factory', 'farm'];
  var img = {}, loaded = 0;
  names.forEach(function (n) {
    var im = new Image();
    im.onload = function () { if (++loaded === names.length) start(); };
    im.src = 'img/' + n + '.png';
    img[n] = im;
  });

  var grid, grass, buildings, trucks, deliveries, hover = null, lastSpawn = 0, running = false, visible = true;
  var deliveriesEl = document.getElementById('city-deliveries');

  function reset() {
    grid = MAP.map(function (row) { return row.split(''); });
    // deterministic grass variety so it doesn't change on every reset
    grass = [];
    for (var r = 0; r < ROWS; r++) {
      grass[r] = [];
      for (var c = 0; c < COLS; c++) {
        var h = (r * 73 + c * 151 + 7) % 11;
        grass[r][c] = h === 0 ? 'grass-c' : (h < 4 ? 'grass-b' : 'grass-a');
      }
    }
    buildings = [];
    for (var rr = 0; rr < ROWS; rr++) for (var cc = 0; cc < COLS; cc++) {
      var ch = grid[rr][cc];
      if (ch === 'A' || ch === 'F' || ch === 'H') buildings.push({ r: rr, c: cc, kind: ch, chain: (rr + cc) % 2 });
    }
    trucks = [];
    deliveries = 0;
    updateCount();
  }

  function isRoad(r, c) { return r >= 0 && r < ROWS && c >= 0 && c < COLS && grid[r][c] === '#'; }
  var DIRS = [[1, 0], [-1, 0], [0, 1], [0, -1]];

  function roadNeighbors(b) {
    var out = [];
    DIRS.forEach(function (d) { if (isRoad(b.r + d[0], b.c + d[1])) out.push([b.r + d[0], b.c + d[1]]); });
    return out;
  }

  // Multi-source BFS from the roads next to `from` to any road next to `to`.
  function findPath(from, to) {
    var starts = roadNeighbors(from), goals = {};
    roadNeighbors(to).forEach(function (g) { goals[g[0] + ',' + g[1]] = true; });
    if (!starts.length || !Object.keys(goals).length) return null;
    var prev = {}, queue = [];
    starts.forEach(function (s) { prev[s[0] + ',' + s[1]] = null; queue.push(s); });
    for (var i = 0; i < queue.length; i++) {
      var cur = queue[i], key = cur[0] + ',' + cur[1];
      if (goals[key]) {
        var path = [], k = key;
        while (k !== null) { var p = k.split(','); path.unshift({ r: +p[0], c: +p[1] }); k = prev[k]; }
        return path;
      }
      for (var d = 0; d < 4; d++) {
        var nr = cur[0] + DIRS[d][0], nc = cur[1] + DIRS[d][1], nk = nr + ',' + nc;
        if (isRoad(nr, nc) && !(nk in prev)) { prev[nk] = key; queue.push([nr, nc]); }
      }
    }
    return null;
  }

  function pick(arr) { return arr[Math.floor(Math.random() * arr.length)]; }

  function spawnTruck() {
    if (trucks.length >= 6) return;
    var leg = Math.random() < 0.5 ? { from: 'A', to: 'F', color: TRUCK_FARM } : { from: 'F', to: 'H', color: TRUCK_FACTORY };
    var src = pick(buildings.filter(function (b) { return b.kind === leg.from; }));
    var dsts = buildings.filter(function (b) { return b.kind === leg.to && b.chain === src.chain; });
    if (!dsts.length) return;
    var path = findPath(src, pick(dsts));
    if (path) trucks.push({ path: path, pos: 0, color: leg.color });
  }

  function step(dt) {
    var speed = 5; // tiles per second
    for (var i = trucks.length - 1; i >= 0; i--) {
      var t = trucks[i];
      t.pos += speed * dt;
      if (t.pos >= t.path.length - 1) { trucks.splice(i, 1); deliveries++; updateCount(); }
    }
  }

  function updateCount() { if (deliveriesEl) deliveriesEl.textContent = deliveries; }

  function draw() {
    for (var r = 0; r < ROWS; r++) for (var c = 0; c < COLS; c++) {
      var ch = grid[r][c], x = c * T, y = r * T;
      ctx.drawImage(img[grass[r][c]], x, y);
      if (ch === '#') ctx.drawImage(img.road, x, y);
      else if (ch === 'H') ctx.drawImage(img.house, x, y);
      else if (ch === 'F') ctx.drawImage(img.factory, x, y);
      else if (ch === 'A') ctx.drawImage(img.farm, x, y);
    }
    buildings.forEach(function (b) {
      ctx.fillStyle = CHAIN_COLORS[b.chain];
      ctx.fillRect(b.c * T + T - 5, b.r * T + 2, 3, 3);
    });
    trucks.forEach(function (t) {
      var i = Math.min(Math.floor(t.pos), t.path.length - 2);
      if (i < 0) return;
      var a = t.path[i], b = t.path[i + 1], f = t.pos - i;
      var x = (a.c + (b.c - a.c) * f) * T + T / 2, y = (a.r + (b.r - a.r) * f) * T + T / 2;
      ctx.fillStyle = '#1f2e25';
      ctx.fillRect(Math.round(x) - 4, Math.round(y) - 3, 8, 6);
      ctx.fillStyle = t.color;
      ctx.fillRect(Math.round(x) - 3, Math.round(y) - 2, 6, 4);
    });
    if (hover) {
      ctx.strokeStyle = '#f2c14e';
      ctx.lineWidth = 1;
      ctx.strokeRect(hover.c * T + 0.5, hover.r * T + 0.5, T - 1, T - 1);
    }
  }

  var last = 0;
  function frame(now) {
    if (!running) return;
    var dt = Math.min((now - last) / 1000, 0.1);
    last = now;
    if (now - lastSpawn > 700) { spawnTruck(); lastSpawn = now; }
    step(dt);
    draw();
    requestAnimationFrame(frame);
  }
  function play() {
    if (reduceMotion || running || !visible) return;
    running = true; last = performance.now(); requestAnimationFrame(frame);
  }
  function stop() { running = false; }

  function tileAt(e) {
    var rect = canvas.getBoundingClientRect();
    var c = Math.floor((e.clientX - rect.left) / rect.width * COLS);
    var r = Math.floor((e.clientY - rect.top) / rect.height * ROWS);
    return (r >= 0 && r < ROWS && c >= 0 && c < COLS) ? { r: r, c: c } : null;
  }

  canvas.addEventListener('pointermove', function (e) { hover = tileAt(e); if (!running) draw(); });
  canvas.addEventListener('pointerleave', function () { hover = null; if (!running) draw(); });
  canvas.addEventListener('click', function (e) {
    var t = tileAt(e);
    if (!t) return;
    var ch = grid[t.r][t.c];
    if (ch === '.') grid[t.r][t.c] = '#';
    else if (ch === '#') {
      grid[t.r][t.c] = '.';
      // trucks whose route used this tile can no longer finish it
      trucks = trucks.filter(function (tr) {
        return !tr.path.some(function (p) { return p.r === t.r && p.c === t.c; });
      });
    }
    if (!running) draw();
  });

  var resetBtn = document.getElementById('city-reset');
  if (resetBtn) resetBtn.addEventListener('click', function () { reset(); if (!running) draw(); });

  if ('IntersectionObserver' in window) {
    new IntersectionObserver(function (entries) {
      visible = entries[0].isIntersecting;
      if (visible) play(); else stop();
    }).observe(canvas);
  }

  function start() { reset(); draw(); play(); }
})();
