// prelude.js --- the Web APIs that need no document, on libjsr2's natives.
//
// Compiled to bytecode at build time (tools/jsr2c) and run once per engine,
// before any page script.  `n` is the __jsr2 object of engine.cpp:
//   log, reportError, now, setTimer, clearTimer, requestFrame, cancelFrame,
//   open, close, utf8Encode, utf8Decode, atob, btoa, randomFill, baseUrl.
// Embedders build on what is defined here (r2web's DOM: dom.js) through
// globalThis.__jsr2lib, which this file leaves behind.
(function (n) {
'use strict';
const g = globalThis;
const hide = (o, name, value) =>
  Object.defineProperty(o, name, { value, writable: true, configurable: true, enumerable: false });
const install = (name, value) => hide(g, name, value);
const brand = (C, tag) => Object.defineProperty(C.prototype, Symbol.toStringTag, { value: tag, configurable: true });

// ── console ───────────────────────────────────────────────────────────────

function inspect(v, depth, seen) {
  if (typeof v === 'string') return depth ? JSON.stringify(v) : v;
  if (typeof v === 'function') return v.name ? `[Function: ${v.name}]` : '[Function (anonymous)]';
  if (typeof v === 'symbol') return v.toString();
  if (typeof v === 'bigint') return `${v}n`;
  if (v === null || typeof v !== 'object') return String(v);
  if (seen.includes(v)) return '[Circular]';
  if (v instanceof Error) return v.stack ? `${v.name}: ${v.message}\n${v.stack}`.trimEnd() : `${v.name}: ${v.message}`;
  if (v instanceof Date) return isNaN(v) ? 'Invalid Date' : v.toISOString();
  if (v instanceof RegExp) return String(v);
  if (typeof v.nodeType === 'number' && typeof v.nodeName === 'string') {
    if (v.nodeType === 1) return `<${v.nodeName.toLowerCase()}${v.id ? '#' + v.id : ''}>`;
    return `#${v.nodeName.replace(/^#/, '')}`;
  }
  if (depth > 2) return Array.isArray(v) ? '[Array]' : '[Object]';
  seen = seen.concat([v]);
  const each = (items, open, close) => {
    const parts = items.slice(0, 50);
    if (items.length > 50) parts.push(`... ${items.length - 50} more`);
    return parts.length ? `${open} ${parts.join(', ')} ${close}` : `${open}${close}`;
  };
  if (Array.isArray(v)) return each(v.map(x => inspect(x, depth + 1, seen)), '[', ']');
  if (v instanceof Map) return `Map(${v.size}) ` + each([...v].map(([k, x]) => `${inspect(k, depth + 1, seen)} => ${inspect(x, depth + 1, seen)}`), '{', '}');
  if (v instanceof Set) return `Set(${v.size}) ` + each([...v].map(x => inspect(x, depth + 1, seen)), '{', '}');
  if (ArrayBuffer.isView(v) && !(v instanceof DataView)) return `${v.constructor.name}(${v.length}) ` + each(Array.from(v).map(String), '[', ']');
  const name = v.constructor && v.constructor !== Object && v.constructor.name ? v.constructor.name + ' ' : '';
  let keys;
  try { keys = Object.keys(v); } catch (e) { keys = []; }
  return name + each(keys.map(k => `${/^[A-Za-z_$][\w$]*$/.test(k) ? k : JSON.stringify(k)}: ${inspect(v[k], depth + 1, seen)}`), '{', '}');
}

function format(args) {
  let out = '', i = 0;
  if (typeof args[0] === 'string' && args.length > 1 && args[0].includes('%')) {
    i = 1;
    out = args[0].replace(/%[sdifoOjc%]/g, m => {
      if (m === '%%') return '%';
      if (i >= args.length) return m;
      const a = args[i++];
      switch (m) {
        case '%s': return typeof a === 'string' ? a : inspect(a, 1, []);
        case '%d': case '%i': return String(typeof a === 'bigint' ? a : parseInt(a));
        case '%f': return String(Number(a));
        case '%c': return '';
        default: return inspect(a, 1, []);
      }
    });
  }
  for (; i < args.length; i++) out += (out ? ' ' : '') + inspect(args[i], 0, []);
  return out;
}

let groupIndent = '';
const counts = new Map(), timers = new Map();
const say = (level, args) => n.log(level, groupIndent + format(args));
const console = {
  log: (...a) => say(0, a), info: (...a) => say(1, a), warn: (...a) => say(2, a),
  error: (...a) => say(3, a), debug: (...a) => say(4, a), trace: (...a) => say(4, a),
  dir: (v) => say(0, [v]), dirxml: (...a) => say(0, a), table: (v) => say(0, [v]),
  assert: (cond, ...a) => { if (!cond) say(3, ['Assertion failed' + (a.length ? ':' : ''), ...a]); },
  group: (...a) => { if (a.length) say(0, a); groupIndent += '  '; },
  groupCollapsed: (...a) => { if (a.length) say(0, a); groupIndent += '  '; },
  groupEnd: () => { groupIndent = groupIndent.slice(2); },
  count: (label = 'default') => { const c = (counts.get(label) || 0) + 1; counts.set(label, c); say(0, [`${label}: ${c}`]); },
  countReset: (label = 'default') => counts.delete(label),
  time: (label = 'default') => timers.set(label, n.now()),
  timeLog: (label = 'default', ...a) => say(0, [`${label}: ${(n.now() - (timers.get(label) ?? n.now())).toFixed(1)}ms`, ...a]),
  timeEnd: (label = 'default') => { say(0, [`${label}: ${(n.now() - (timers.get(label) ?? n.now())).toFixed(1)}ms`]); timers.delete(label); },
  clear: () => {},
};
install('console', console);

function reportError(e) { n.reportError(e); }
install('reportError', reportError);

// ── timers and frames ─────────────────────────────────────────────────────

install('setTimeout', function setTimeout(fn, ms, ...args) { return n.setTimer(fn, +ms || 0, false, args.length ? args : undefined); });
install('setInterval', function setInterval(fn, ms, ...args) { return n.setTimer(fn, Math.max(+ms || 0, 1), true, args.length ? args : undefined); });
install('clearTimeout', function clearTimeout(id) { if (id != null) n.clearTimer(id | 0); });
install('clearInterval', function clearInterval(id) { if (id != null) n.clearTimer(id | 0); });
install('queueMicrotask', function queueMicrotask(fn) {
  if (typeof fn !== 'function') throw new TypeError("queueMicrotask: argument is not a function");
  Promise.resolve().then(() => { try { fn(); } catch (e) { reportError(e); } });
});
install('requestAnimationFrame', function requestAnimationFrame(fn) { return n.requestFrame(fn); });
install('cancelAnimationFrame', function cancelAnimationFrame(id) { n.cancelFrame(id | 0); });
install('requestIdleCallback', function requestIdleCallback(fn, opts) {
  return n.setTimer(() => fn({ didTimeout: false, timeRemaining: () => 10 }), Math.min(opts && opts.timeout || 1, 50), false);
});
install('cancelIdleCallback', function cancelIdleCallback(id) { n.clearTimer(id | 0); });

const timeOrigin = Date.now() - n.now();
const marks = [];
install('performance', {
  now: () => n.now(),
  timeOrigin,
  mark: (name) => { const m = { name, entryType: 'mark', startTime: n.now(), duration: 0 }; marks.push(m); return m; },
  measure: (name, start, end) => {
    const t = (x) => typeof x === 'string' ? (marks.findLast(m => m.name === x) || { startTime: 0 }).startTime : (x ?? n.now());
    const m = { name, entryType: 'measure', startTime: t(start ?? 0), duration: t(end) - t(start ?? 0) };
    marks.push(m); return m;
  },
  getEntriesByName: (name) => marks.filter(m => m.name === name),
  getEntriesByType: (type) => marks.filter(m => m.entryType === type),
  getEntries: () => marks.slice(),
  clearMarks: () => { for (let i = marks.length; i--;) if (marks[i].entryType === 'mark') marks.splice(i, 1); },
  clearMeasures: () => { for (let i = marks.length; i--;) if (marks[i].entryType === 'measure') marks.splice(i, 1); },
  toJSON() { return { timeOrigin }; },
});

// ── DOMException ──────────────────────────────────────────────────────────

const legacyCodes = {
  IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5,
  NoModificationAllowedError: 7, NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11,
  SyntaxError: 12, InvalidModificationError: 13, NamespaceError: 14, InvalidAccessError: 15,
  TypeMismatchError: 17, SecurityError: 18, NetworkError: 19, AbortError: 20, URLMismatchError: 21,
  QuotaExceededError: 22, TimeoutError: 23, InvalidNodeTypeError: 24, DataCloneError: 25,
};
class DOMException extends Error {
  constructor(message = '', name = 'Error') {
    super(message);
    hide(this, 'name', String(name));
  }
  get code() { return legacyCodes[this.name] || 0; }
}
for (const [k, v] of Object.entries(legacyCodes))
  Object.defineProperty(DOMException, k.replace(/Error$/, '').replace(/([a-z])([A-Z])/g, '$1_$2').toUpperCase() + '_ERR', { value: v });
install('DOMException', DOMException);

// ── events ────────────────────────────────────────────────────────────────

const kListeners = Symbol('listeners');
const kParent = Symbol.for('jsr2.parent');       // DOM: the next target up the path
const kActivation = Symbol.for('jsr2.activation'); // DOM: default action after dispatch
const kStop = Symbol('stop'), kStopNow = Symbol('stopNow'), kCanceled = Symbol('canceled');
const kInPassive = Symbol('passive'), kDispatching = Symbol('dispatching');

class Event {
  constructor(type, init) {
    if (arguments.length === 0) throw new TypeError("Failed to construct 'Event': 1 argument required.");
    init = init || {};
    this.type = String(type);
    this.bubbles = !!init.bubbles;
    this.cancelable = !!init.cancelable;
    this.composed = !!init.composed;
    this.target = null;
    this.currentTarget = null;
    this.eventPhase = 0;
    this.isTrusted = false;
    this.timeStamp = n.now();
    hide(this, kStop, false); hide(this, kStopNow, false); hide(this, kCanceled, false);
    hide(this, kInPassive, false); hide(this, kDispatching, false); hide(this, '_path', []);
  }
  get srcElement() { return this.target; }
  get defaultPrevented() { return this[kCanceled]; }
  get returnValue() { return !this[kCanceled]; }
  set returnValue(v) { if (!v) this.preventDefault(); }
  get cancelBubble() { return this[kStop]; }
  set cancelBubble(v) { if (v) this[kStop] = true; }
  stopPropagation() { this[kStop] = true; }
  stopImmediatePropagation() { this[kStop] = true; this[kStopNow] = true; }
  preventDefault() { if (this.cancelable && !this[kInPassive]) this[kCanceled] = true; }
  composedPath() { return this[kDispatching] ? this._path.slice() : []; }
  initEvent(type, bubbles, cancelable) {
    if (this[kDispatching]) return;
    this.type = String(type); this.bubbles = !!bubbles; this.cancelable = !!cancelable;
  }
}
Object.assign(Event, { NONE: 0, CAPTURING_PHASE: 1, AT_TARGET: 2, BUBBLING_PHASE: 3 });
Object.assign(Event.prototype, { NONE: 0, CAPTURING_PHASE: 1, AT_TARGET: 2, BUBBLING_PHASE: 3 });

const initFields = (ev, init, fields) => { for (const [k, d] of fields) ev[k] = init && init[k] !== undefined ? init[k] : d; };
class CustomEvent extends Event {
  constructor(type, init) { super(type, init); this.detail = init && init.detail !== undefined ? init.detail : null; }
  initCustomEvent(type, bubbles, cancelable, detail) { this.initEvent(type, bubbles, cancelable); this.detail = detail; }
}
class MessageEvent extends Event {
  constructor(type, init) { super(type, init); initFields(this, init, [['data', null], ['origin', ''], ['lastEventId', ''], ['source', null], ['ports', []]]); }
}
class ErrorEvent extends Event {
  constructor(type, init) { super(type, init); initFields(this, init, [['message', ''], ['filename', ''], ['lineno', 0], ['colno', 0], ['error', undefined]]); }
}
class ProgressEvent extends Event {
  constructor(type, init) { super(type, init); initFields(this, init, [['lengthComputable', false], ['loaded', 0], ['total', 0]]); }
}
class PromiseRejectionEvent extends Event {
  constructor(type, init) { super(type, init); initFields(this, init, [['promise', null], ['reason', undefined]]); }
}
class CloseEvent extends Event {
  constructor(type, init) { super(type, init); initFields(this, init, [['wasClean', false], ['code', 0], ['reason', '']]); }
}

function flatten(options) {
  if (typeof options === 'boolean') return { capture: options, once: false, passive: false, signal: null };
  options = options || {};
  return { capture: !!options.capture, once: !!options.once, passive: !!options.passive, signal: options.signal || null };
}

class EventTarget {
  constructor() { hide(this, kListeners, null); }
  addEventListener(type, callback, options) {
    if (callback == null) return;
    const o = flatten(options);
    if (o.signal && o.signal.aborted) return;
    let map = this[kListeners];
    if (!map) { map = new Map(); hide(this, kListeners, map); }
    type = String(type);
    let list = map.get(type);
    if (!list) map.set(type, list = []);
    if (list.some(l => l.callback === callback && l.capture === o.capture)) return;
    const entry = { callback, capture: o.capture, once: o.once, passive: o.passive, removed: false };
    list.push(entry);
    if (o.signal) o.signal.addEventListener('abort', () => this.removeEventListener(type, callback, o));
  }
  removeEventListener(type, callback, options) {
    const map = this[kListeners];
    if (!map) return;
    const list = map.get(String(type));
    if (!list) return;
    const capture = flatten(options).capture;
    const i = list.findIndex(l => l.callback === callback && l.capture === capture);
    if (i >= 0) { list[i].removed = true; list.splice(i, 1); }
  }
  dispatchEvent(event) {
    if (!(event instanceof Event)) throw new TypeError("Failed to execute 'dispatchEvent': parameter 1 is not of type 'Event'.");
    if (event[kDispatching]) throw new DOMException('The event is already being dispatched.', 'InvalidStateError');
    return dispatch(this, event);
  }
}

function invoke(target, event, phase) {
  const map = target[kListeners];
  if (!map) return;
  const list = map.get(event.type);
  event.currentTarget = target;
  if (list) {
    for (const l of list.slice()) {
      if (l.removed) continue;
      if (phase === 1 && !l.capture) continue;
      if (phase === 3 && l.capture) continue;
      if (l.once) target.removeEventListener(event.type, l.callback, { capture: l.capture });
      event[kInPassive] = l.passive;
      try {
        if (typeof l.callback === 'function') l.callback.call(target, event);
        else if (l.callback && typeof l.callback.handleEvent === 'function') l.callback.handleEvent(event);
      } catch (e) { reportError(e); }
      event[kInPassive] = false;
      if (event[kStopNow]) return;
    }
  }
}

// on<type> properties: a handler stored on the target, run in registration
// order relative to listeners would need a listener entry; it is run as a
// listener added when the property was first set, as browsers do.
const kHandlers = Symbol('handlers');
function eventHandler(proto, type) {
  Object.defineProperty(proto, 'on' + type, {
    configurable: true, enumerable: true,
    get() { const h = this[kHandlers]; return h && h[type] ? h[type].fn : null; },
    set(fn) {
      let h = this[kHandlers];
      if (!h) { h = Object.create(null); hide(this, kHandlers, h); }
      if (typeof fn !== 'function' && (typeof fn !== 'object' || fn === null)) fn = null;
      if (h[type]) { h[type].fn = fn; return; }
      if (fn === null) return;
      const slot = h[type] = { fn };
      this.addEventListener(type, function (event) {
        const f = slot.fn;
        if (typeof f !== 'function') return;
        const r = f.call(this, event);
        if (type === 'error' && r === true) event.preventDefault();
        else if (r === false && type !== 'error') event.preventDefault();
      });
    },
  });
}

function dispatch(target, event) {
  event[kDispatching] = true;
  event.target = target;
  event[kStop] = event[kStopNow] = false;
  const path = [target];
  for (let t = target[kParent] ? target[kParent](event) : null; t; t = t[kParent] ? t[kParent](event) : null) path.push(t);
  event._path = path;
  for (let i = path.length - 1; i > 0 && !event[kStop]; i--) { event.eventPhase = 1; invoke(path[i], event, 1); }
  if (!event[kStop]) { event.eventPhase = 2; invoke(target, event, 2); }
  if (event.bubbles) for (let i = 1; i < path.length && !event[kStop]; i++) { event.eventPhase = 3; invoke(path[i], event, 3); }
  event.eventPhase = 0;
  event.currentTarget = null;
  event[kDispatching] = false;
  if (!event[kCanceled] && target[kActivation]) target[kActivation](event);
  return !event[kCanceled];
}

install('Event', Event); install('CustomEvent', CustomEvent); install('MessageEvent', MessageEvent);
install('ErrorEvent', ErrorEvent); install('ProgressEvent', ProgressEvent);
install('PromiseRejectionEvent', PromiseRejectionEvent); install('CloseEvent', CloseEvent);
install('EventTarget', EventTarget);

// ── AbortController ───────────────────────────────────────────────────────

class AbortSignal extends EventTarget {
  constructor(key) {
    if (key !== kListeners) throw new TypeError('Illegal constructor');
    super(); this.aborted = false; this.reason = undefined;
  }
  throwIfAborted() { if (this.aborted) throw this.reason; }
  static abort(reason) { const c = new AbortController(); c.abort(reason); return c.signal; }
  static timeout(ms) {
    const c = new AbortController();
    setTimeout(() => c.abort(new DOMException('The operation timed out.', 'TimeoutError')), ms);
    return c.signal;
  }
  static any(signals) {
    const c = new AbortController();
    for (const s of signals) {
      if (s.aborted) { c.abort(s.reason); break; }
      s.addEventListener('abort', () => c.abort(s.reason), { once: true });
    }
    return c.signal;
  }
}
eventHandler(AbortSignal.prototype, 'abort');
class AbortController {
  constructor() { hide(this, '_signal', new AbortSignal(kListeners)); }
  get signal() { return this._signal; }
  abort(reason) {
    const s = this._signal;
    if (s.aborted) return;
    s.aborted = true;
    s.reason = reason !== undefined ? reason : new DOMException('This operation was aborted', 'AbortError');
    s.dispatchEvent(new Event('abort'));
  }
}
install('AbortSignal', AbortSignal); install('AbortController', AbortController);

// ── URL ───────────────────────────────────────────────────────────────────

const special = { 'ftp:': 21, 'file:': null, 'http:': 80, 'https:': 443, 'ws:': 80, 'wss:': 443 };
const hex = '0123456789ABCDEF';
function pct(s, keep) {
  let out = '';
  for (const ch of s) {
    const c = ch.codePointAt(0);
    if (c > 0x20 && c < 0x7f && !keep.includes(ch)) { out += ch; continue; }
    if (ch === '%') { out += ch; continue; }
    for (const b of new Uint8Array(n.utf8Encode(ch))) out += '%' + hex[b >> 4] + hex[b & 15];
  }
  return out;
}
const PATH_SET = ' "#<>?`{}', QUERY_SET = ' "#<>', SPECIAL_QUERY_SET = ' "#<>\'', FRAG_SET = ' "<>`', USER_SET = ' "#<>?`{}/:;=@[\\]^|';

function parseHost(s, isSpecial) {
  if (s.startsWith('[')) {
    if (!s.endsWith(']')) return null;
    return s.toLowerCase();
  }
  if (!isSpecial) return pct(s, '');
  let h;
  try { h = decodeURIComponent(s); } catch (e) { h = s; }
  h = h.toLowerCase();
  if (/[\x00-\x20#%/:<>?@[\\\]^|]/.test(h)) return null;
  // IPv4 with fewer parts or hex/octal parts, normalised.
  const parts = h.split('.');
  if (parts[parts.length - 1] === '') parts.pop();
  if (parts.length && parts.length <= 4 && /^(0x[0-9a-f]*|[0-9]+)$/.test(parts[parts.length - 1])) {
    const nums = parts.map(p => /^0x/.test(p) ? parseInt(p.slice(2) || '0', 16) : /^0[0-7]+$/.test(p) ? parseInt(p, 8) : /^[0-9]+$/.test(p) ? parseInt(p, 10) : NaN);
    if (nums.some(isNaN)) return null;
    let v = nums.pop();
    if (nums.some(x => x > 255) || v >= 256 ** (4 - nums.length)) return null;
    nums.forEach((x, i) => { v += x * 256 ** (3 - i); });
    return [v >>> 24, (v >>> 16) & 255, (v >>> 8) & 255, v & 255].join('.');
  }
  return h;
}

function dotSegments(path) {
  const out = [];
  const segs = path.split('/');
  for (let i = 0; i < segs.length; i++) {
    const s = segs[i], d = s.toLowerCase();
    if (d === '..' || d === '.%2e' || d === '%2e.' || d === '%2e%2e') {
      if (out.length > 1) out.pop();
      if (i === segs.length - 1) out.push('');
    } else if (d === '.' || d === '%2e') {
      if (i === segs.length - 1) out.push('');
    } else out.push(s);
  }
  return out.join('/');
}

function parseURL(input, base) {
  input = String(input).replace(/^[\x00-\x20]+|[\x00-\x20]+$/g, '').replace(/[\t\n\r]/g, '');
  const r = { protocol: '', username: '', password: '', hostname: '', port: '', pathname: '', search: '', hash: '', opaque: false };
  const m = /^([A-Za-z][A-Za-z0-9+.-]*):(.*)$/s.exec(input);
  let rest;
  if (m) {
    r.protocol = m[1].toLowerCase() + ':';
    rest = m[2];
    const isSpecial = r.protocol in special;
    if (isSpecial && base && base.protocol === r.protocol && !/^[/\\]/.test(rest) && r.protocol !== 'file:') {
      return parseRelative(rest, base);
    }
    if (isSpecial) rest = rest.replace(/\\/g, '/');
    // file:/path and file:///path have no host; file://host/path has one.
    if (r.protocol === 'file:') {
      if (rest.startsWith('//')) return authority(r, rest.slice(2), true);
      r.hostname = '';
      return tail(r, rest, true);
    }
    if (rest.startsWith('//') || isSpecial) {
      rest = rest.replace(/^\/*/, '');
      return authority(r, rest, isSpecial);
    }
    if (!rest.startsWith('/')) {
      r.opaque = true;
      const h = rest.indexOf('#');
      if (h >= 0) { r.hash = '#' + pct(rest.slice(h + 1), FRAG_SET); rest = rest.slice(0, h); }
      const q = rest.indexOf('?');
      if (q >= 0) { r.search = '?' + pct(rest.slice(q + 1), QUERY_SET); rest = rest.slice(0, q); }
      r.pathname = pct(rest, '');
      return r;
    }
    return tail(r, rest, false);
  }
  if (!base) return null;
  return parseRelative(input, base);
}

function parseRelative(input, base) {
  const r = Object.assign({}, base);
  const isSpecial = r.protocol in special;
  if (isSpecial) input = input.replace(/\\/g, '/');
  if (base.opaque) {
    if (!input.startsWith('#')) return null;
    r.hash = '#' + pct(input.slice(1), FRAG_SET);
    return r;
  }
  if (input.startsWith('//')) return authority(r, input.slice(2), isSpecial);
  if (input === '') { r.hash = ''; return r; }
  if (input.startsWith('#')) { r.hash = '#' + pct(input.slice(1), FRAG_SET); return r; }
  if (input.startsWith('?')) { r.hash = ''; return tail(r, r.pathname + input, isSpecial, true); }
  r.search = ''; r.hash = '';
  if (input.startsWith('/')) return tail(r, input, isSpecial);
  const dir = r.pathname.slice(0, r.pathname.lastIndexOf('/') + 1) || '/';
  return tail(r, dir + input, isSpecial);
}

function authority(r, rest, isSpecial) {
  let end = rest.search(isSpecial ? /[/?#]/ : /[/?#]/);
  if (end < 0) end = rest.length;
  let auth = rest.slice(0, end);
  rest = rest.slice(end);
  const at = auth.lastIndexOf('@');
  if (at >= 0) {
    const cred = auth.slice(0, at);
    auth = auth.slice(at + 1);
    const c = cred.indexOf(':');
    r.username = pct(c >= 0 ? cred.slice(0, c) : cred, USER_SET);
    r.password = c >= 0 ? pct(cred.slice(c + 1), USER_SET) : '';
  } else { r.username = ''; r.password = ''; }
  const pm = /^(\[[^\]]*\]|[^:]*)(?::(.*))?$/.exec(auth);
  const host = parseHost(pm[1], isSpecial);
  if (host === null) return null;
  if (isSpecial && host === '' && r.protocol !== 'file:') return null;
  r.hostname = host;
  r.port = '';
  if (pm[2] !== undefined && pm[2] !== '') {
    if (!/^[0-9]+$/.test(pm[2])) return null;
    const p = parseInt(pm[2], 10);
    if (p > 65535) return null;
    r.port = special[r.protocol] === p ? '' : String(p);
  }
  return tail(r, rest || (isSpecial ? '/' : ''), isSpecial);
}

function tail(r, rest, isSpecial, keepPath) {
  const h = rest.indexOf('#');
  r.hash = h >= 0 ? '#' + pct(rest.slice(h + 1), FRAG_SET) : '';
  if (h >= 0) rest = rest.slice(0, h);
  const q = rest.indexOf('?');
  r.search = q >= 0 ? '?' + pct(rest.slice(q + 1), isSpecial ? SPECIAL_QUERY_SET : QUERY_SET) : '';
  if (q >= 0) rest = rest.slice(0, q);
  if (r.search === '?') r.search = '';
  if (!keepPath) r.pathname = dotSegments(pct(rest, PATH_SET)) || (isSpecial ? '/' : '');
  if (isSpecial && !r.pathname.startsWith('/')) r.pathname = '/' + r.pathname;
  return r;
}

function serialize(r, excludeHash) {
  let s = r.protocol;
  if (!r.opaque && (r.hostname !== '' || r.protocol in special || r.protocol === 'file:')) {
    s += '//';
    if (r.username || r.password) s += r.username + (r.password ? ':' + r.password : '') + '@';
    s += r.hostname + (r.port ? ':' + r.port : '');
  }
  return s + r.pathname + r.search + (excludeHash ? '' : r.hash);
}

const kUrl = Symbol('url'), kQuery = Symbol('query');
class URL {
  constructor(url, base) {
    let b = null;
    if (base !== undefined) {
      b = parseURL(String(base), null);
      if (!b) throw new TypeError(`Invalid base URL: ${base}`);
    }
    const r = parseURL(String(url), b);
    if (!r) throw new TypeError(`Invalid URL: ${url}`);
    hide(this, kUrl, r);
    hide(this, kQuery, null);
  }
  static canParse(url, base) { try { new URL(url, base); return true; } catch (e) { return false; } }
  static parse(url, base) { try { return new URL(url, base); } catch (e) { return null; } }
  static createObjectURL(blob) { return blobUrls.add(blob); }
  static revokeObjectURL(url) { blobUrls.revoke(url); }
  get href() { return serialize(this[kUrl]); }
  set href(v) { const r = parseURL(String(v), null); if (!r) throw new TypeError(`Invalid URL: ${v}`); this[kUrl] = r; if (this[kQuery]) this[kQuery]._reset(r.search); }
  get origin() {
    const r = this[kUrl];
    if (r.protocol === 'blob:') { try { return new URL(r.pathname).origin; } catch (e) { return 'null'; } }
    return r.protocol in special && r.protocol !== 'file:' ? `${r.protocol}//${r.hostname}${r.port ? ':' + r.port : ''}` : 'null';
  }
  get protocol() { return this[kUrl].protocol; }
  set protocol(v) { v = String(v).replace(/:.*$/, '').toLowerCase(); if (/^[a-z][a-z0-9+.-]*$/.test(v) && ((v + ':') in special) === (this[kUrl].protocol in special)) { this[kUrl].protocol = v + ':'; if (special[v + ':'] === +this[kUrl].port) this[kUrl].port = ''; } }
  get username() { return this[kUrl].username; }
  set username(v) { if (this[kUrl].hostname) this[kUrl].username = pct(String(v), USER_SET); }
  get password() { return this[kUrl].password; }
  set password(v) { if (this[kUrl].hostname) this[kUrl].password = pct(String(v), USER_SET); }
  get host() { const r = this[kUrl]; return r.hostname + (r.port ? ':' + r.port : ''); }
  set host(v) { const r = this[kUrl]; if (r.opaque) return; const m = /^([^:/?#]*)(?::([0-9]*))?/.exec(String(v)); const h = parseHost(m[1], r.protocol in special); if (h) { r.hostname = h; if (m[2] !== undefined) this.port = m[2]; } }
  get hostname() { return this[kUrl].hostname; }
  set hostname(v) { const r = this[kUrl]; if (r.opaque) return; const h = parseHost(String(v).replace(/[:/?#].*$/, ''), r.protocol in special); if (h) r.hostname = h; }
  get port() { return this[kUrl].port; }
  set port(v) { const r = this[kUrl]; v = String(v); if (v === '') { r.port = ''; return; } const m = /^[0-9]+/.exec(v); if (!m) return; const p = parseInt(m[0], 10); if (p <= 65535) r.port = special[r.protocol] === p ? '' : String(p); }
  get pathname() { return this[kUrl].pathname; }
  set pathname(v) { const r = this[kUrl]; if (r.opaque) return; const sp = r.protocol in special; let p = String(v); if (sp) p = p.replace(/\\/g, '/'); r.pathname = dotSegments(pct(p.startsWith('/') || !sp ? p : '/' + p, PATH_SET + '?')); }
  get search() { return this[kUrl].search; }
  set search(v) { v = String(v); if (v.startsWith('?')) v = v.slice(1); this[kUrl].search = v ? '?' + pct(v, this[kUrl].protocol in special ? SPECIAL_QUERY_SET : QUERY_SET) : ''; if (this[kQuery]) this[kQuery]._reset(this[kUrl].search); }
  get searchParams() {
    if (!this[kQuery]) { const q = new URLSearchParams(this[kUrl].search); hide(q, '_url', this); this[kQuery] = q; }
    return this[kQuery];
  }
  get hash() { return this[kUrl].hash; }
  set hash(v) { v = String(v); if (v.startsWith('#')) v = v.slice(1); this[kUrl].hash = v ? '#' + pct(v, FRAG_SET) : ''; }
  toString() { return this.href; }
  toJSON() { return this.href; }
}
brand(URL, 'URL');

const formEncode = (s) => {
  let out = '';
  for (const b of new Uint8Array(n.utf8Encode(String(s)))) {
    const c = String.fromCharCode(b);
    if (/[A-Za-z0-9*\-._]/.test(c)) out += c;
    else if (b === 0x20) out += '+';
    else out += '%' + hex[b >> 4] + hex[b & 15];
  }
  return out;
};
const formDecode = (s) => {
  s = s.replace(/\+/g, ' ');
  const bytes = [];
  for (let i = 0; i < s.length; i++) {
    if (s[i] === '%' && /^[0-9A-Fa-f]{2}$/.test(s.substr(i + 1, 2))) { bytes.push(parseInt(s.substr(i + 1, 2), 16)); i += 2; }
    else for (const b of new Uint8Array(n.utf8Encode(s[i]))) bytes.push(b);
  }
  return n.utf8Decode(new Uint8Array(bytes), false, false)[0];
};

class URLSearchParams {
  constructor(init) {
    hide(this, '_list', []);
    hide(this, '_url', null);
    if (init == null) return;
    if (typeof init === 'object' && typeof init[Symbol.iterator] === 'function') {
      for (const pair of init) {
        const p = Array.from(pair);
        if (p.length !== 2) throw new TypeError('Each query pair must be an iterable [name, value] tuple');
        this._list.push([String(p[0]), String(p[1])]);
      }
    } else if (typeof init === 'object') {
      for (const k of Object.keys(init)) this._list.push([k, String(init[k])]);
    } else this._reset(String(init));
  }
  _reset(s) {
    this._list.length = 0;
    if (s.startsWith('?')) s = s.slice(1);
    for (const part of s.split('&')) {
      if (!part) continue;
      const eq = part.indexOf('=');
      this._list.push(eq >= 0 ? [formDecode(part.slice(0, eq)), formDecode(part.slice(eq + 1))] : [formDecode(part), '']);
    }
  }
  _update() {
    if (!this._url) return;
    const s = this.toString();
    this._url[kUrl].search = s ? '?' + s : '';
  }
  get size() { return this._list.length; }
  append(k, v) { this._list.push([String(k), String(v)]); this._update(); }
  delete(k, v) { k = String(k); for (let i = this._list.length; i--;) if (this._list[i][0] === k && (v === undefined || this._list[i][1] === String(v))) this._list.splice(i, 1); this._update(); }
  get(k) { k = String(k); const p = this._list.find(x => x[0] === k); return p ? p[1] : null; }
  getAll(k) { k = String(k); return this._list.filter(x => x[0] === k).map(x => x[1]); }
  has(k, v) { k = String(k); return this._list.some(x => x[0] === k && (v === undefined || x[1] === String(v))); }
  set(k, v) {
    k = String(k); v = String(v);
    const i = this._list.findIndex(x => x[0] === k);
    if (i < 0) this._list.push([k, v]);
    else { this._list[i][1] = v; for (let j = this._list.length; --j > i;) if (this._list[j][0] === k) this._list.splice(j, 1); }
    this._update();
  }
  sort() { const l = this._list.map((x, i) => [x, i]); l.sort((a, b) => a[0][0] < b[0][0] ? -1 : a[0][0] > b[0][0] ? 1 : a[1] - b[1]); this._list.splice(0, this._list.length, ...l.map(x => x[0])); this._update(); }
  forEach(fn, self) { for (const [k, v] of this._list.slice()) fn.call(self, v, k, this); }
  *keys() { for (const x of this._list) yield x[0]; }
  *values() { for (const x of this._list) yield x[1]; }
  *entries() { for (const x of this._list) yield [x[0], x[1]]; }
  [Symbol.iterator]() { return this.entries(); }
  toString() { return this._list.map(([k, v]) => formEncode(k) + '=' + formEncode(v)).join('&'); }
}
brand(URLSearchParams, 'URLSearchParams');
install('URL', URL); install('URLSearchParams', URLSearchParams);
install('webkitURL', URL);

// ── encoding ──────────────────────────────────────────────────────────────

const toBytes = (v) => {
  if (v instanceof ArrayBuffer) return new Uint8Array(v);
  if (ArrayBuffer.isView(v)) return new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
  if (v === undefined) return new Uint8Array(0);
  throw new TypeError('The provided value is not of type (ArrayBuffer or ArrayBufferView)');
};

class TextEncoder {
  get encoding() { return 'utf-8'; }
  encode(s = '') { return new Uint8Array(n.utf8Encode(String(s))); }
  encodeInto(s, dest) {
    s = String(s);
    let read = 0, written = 0;
    for (const ch of s) {
      const b = new Uint8Array(n.utf8Encode(ch));
      if (written + b.length > dest.length) break;
      dest.set(b, written);
      written += b.length;
      read += ch.length;
    }
    return { read, written };
  }
}
brand(TextEncoder, 'TextEncoder');

const latin1Names = ['latin1', 'iso-8859-1', 'iso8859-1', 'us-ascii', 'ascii', 'windows-1252', 'cp1252', 'l1', 'iso_8859-1'];
const cp1252 = '€\x81‚ƒ„…†‡ˆ‰Š‹Œ\x8dŽ\x8f\x90‘’“”•–—˜™š›œ\x9džŸ';
class TextDecoder {
  constructor(label = 'utf-8', options) {
    const l = String(label).trim().toLowerCase();
    if (['utf-8', 'utf8', 'unicode-1-1-utf-8'].includes(l)) hide(this, '_enc', 'utf-8');
    else if (latin1Names.includes(l)) hide(this, '_enc', 'windows-1252');
    else if (['utf-16le', 'utf-16'].includes(l)) hide(this, '_enc', 'utf-16le');
    else throw new RangeError(`The encoding label provided ('${label}') is invalid.`);
    hide(this, '_fatal', !!(options && options.fatal));
    hide(this, '_ignoreBOM', !!(options && options.ignoreBOM));
    hide(this, '_pending', null);
    hide(this, '_bomSeen', false);
  }
  get encoding() { return this._enc; }
  get fatal() { return this._fatal; }
  get ignoreBOM() { return this._ignoreBOM; }
  decode(input, options) {
    const stream = !!(options && options.stream);
    let bytes = toBytes(input);
    if (this._pending) { const b = new Uint8Array(this._pending.length + bytes.length); b.set(this._pending); b.set(bytes, this._pending.length); bytes = b; this._pending = null; }
    let text;
    if (this._enc === 'utf-8') {
      const [t, used] = n.utf8Decode(bytes, stream, this._fatal);
      if (used < bytes.length) this._pending = bytes.slice(used);
      text = t;
    } else if (this._enc === 'utf-16le') {
      const even = bytes.length & ~1;
      let s = '';
      for (let i = 0; i < even; i += 2) s += String.fromCharCode(bytes[i] | (bytes[i + 1] << 8));
      if (even < bytes.length) { if (stream) this._pending = bytes.slice(even); else s += '�'; }
      text = s;
    } else {
      let s = '';
      for (const b of bytes) s += b >= 0x80 && b < 0xa0 ? cp1252[b - 0x80] : String.fromCharCode(b);
      text = s;
    }
    if (!this._ignoreBOM && !this._bomSeen && text.length) {
      if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);
      this._bomSeen = true;
    }
    if (!stream) this._bomSeen = false;
    return text;
  }
}
brand(TextDecoder, 'TextDecoder');
install('TextEncoder', TextEncoder); install('TextDecoder', TextDecoder);

install('atob', function atob(s) {
  if (arguments.length === 0) throw new TypeError("Failed to execute 'atob': 1 argument required.");
  const r = n.atob(String(s));
  if (r === null) throw new DOMException("Failed to execute 'atob': The string to be decoded is not correctly encoded.", 'InvalidCharacterError');
  return r;
});
install('btoa', function btoa(s) {
  if (arguments.length === 0) throw new TypeError("Failed to execute 'btoa': 1 argument required.");
  const r = n.btoa(String(s));
  if (r === null) throw new DOMException("Failed to execute 'btoa': The string to be encoded contains characters outside of the Latin1 range.", 'InvalidCharacterError');
  return r;
});

// ── crypto ────────────────────────────────────────────────────────────────

install('crypto', {
  getRandomValues(a) {
    if (!ArrayBuffer.isView(a) || a instanceof Float32Array || a instanceof Float64Array || a instanceof DataView)
      throw new DOMException("The provided ArrayBufferView is of an unsupported type.", 'TypeMismatchError');
    if (a.byteLength > 65536) throw new DOMException('The ArrayBufferView\'s byte length exceeds 65536.', 'QuotaExceededError');
    return n.randomFill(a);
  },
  randomUUID() {
    const b = n.randomFill(new Uint8Array(16));
    b[6] = (b[6] & 0x0f) | 0x40; b[8] = (b[8] & 0x3f) | 0x80;
    const h = Array.from(b, x => x.toString(16).padStart(2, '0')).join('');
    return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
  },
});

// ── structuredClone ───────────────────────────────────────────────────────

function clone(v, seen) {
  if (v === null || typeof v !== 'object') {
    if (typeof v === 'function' || typeof v === 'symbol') throw new DOMException(`${String(v)} could not be cloned.`, 'DataCloneError');
    return v;
  }
  if (seen.has(v)) return seen.get(v);
  let out;
  if (v instanceof Date) out = new Date(v.getTime());
  else if (v instanceof RegExp) out = new RegExp(v.source, v.flags);
  else if (v instanceof ArrayBuffer) out = v.slice(0);
  else if (ArrayBuffer.isView(v)) out = new v.constructor(clone(v.buffer, seen), v.byteOffset, v instanceof DataView ? v.byteLength : v.length);
  else if (v instanceof Map) { out = new Map(); seen.set(v, out); for (const [k, x] of v) out.set(clone(k, seen), clone(x, seen)); return out; }
  else if (v instanceof Set) { out = new Set(); seen.set(v, out); for (const x of v) out.add(clone(x, seen)); return out; }
  else if (v instanceof Error) { out = new (g[v.name] || Error)(v.message); if (v.stack) out.stack = v.stack; }
  else if (v instanceof Boolean || v instanceof Number || v instanceof String) out = Object(v.valueOf());
  else if (typeof Blob !== 'undefined' && v instanceof Blob) out = v;
  else if (Array.isArray(v)) { out = new Array(v.length); seen.set(v, out); for (let i = 0; i < v.length; i++) if (i in v) out[i] = clone(v[i], seen); return out; }
  else {
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null && typeof v.nodeType === 'number') throw new DOMException('A node could not be cloned.', 'DataCloneError');
    out = {}; seen.set(v, out);
    for (const k of Object.keys(v)) out[k] = clone(v[k], seen);
    return out;
  }
  seen.set(v, out);
  return out;
}
install('structuredClone', function structuredClone(v) { return clone(v, new Map()); });

// ── Blob, File, FormData ──────────────────────────────────────────────────

const kBytes = Symbol('bytes');
const encoder = new TextEncoder();
class Blob {
  constructor(parts = [], options) {
    const chunks = [];
    let size = 0;
    for (const p of parts) {
      let b;
      if (p instanceof Blob) b = p[kBytes];
      else if (p instanceof ArrayBuffer || ArrayBuffer.isView(p)) b = toBytes(p).slice();
      else b = encoder.encode(String(p));
      chunks.push(b); size += b.length;
    }
    const all = new Uint8Array(size);
    let at = 0;
    for (const c of chunks) { all.set(c, at); at += c.length; }
    hide(this, kBytes, all);
    const t = options && options.type !== undefined ? String(options.type) : '';
    hide(this, '_type', /^[\x20-\x7e]*$/.test(t) ? t.toLowerCase() : '');
  }
  get size() { return this[kBytes].length; }
  get type() { return this._type; }
  slice(start = 0, end = this.size, type = '') {
    const s = this.size;
    const clamp = (x) => x < 0 ? Math.max(s + x, 0) : Math.min(x, s);
    const b = new Blob([], { type });
    b[kBytes] = this[kBytes].slice(clamp(start), clamp(end));
    return b;
  }
  text() { return Promise.resolve(new TextDecoder().decode(this[kBytes])); }
  arrayBuffer() { return Promise.resolve(this[kBytes].slice().buffer); }
  bytes() { return Promise.resolve(this[kBytes].slice()); }
}
brand(Blob, 'Blob');
class File extends Blob {
  constructor(parts, name, options) {
    super(parts, options);
    hide(this, '_name', String(name));
    hide(this, '_lastModified', options && options.lastModified !== undefined ? Number(options.lastModified) : Date.now());
  }
  get name() { return this._name; }
  get lastModified() { return this._lastModified; }
}
brand(File, 'File');

const blobUrls = {
  map: new Map(), next: 1,
  add(b) { const u = `blob:${n.baseUrl ? new URL(n.baseUrl).origin : 'null'}/${crypto.randomUUID()}`; this.map.set(u, b); return u; },
  revoke(u) { this.map.delete(String(u)); },
};

class FormData {
  constructor(form) {
    hide(this, '_list', []);
    if (form && typeof form._formData === 'function') for (const e of form._formData()) this._list.push(e);
  }
  _value(v, filename) {
    if (v instanceof Blob) return v instanceof File && filename === undefined ? v : new File([v], filename !== undefined ? filename : (v instanceof File ? v.name : 'blob'), { type: v.type });
    return String(v);
  }
  append(k, v, f) { this._list.push([String(k), this._value(v, f)]); }
  delete(k) { k = String(k); this._list = this._list.filter(x => x[0] !== k); }
  get(k) { k = String(k); const p = this._list.find(x => x[0] === k); return p ? p[1] : null; }
  getAll(k) { k = String(k); return this._list.filter(x => x[0] === k).map(x => x[1]); }
  has(k) { k = String(k); return this._list.some(x => x[0] === k); }
  set(k, v, f) {
    k = String(k); v = this._value(v, f);
    const i = this._list.findIndex(x => x[0] === k);
    if (i < 0) this._list.push([k, v]);
    else { this._list[i] = [k, v]; this._list = this._list.filter((x, j) => j <= i || x[0] !== k); }
  }
  forEach(fn, self) { for (const [k, v] of this._list.slice()) fn.call(self, v, k, this); }
  *keys() { for (const x of this._list) yield x[0]; }
  *values() { for (const x of this._list) yield x[1]; }
  *entries() { for (const x of this._list) yield [x[0], x[1]]; }
  [Symbol.iterator]() { return this.entries(); }
}
brand(FormData, 'FormData');
install('Blob', Blob); install('File', File); install('FormData', FormData);

// ── fetch ─────────────────────────────────────────────────────────────────

const forbiddenHeaders = /^(accept-charset|accept-encoding|access-control-request-headers|access-control-request-method|connection|content-length|cookie|cookie2|date|dnt|expect|host|keep-alive|origin|referer|te|trailer|transfer-encoding|upgrade|via|proxy-.*|sec-.*)$/;
class Headers {
  constructor(init) {
    hide(this, '_list', []);
    if (init == null) return;
    if (init instanceof Headers) { for (const [k, v] of init._list) this._list.push([k, v]); return; }
    if (typeof init[Symbol.iterator] === 'function') {
      for (const pair of init) { const p = Array.from(pair); if (p.length !== 2) throw new TypeError('Invalid header pair'); this.append(p[0], p[1]); }
    } else for (const k of Object.keys(init)) this.append(k, init[k]);
  }
  _name(k) {
    k = String(k).toLowerCase();
    if (!/^[!#$%&'*+\-.^_`|~0-9a-z]+$/.test(k)) throw new TypeError(`Invalid header name: ${k}`);
    return k;
  }
  _norm(v) { return String(v).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, ''); }
  append(k, v) {
    k = this._name(k); v = this._norm(v);
    const i = this._list.findIndex(x => x[0] === k);
    if (i >= 0 && k !== 'set-cookie') this._list[i][1] += ', ' + v;
    else this._list.push([k, v]);
  }
  delete(k) { k = this._name(k); this._list = this._list.filter(x => x[0] !== k); }
  get(k) { k = this._name(k); const vs = this._list.filter(x => x[0] === k).map(x => x[1]); return vs.length ? vs.join(', ') : null; }
  getSetCookie() { return this._list.filter(x => x[0] === 'set-cookie').map(x => x[1]); }
  has(k) { k = this._name(k); return this._list.some(x => x[0] === k); }
  set(k, v) { k = this._name(k); v = this._norm(v); this._list = this._list.filter(x => x[0] !== k); this._list.push([k, v]); }
  forEach(fn, self) { for (const [k, v] of this.entries()) fn.call(self, v, k, this); }
  *entries() { const l = this._list.slice().sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0); for (const x of l) yield [x[0], x[1]]; }
  *keys() { for (const [k] of this.entries()) yield k; }
  *values() { for (const [, v] of this.entries()) yield v; }
  [Symbol.iterator]() { return this.entries(); }
}
brand(Headers, 'Headers');

function parseHeaderBlock(text) {
  const h = new Headers();
  for (const line of String(text).split(/\r?\n/)) {
    const c = line.indexOf(':');
    if (c > 0) { try { h.append(line.slice(0, c).trim(), line.slice(c + 1).trim()); } catch (e) {} }
  }
  return h;
}

function bodyBytes(body, headers) {
  if (body == null) return null;
  if (typeof body === 'string') { if (!headers.has('content-type')) headers.set('content-type', 'text/plain;charset=UTF-8'); return encoder.encode(body); }
  if (body instanceof URLSearchParams) { if (!headers.has('content-type')) headers.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8'); return encoder.encode(body.toString()); }
  if (body instanceof Blob) { if (!headers.has('content-type') && body.type) headers.set('content-type', body.type); return body[kBytes]; }
  if (body instanceof FormData) {
    const boundary = '----r2web' + Array.from(crypto.getRandomValues(new Uint8Array(8)), b => b.toString(16).padStart(2, '0')).join('');
    if (!headers.has('content-type')) headers.set('content-type', 'multipart/form-data; boundary=' + boundary);
    const parts = [];
    const esc = (s) => s.replace(/\r\n|\r|\n/g, '\r\n').replace(/"/g, '%22');
    for (const [k, v] of body) {
      if (v instanceof File) {
        parts.push(`--${boundary}\r\nContent-Disposition: form-data; name="${esc(k)}"; filename="${esc(v.name)}"\r\nContent-Type: ${v.type || 'application/octet-stream'}\r\n\r\n`, v, '\r\n');
      } else parts.push(`--${boundary}\r\nContent-Disposition: form-data; name="${esc(k)}"\r\n\r\n${v.replace(/\r\n|\r|\n/g, '\r\n')}\r\n`);
    }
    parts.push(`--${boundary}--\r\n`);
    return new Blob(parts)[kBytes];
  }
  if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) return toBytes(body).slice();
  if (typeof ReadableStream !== 'undefined' && body instanceof ReadableStream) throw new TypeError('Streaming request bodies are not supported.');
  return bodyBytes(String(body), headers);
}

const kBody = Symbol('body');
class Body {
  _initBody(bytesPromise) { hide(this, kBody, bytesPromise); hide(this, '_used', false); }
  get bodyUsed() { return this._used; }
  _consume() {
    if (this._used) return Promise.reject(new TypeError('Body has already been consumed.'));
    this._used = true;
    return this[kBody] || Promise.resolve(new Uint8Array(0));
  }
  get body() {
    if (typeof ReadableStream === 'undefined') return null;
    const self = this;
    return new ReadableStream({ start(c) { self._consume().then(b => { if (b.length) c.enqueue(b); c.close(); }, e => c.error(e)); } });
  }
  arrayBuffer() { return this._consume().then(b => b.slice().buffer); }
  bytes() { return this._consume().then(b => b.slice()); }
  text() { return this._consume().then(b => new TextDecoder().decode(b)); }
  json() { return this.text().then(t => JSON.parse(t)); }
  blob() { return this._consume().then(b => new Blob([b], { type: (this.headers && this.headers.get('content-type')) || '' })); }
  formData() {
    return this.text().then(t => {
      const f = new FormData();
      for (const [k, v] of new URLSearchParams(t)) f.append(k, v);
      return f;
    });
  }
}

class Request extends Body {
  constructor(input, init) {
    super();
    init = init || {};
    const from = input instanceof Request ? input : null;
    const url = from ? from.url : new URL(String(input), n.baseUrl || undefined).href;
    hide(this, '_url', url);
    hide(this, '_method', String(init.method || (from ? from.method : 'GET')).toUpperCase());
    hide(this, '_headers', new Headers(init.headers || (from ? from.headers : undefined)));
    hide(this, '_signal', init.signal || (from ? from.signal : null) || new AbortController().signal);
    hide(this, '_mode', init.mode || 'cors');
    hide(this, '_credentials', init.credentials || 'same-origin');
    hide(this, '_redirect', init.redirect || 'follow');
    hide(this, '_cache', init.cache || 'default');
    if (init.body != null && (this._method === 'GET' || this._method === 'HEAD')) throw new TypeError('Request with GET/HEAD method cannot have body.');
    const raw = init.body !== undefined ? bodyBytes(init.body, this._headers) : from ? from._raw : null;
    hide(this, '_raw', raw);
    this._initBody(raw ? Promise.resolve(raw) : null);
  }
  get url() { return this._url; }
  get method() { return this._method; }
  get headers() { return this._headers; }
  get signal() { return this._signal; }
  get mode() { return this._mode; }
  get credentials() { return this._credentials; }
  get redirect() { return this._redirect; }
  get cache() { return this._cache; }
  get destination() { return ''; }
  get referrer() { return 'about:client'; }
  clone() { if (this._used) throw new TypeError('Request body is already used'); return new Request(this); }
}
brand(Request, 'Request');

class Response extends Body {
  constructor(body = null, init) {
    super();
    init = init || {};
    const status = init.status !== undefined ? Number(init.status) : 200;
    if (status < 200 || status > 599) throw new RangeError(`Failed to construct 'Response': The status provided (${status}) is outside the range [200, 599].`);
    hide(this, '_status', status);
    hide(this, '_statusText', init.statusText !== undefined ? String(init.statusText) : '');
    hide(this, '_headers', new Headers(init.headers));
    hide(this, '_url', '');
    hide(this, '_type', 'default');
    hide(this, '_redirected', false);
    const raw = body === null ? null : bodyBytes(body, this._headers);
    this._initBody(raw ? Promise.resolve(raw) : null);
  }
  static error() { const r = new Response(null, { status: 200 }); r._status = 0; r._type = 'error'; return r; }
  static redirect(url, status = 302) { const r = new Response(null, { status, headers: { location: new URL(url, n.baseUrl || undefined).href } }); return r; }
  static json(data, init) { const h = new Headers(init && init.headers); if (!h.has('content-type')) h.set('content-type', 'application/json'); return new Response(JSON.stringify(data), Object.assign({}, init, { headers: h })); }
  get status() { return this._status; }
  get ok() { return this._status >= 200 && this._status < 300; }
  get statusText() { return this._statusText; }
  get headers() { return this._headers; }
  get url() { return this._url; }
  get type() { return this._type; }
  get redirected() { return this._redirected; }
  clone() {
    if (this._used) throw new TypeError('Response body is already used');
    const r = Object.create(Response.prototype);
    for (const k of ['_status', '_statusText', '_url', '_type', '_redirected']) hide(r, k, this[k]);
    hide(r, '_headers', new Headers(this._headers));
    r._initBody(this[kBody]);
    return r;
  }
}
brand(Response, 'Response');

function headerText(headers) {
  let s = '';
  for (const [k, v] of headers._list) if (!forbiddenHeaders.test(k)) s += `${k}: ${v}\r\n`;
  return s;
}

// The transport, as the three network APIs use it: one request, its events.
function request(method, url, headers, body, stream, on) {
  return n.open(method, url, headerText(headers), body, stream, (kind, a, b, c, d) => {
    if (kind === 'headers') on.headers(a, b, c, d);
    else if (kind === 'data') on.data(new Uint8Array(a));
    else if (kind === 'end') on.end();
    else on.fail(a);
  });
}

function fetch(input, init) {
  return new Promise((resolve, reject) => {
    let req;
    try { req = new Request(input, init); } catch (e) { reject(e); return; }
    const signal = req.signal;
    if (signal.aborted) { reject(signal.reason); return; }
    const u = new URL(req.url);
    if (u.protocol === 'data:') { resolve(dataResponse(u)); return; }
    if (u.protocol === 'blob:') {
      const b = blobUrls.map.get(u.href);
      if (!b) reject(new TypeError('Failed to fetch')); else resolve(new Response(b, { headers: { 'content-type': b.type } }));
      return;
    }
    if (u.protocol !== 'http:' && u.protocol !== 'https:') { reject(new TypeError(`Failed to fetch: ${u.protocol} is not supported`)); return; }
    let id = 0, chunks = [], size = 0, response = null, finish, fail;
    const done = new Promise((res, rej) => { finish = res; fail = rej; });
    done.catch(() => {});
    const onAbort = () => {
      if (id) n.close(id);
      id = 0;
      if (!response) reject(signal.reason); else fail(signal.reason);
    };
    signal.addEventListener('abort', onAbort, { once: true });
    try {
      id = request(req.method, req.url, req.headers, req._raw, false, {
        headers(status, statusText, headerBlock, finalUrl) {
          response = Object.create(Response.prototype);
          hide(response, '_status', status);
          hide(response, '_statusText', statusText);
          hide(response, '_headers', parseHeaderBlock(headerBlock));
          hide(response, '_url', finalUrl || req.url);
          hide(response, '_type', 'basic');
          hide(response, '_redirected', !!finalUrl && finalUrl !== req.url);
          response._initBody(done);
          resolve(response);
        },
        data(bytes) { chunks.push(bytes); size += bytes.length; },
        end() {
          signal.removeEventListener('abort', onAbort);
          const all = new Uint8Array(size);
          let at = 0;
          for (const c of chunks) { all.set(c, at); at += c.length; }
          chunks = null;
          finish(all);
        },
        fail(msg) {
          signal.removeEventListener('abort', onAbort);
          const e = new TypeError(`Failed to fetch: ${msg}`);
          if (!response) reject(e); else fail(e);
        },
      });
    } catch (e) { reject(new TypeError(`Failed to fetch: ${e.message}`)); }
  });
}

function dataResponse(u) {
  const s = u.href.slice(5);
  const comma = s.indexOf(',');
  if (comma < 0) throw new TypeError('Invalid data URL');
  const meta = s.slice(0, comma), payload = s.slice(comma + 1);
  const b64 = /;base64$/i.test(meta);
  const type = (b64 ? meta.slice(0, -7) : meta) || 'text/plain;charset=US-ASCII';
  let bytes;
  if (b64) { const bin = atob(decodeURIComponent(payload)); bytes = Uint8Array.from(bin, ch => ch.charCodeAt(0)); }
  else bytes = Uint8Array.from(unescape(payload), ch => ch.charCodeAt(0) & 255);
  return new Response(bytes, { headers: { 'content-type': type } });
}

install('Headers', Headers); install('Request', Request); install('Response', Response); install('fetch', fetch);

// ── XMLHttpRequest ────────────────────────────────────────────────────────

class XMLHttpRequestEventTarget extends EventTarget {}
for (const t of ['loadstart', 'progress', 'abort', 'error', 'load', 'timeout', 'loadend']) eventHandler(XMLHttpRequestEventTarget.prototype, t);
class XMLHttpRequestUpload extends XMLHttpRequestEventTarget {}

class XMLHttpRequest extends XMLHttpRequestEventTarget {
  constructor() {
    super();
    hide(this, '_s', { state: 0, method: 'GET', url: '', headers: new Headers(), response: null, respHeaders: new Headers(), status: 0, statusText: '', id: 0, chunks: [], size: 0, send: false, url2: '', error: false, timer: 0, mime: null });
    this.responseType = '';
    this.timeout = 0;
    this.withCredentials = false;
    hide(this, '_upload', new XMLHttpRequestUpload());
  }
  get upload() { return this._upload; }
  get readyState() { return this._s.state; }
  get status() { return this._s.status; }
  get statusText() { return this._s.statusText; }
  get responseURL() { return this._s.url2; }
  get responseText() {
    if (this.responseType !== '' && this.responseType !== 'text') throw new DOMException("The value is only accessible if the object's 'responseType' is '' or 'text'.", 'InvalidStateError');
    return this._s.state < 3 ? '' : new TextDecoder().decode(this._bytes());
  }
  get responseXML() { return null; }
  get response() {
    const s = this._s, t = this.responseType;
    if (t === '' || t === 'text') return this.responseText;
    if (s.state !== 4 || s.error) return null;
    if (s.response !== null) return s.response;
    const b = this._bytes();
    if (t === 'json') { try { s.response = JSON.parse(new TextDecoder().decode(b)); } catch (e) { s.response = null; } }
    else if (t === 'arraybuffer') s.response = b.slice().buffer;
    else if (t === 'blob') s.response = new Blob([b], { type: s.respHeaders.get('content-type') || '' });
    else if (t === 'document' && typeof DOMParser !== 'undefined') s.response = new DOMParser().parseFromString(new TextDecoder().decode(b), 'text/html');
    return s.response;
  }
  _bytes() {
    const s = this._s;
    if (s.chunks.length !== 1) {
      const all = new Uint8Array(s.size);
      let at = 0;
      for (const c of s.chunks) { all.set(c, at); at += c.length; }
      s.chunks = [all];
    }
    return s.chunks[0] || new Uint8Array(0);
  }
  _change(state) { this._s.state = state; this.dispatchEvent(new Event('readystatechange')); }
  _progress(type, target = this) {
    const s = this._s;
    const total = +s.respHeaders.get('content-length') || 0;
    target.dispatchEvent(new ProgressEvent(type, { lengthComputable: !!total, loaded: s.size, total }));
  }
  open(method, url, async = true) {
    if (async === false) throw new DOMException('Synchronous XMLHttpRequest is not supported by r2web.', 'InvalidAccessError');
    this.abort();
    const s = this._s;
    s.method = String(method).toUpperCase();
    s.url = new URL(String(url), n.baseUrl || undefined).href;
    s.headers = new Headers();
    Object.assign(s, { response: null, respHeaders: new Headers(), status: 0, statusText: '', chunks: [], size: 0, send: false, url2: '', error: false });
    this._change(1);
  }
  setRequestHeader(k, v) {
    if (this._s.state !== 1 || this._s.send) throw new DOMException("The object's state must be OPENED.", 'InvalidStateError');
    this._s.headers.append(k, v);
  }
  overrideMimeType(m) { this._s.mime = String(m); }
  getResponseHeader(k) { return this._s.state < 2 ? null : this._s.respHeaders.get(k); }
  getAllResponseHeaders() {
    if (this._s.state < 2) return '';
    let out = '';
    for (const [k, v] of this._s.respHeaders) out += `${k}: ${v}\r\n`;
    return out;
  }
  send(body = null) {
    const s = this._s;
    if (s.state !== 1 || s.send) throw new DOMException("The object's state must be OPENED.", 'InvalidStateError');
    s.send = true;
    const raw = s.method === 'GET' || s.method === 'HEAD' ? null : bodyBytes(body, s.headers);
    this._progress('loadstart');
    const u = new URL(s.url);
    if (u.protocol === 'data:') {
      setTimeout(() => {
        const r = dataResponse(u);
        s.status = 200; s.statusText = 'OK'; s.respHeaders = r.headers; s.url2 = s.url;
        this._change(2);
        r.bytes().then(b => { s.chunks = [b]; s.size = b.length; this._change(3); this._finish(); });
      });
      return;
    }
    if (this.timeout > 0) s.timer = setTimeout(() => this._fail('timeout'), this.timeout);
    try {
      s.id = request(s.method, s.url, s.headers, raw, false, {
        headers: (status, text, block, finalUrl) => {
          s.status = status; s.statusText = text; s.respHeaders = parseHeaderBlock(block); s.url2 = finalUrl || s.url;
          this._change(2);
        },
        data: (bytes) => { s.chunks.push(bytes); s.size += bytes.length; if (s.state === 2) this._change(3); else this._change(3); this._progress('progress'); },
        end: () => { s.id = 0; if (s.state < 3) this._change(3); this._finish(); },
        fail: () => { s.id = 0; this._fail('error'); },
      });
    } catch (e) { setTimeout(() => this._fail('error')); }
  }
  _finish() {
    const s = this._s;
    if (s.timer) clearTimeout(s.timer);
    this._change(4);
    this._progress('load');
    this._progress('loadend');
  }
  _fail(type) {
    const s = this._s;
    if (s.id) n.close(s.id);
    s.id = 0;
    if (s.timer) clearTimeout(s.timer);
    s.error = true; s.status = 0; s.statusText = ''; s.chunks = []; s.size = 0;
    this._change(4);
    this._progress(type);
    this._progress('loadend');
  }
  abort() {
    const s = this._s;
    if (s.id) {
      n.close(s.id); s.id = 0;
      if (s.timer) clearTimeout(s.timer);
      s.error = true; s.chunks = []; s.size = 0;
      this._change(4);
      this._progress('abort');
      this._progress('loadend');
    }
    if (s.state === 4) s.state = 0;
  }
}
Object.assign(XMLHttpRequest, { UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4 });
eventHandler(XMLHttpRequest.prototype, 'readystatechange');
install('XMLHttpRequestEventTarget', XMLHttpRequestEventTarget);
install('XMLHttpRequestUpload', XMLHttpRequestUpload);
install('XMLHttpRequest', XMLHttpRequest);

// ── EventSource ───────────────────────────────────────────────────────────

class EventSource extends EventTarget {
  constructor(url, init) {
    super();
    const u = new URL(String(url), n.baseUrl || undefined);
    hide(this, '_url', u.href);
    hide(this, '_withCredentials', !!(init && init.withCredentials));
    hide(this, '_s', { state: 0, id: 0, retry: 3000, lastId: '', timer: 0, decoder: null, buf: '', data: '', event: '', idSet: false });
    setTimeout(() => this._connect(), 0);
  }
  get url() { return this._url; }
  get withCredentials() { return this._withCredentials; }
  get readyState() { return this._s.state; }
  close() {
    const s = this._s;
    if (s.id) n.close(s.id);
    if (s.timer) clearTimeout(s.timer);
    s.id = 0; s.timer = 0; s.state = 2;
  }
  _connect() {
    const s = this._s;
    if (s.state === 2) return;
    s.timer = 0;
    s.decoder = new TextDecoder();
    s.buf = ''; s.data = ''; s.event = '';
    const h = new Headers({ accept: 'text/event-stream', 'cache-control': 'no-cache' });
    if (s.lastId) h.set('last-event-id', s.lastId);
    let ok = false;
    try {
      s.id = request('GET', this._url, h, null, true, {
        headers: (status, text, block) => {
          const type = (parseHeaderBlock(block).get('content-type') || '').split(';')[0].trim().toLowerCase();
          if (status !== 200 || type !== 'text/event-stream') {
            n.close(s.id); s.id = 0;
            s.state = 2;
            this.dispatchEvent(new Event('error'));
            return;
          }
          ok = true;
          s.state = 1;
          this.dispatchEvent(new Event('open'));
        },
        data: (bytes) => { if (ok) this._feed(s.decoder.decode(bytes, { stream: true })); },
        end: () => { s.id = 0; this._reconnect(); },
        fail: () => { s.id = 0; this._reconnect(); },
      });
    } catch (e) { this._reconnect(); }
  }
  _reconnect() {
    const s = this._s;
    if (s.state === 2) return;
    s.state = 0;
    this.dispatchEvent(new Event('error'));
    if (s.state === 2) return;
    s.timer = setTimeout(() => this._connect(), s.retry);
  }
  _feed(text) {
    const s = this._s;
    s.buf += text;
    for (;;) {
      const m = /\r\n|\r|\n/.exec(s.buf);
      if (!m) break;
      // A lone \r at the very end may be the first half of \r\n.
      if (m[0] === '\r' && m.index === s.buf.length - 1) break;
      const line = s.buf.slice(0, m.index);
      s.buf = s.buf.slice(m.index + m[0].length);
      this._line(line);
      if (s.state === 2) return;
    }
  }
  _line(line) {
    const s = this._s;
    if (line === '') {
      if (s.idSet) s.idSet = false;
      if (s.data === '') { s.event = ''; return; }
      const data = s.data.endsWith('\n') ? s.data.slice(0, -1) : s.data;
      const ev = new MessageEvent(s.event || 'message', { data, origin: new URL(this._url).origin, lastEventId: s.lastId });
      s.data = ''; s.event = '';
      this.dispatchEvent(ev);
      return;
    }
    if (line.startsWith(':')) return;
    const c = line.indexOf(':');
    const field = c < 0 ? line : line.slice(0, c);
    let value = c < 0 ? '' : line.slice(c + 1);
    if (value.startsWith(' ')) value = value.slice(1);
    if (field === 'event') s.event = value;
    else if (field === 'data') s.data += value + '\n';
    else if (field === 'id') { if (!value.includes('\0')) { s.lastId = value; s.idSet = true; } }
    else if (field === 'retry') { if (/^[0-9]+$/.test(value)) s.retry = parseInt(value, 10); }
  }
}
Object.assign(EventSource, { CONNECTING: 0, OPEN: 1, CLOSED: 2 });
Object.assign(EventSource.prototype, { CONNECTING: 0, OPEN: 1, CLOSED: 2 });
for (const t of ['open', 'message', 'error']) eventHandler(EventSource.prototype, t);
install('EventSource', EventSource);

// ── navigator, self ───────────────────────────────────────────────────────

install('navigator', {
  userAgent: 'Mozilla/5.0 (rou2exOS; x86_64) r2web/2.0 QuickJS',
  appName: 'Netscape', appVersion: '5.0 (rou2exOS)', appCodeName: 'Mozilla', product: 'Gecko',
  platform: 'r2', vendor: '', language: 'en-US', languages: ['en-US', 'en'],
  onLine: true, cookieEnabled: false, hardwareConcurrency: 1, maxTouchPoints: 0, webdriver: false,
  doNotTrack: null, pdfViewerEnabled: false,
  sendBeacon(url, data) { fetch(url, { method: 'POST', body: data }).catch(() => {}); return true; },
});
install('self', g);

// What an embedder's own prelude builds on.
hide(g, '__jsr2lib', {
  n, hide, install, brand, eventHandler, dispatch, inspect, toBytes, parseURL, serialize, blobUrls,
  kParent, kActivation, kListeners, kBytes, parseHeaderBlock,
});
})(__jsr2);
