// POM2 — GPL-3.0-or-later. Linked with --pre-js.
//
// Two-finger pinch zoom in the browser (src/PinchZoomHost.h, policy in
// src/PinchZoom.h). Three shapes reach a page:
//
//   • a trackpad pinch in Chrome / Firefox / Edge is a `wheel` with ctrlKey.
//     main.cpp's wheel callback turns that into a zoom; this file only makes
//     sure the browser's own page zoom never runs on top of it (a passive
//     listener could not cancel it);
//   • a trackpad pinch in Safari is gesturestart / gesturechange /
//     gestureend, with a cumulative `scale`;
//   • a tablet or phone gives two touches.
//
// The last two call `pom2_pinch_zoom(ratio, x, y, hasFocal)`: ratio = new
// scale / old (> 1 = fingers apart), (x, y) in GLFW's cursor units — the
// canvas backbuffer, as Emscripten's Browser.calculateMouseCoords computes
// them. During a two-finger pinch the touch moves are kept from Emscripten's
// GLFW, which would otherwise read the first finger as a mouse drag (a pan,
// a brush stroke), and the button GLFW pressed for the first finger is
// released off every widget as the second lands (pom2_pinch_touch_begin), so
// the pinch never ends as a click. GLFW's own touchend still goes through,
// to keep its button state in step.
(function() {
  'use strict';
  if (typeof window === 'undefined') return;

  function canvas() { return Module.canvas || document.getElementById('canvas'); }
  function onCanvas(e) { const c = canvas(); return !!c && e.target === c; }
  function toBackbuffer(clientX, clientY) {
    const c = canvas();
    const r = c.getBoundingClientRect();
    if (!(r.width > 0) || !(r.height > 0)) return { x: 0, y: 0 };
    return { x: (clientX - r.left) * (c.width / r.width),
             y: (clientY - r.top) * (c.height / r.height) };
  }
  function send(ratio, x, y, hasFocal) {
    if (!(ratio > 0) || !isFinite(ratio) || ratio === 1) return;
    try {
      Module.ccall('pom2_pinch_zoom', null, ['number', 'number', 'number', 'number'],
                   [ratio, x, y, hasFocal ? 1 : 0]);
    } catch (err) { /* the runtime is not up yet: the gesture is simply lost */ }
  }

  // ── Ctrl+wheel: no page zoom over the canvas (main.cpp does the zoom) ──
  window.addEventListener('wheel', function(e) {
    if (e.ctrlKey && onCanvas(e)) e.preventDefault();
  }, { capture: true, passive: false });

  // ── Two touches ────────────────────────────────────────────────────────
  let touchDist = 0;   // finger distance at the last move, 0 = no pinch
  function pair(e) {
    const t = e.targetTouches;
    if (!t || t.length < 2) return null;
    const a = t[0], b = t[1];
    const dx = b.clientX - a.clientX, dy = b.clientY - a.clientY;
    return { dist: Math.hypot(dx, dy),
             cx: (a.clientX + b.clientX) / 2, cy: (a.clientY + b.clientY) / 2 };
  }
  function touchStart(e) {
    if (!onCanvas(e)) return;
    const p = pair(e);
    if (!p) return;
    e.preventDefault();
    if (touchDist <= 0) {
      // The pinch begins. Emscripten's GLFW already pressed the left button
      // for the first finger: have POM2 release it off every widget, so
      // lifting the fingers clicks nothing (Router::cancelPress).
      try { Module.ccall('pom2_pinch_touch_begin', null, [], []); } catch (err) {}
    }
    touchDist = p.dist;
  }
  function touchMove(e) {
    if (!onCanvas(e) || touchDist <= 0) return;
    const p = pair(e);
    if (!p) return;
    e.preventDefault();
    e.stopPropagation();          // not a mouse drag for Emscripten's GLFW
    if (p.dist > 0) {
      const f = toBackbuffer(p.cx, p.cy);
      send(p.dist / touchDist, f.x, f.y, true);
      touchDist = p.dist;
    }
  }
  function touchEnd(e) {
    if (!onCanvas(e)) return;
    if (!pair(e)) touchDist = 0;  // let the event through: GLFW releases
  }
  window.addEventListener('touchstart', touchStart, { capture: true, passive: false });
  window.addEventListener('touchmove', touchMove, { capture: true, passive: false });
  window.addEventListener('touchend', touchEnd, { capture: true });
  window.addEventListener('touchcancel', touchEnd, { capture: true });

  // ── Safari's gesture events (macOS trackpad; iOS sends them too) ───────
  // On iOS the same two fingers also produce touches, handled above, so a
  // gesture is only forwarded while no touch pinch is running.
  let gestureScale = 1;
  function gesture(e) {
    if (!onCanvas(e)) return;
    e.preventDefault();           // no page zoom
    if (e.type === 'gesturestart') { gestureScale = 1; return; }
    if (e.type === 'gestureend' || touchDist > 0) return;
    const s = Number(e.scale);
    if (!(s > 0)) return;
    if (typeof e.clientX === 'number' && typeof e.clientY === 'number') {
      const f = toBackbuffer(e.clientX, e.clientY);
      send(s / gestureScale, f.x, f.y, true);
    } else {
      send(s / gestureScale, 0, 0, false);
    }
    gestureScale = s;
  }
  window.addEventListener('gesturestart', gesture, { capture: true, passive: false });
  window.addEventListener('gesturechange', gesture, { capture: true, passive: false });
  window.addEventListener('gestureend', gesture, { capture: true, passive: false });
})();
