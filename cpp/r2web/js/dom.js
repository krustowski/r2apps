// dom.js --- r2web's document: the DOM, the window, and the bridge to the
// browser.
//
// Compiled to bytecode at build time (libjsr2's jsr2c) and run after
// libjsr2's prelude in every page that has scripts.  The tree lives here,
// in JavaScript; the browser keeps its own text layout (web/doc.cpp) and
// gets this tree back as HTML whenever it changed (render()), with every
// link and form control marked onclick="r2:N" so that what the user does to
// the laid-out page comes back here as events on node N.
//
// `b` is the browser's natives (script.cpp):
//   parse(html, fragment) -> flat ops   invalidate()   navigate(url, replace)
//   alert(text)   loadScript(url, id)   evalScript(src, name)   status(text)
//   storage(origin, json?)   viewport() -> [cols, rows, cellW, cellH, dark]
// and what the browser calls back is the object this function returns.
(function (b, lib) {
'use strict';
const g = globalThis;
const { hide, install, eventHandler, kParent, kActivation, kListeners } = lib;

const ELEMENT = 1, ATTRIBUTE = 2, TEXT = 3, CDATA = 4, PI = 7, COMMENT = 8, DOCUMENT = 9, DOCTYPE = 10, FRAGMENT = 11;
const HTML_NS = 'http://www.w3.org/1999/xhtml', SVG_NS = 'http://www.w3.org/2000/svg', MATH_NS = 'http://www.w3.org/1998/Math/MathML';
const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr', 'param', 'keygen', 'basefont', 'bgsound', 'frame']);
const RAW = new Set(['script', 'style', 'xmp', 'iframe', 'noembed', 'noframes', 'noscript', 'plaintext']);

let dirty = true;
let renderDepth = 0;
const touch = () => { if (!dirty) { dirty = true; b.invalidate(); } };

// ── mutation observers ────────────────────────────────────────────────────

const observers = [];
let observerQueued = false;
function queueRecord(type, target, fields) {
  for (const o of observers) {
    for (const reg of o._regs) {
      const opts = reg.options;
      let n = target, ok = false;
      for (; n; n = n.parentNode) {
        if (n === reg.target) { ok = n === target || opts.subtree; break; }
        if (!opts.subtree) break;
      }
      if (!ok) continue;
      if (type === 'attributes' && (!opts.attributes || (opts.attributeFilter && !opts.attributeFilter.includes(fields.attributeName)))) continue;
      if (type === 'characterData' && !opts.characterData) continue;
      if (type === 'childList' && !opts.childList) continue;
      const rec = Object.assign({ type, target, addedNodes: [], removedNodes: [], previousSibling: null, nextSibling: null, attributeName: null, attributeNamespace: null, oldValue: null }, fields);
      if (!((type === 'attributes' && opts.attributeOldValue) || (type === 'characterData' && opts.characterDataOldValue))) rec.oldValue = null;
      o._records.push(rec);
      if (!observerQueued) { observerQueued = true; queueMicrotask(notifyObservers); }
      break;
    }
  }
}
function notifyObservers() {
  observerQueued = false;
  for (const o of observers.slice()) {
    const recs = o.takeRecords();
    if (recs.length) { try { o._cb.call(o, recs, o); } catch (e) { reportError(e); } }
  }
}
class MutationObserver {
  constructor(cb) { if (typeof cb !== 'function') throw new TypeError("Failed to construct 'MutationObserver': parameter 1 is not of type 'Function'."); hide(this, '_cb', cb); hide(this, '_regs', []); hide(this, '_records', []); }
  observe(target, options = {}) {
    const o = Object.assign({}, options);
    if (o.attributeOldValue || o.attributeFilter) o.attributes = o.attributes !== false;
    if (o.characterDataOldValue) o.characterData = o.characterData !== false;
    if (!o.childList && !o.attributes && !o.characterData) throw new TypeError("The options object must set at least one of 'attributes', 'characterData', or 'childList' to true.");
    const r = this._regs.find(x => x.target === target);
    if (r) r.options = o; else this._regs.push({ target, options: o });
    if (!observers.includes(this)) observers.push(this);
  }
  disconnect() { this._regs.length = 0; this._records.length = 0; const i = observers.indexOf(this); if (i >= 0) observers.splice(i, 1); }
  takeRecords() { return this._records.splice(0); }
}
install('MutationObserver', MutationObserver);
install('WebKitMutationObserver', MutationObserver);

// ── nodes ─────────────────────────────────────────────────────────────────

let nextNodeId = 1;
class Node extends EventTarget {
  constructor() {
    super();
    hide(this, '_parent', null); hide(this, '_children', null); hide(this, '_doc', null);
    hide(this, '_uid', nextNodeId++);
  }
  get nodeType() { return 0; }
  get nodeName() { return ''; }
  get parentNode() { return this._parent; }
  get parentElement() { const p = this._parent; return p && p.nodeType === ELEMENT ? p : null; }
  get ownerDocument() { return this.nodeType === DOCUMENT ? null : this._doc; }
  get childNodes() { return nodeList(this._children || (this._children = [])); }
  get firstChild() { const c = this._children; return c && c.length ? c[0] : null; }
  get lastChild() { const c = this._children; return c && c.length ? c[c.length - 1] : null; }
  get previousSibling() { const p = this._parent; if (!p) return null; const i = p._children.indexOf(this); return i > 0 ? p._children[i - 1] : null; }
  get nextSibling() { const p = this._parent; if (!p) return null; const c = p._children, i = c.indexOf(this); return i >= 0 && i + 1 < c.length ? c[i + 1] : null; }
  get nodeValue() { return null; }
  set nodeValue(v) {}
  get textContent() { return null; }
  set textContent(v) {}
  get isConnected() { let n = this; while (n._parent) n = n._parent; return n.nodeType === DOCUMENT || (n._host && n._host.isConnected); }
  get baseURI() { return document.baseURI; }
  hasChildNodes() { return !!(this._children && this._children.length); }
  getRootNode(opts) { let n = this; for (;;) { if (n._parent) n = n._parent; else if (opts && opts.composed && n._host) n = n._host; else return n; } }
  contains(other) { for (let n = other; n; n = n._parent) if (n === this) return true; return false; }
  isSameNode(o) { return o === this; }
  isEqualNode(o) {
    if (!o || o.nodeType !== this.nodeType || o.nodeName !== this.nodeName || o.nodeValue !== this.nodeValue) return false;
    if (this.nodeType === ELEMENT) {
      if (this._attrs.length !== o._attrs.length) return false;
      for (const a of this._attrs) if (o.getAttribute(a.name) !== a.value) return false;
    }
    const x = this._children || [], y = o._children || [];
    return x.length === y.length && x.every((c, i) => c.isEqualNode(y[i]));
  }
  compareDocumentPosition(o) {
    if (o === this) return 0;
    const path = (n) => { const p = []; for (; n; n = n._parent) p.unshift(n); return p; };
    const a = path(this), c = path(o);
    if (a[0] !== c[0]) return 1 | 32 | (this._uid < o._uid ? 4 : 2);
    let i = 0;
    while (i < a.length && i < c.length && a[i] === c[i]) i++;
    if (i === a.length) return 16 | 4; // o is inside this
    if (i === c.length) return 8 | 2;  // this is inside o
    const p = a[i - 1]._children;
    return p.indexOf(c[i]) > p.indexOf(a[i]) ? 4 : 2;
  }
  normalize() {
    const c = this._children;
    if (!c) return;
    for (let i = 0; i < c.length; i++) {
      const n = c[i];
      if (n.nodeType === TEXT) {
        while (i + 1 < c.length && c[i + 1].nodeType === TEXT) { n._data += c[i + 1]._data; this.removeChild(c[i + 1]); }
        if (!n._data) { this.removeChild(n); i--; }
      } else n.normalize();
    }
  }
  cloneNode(deep) {
    const n = this._clone();
    if (deep && this._children) for (const c of this._children) n.appendChild(c.cloneNode(true));
    if (this._content) for (const c of this._content._children || []) n.content.appendChild(c.cloneNode(true));
    return n;
  }
  appendChild(n) { return this.insertBefore(n, null); }
  insertBefore(n, ref) {
    if (!(n instanceof Node)) throw new TypeError("Failed to execute 'insertBefore' on 'Node': parameter 1 is not of type 'Node'.");
    if (ref === undefined) ref = null;
    if (ref !== null && ref._parent !== this) throw new DOMException("The node before which the new node is to be inserted is not a child of this node.", 'NotFoundError');
    if (this.nodeType !== ELEMENT && this.nodeType !== DOCUMENT && this.nodeType !== FRAGMENT) throw new DOMException('This node type does not support this method.', 'HierarchyRequestError');
    for (let p = this; p; p = p._parent) if (p === n) throw new DOMException('The new child element contains the parent.', 'HierarchyRequestError');
    if (n === ref) ref = n.nextSibling;
    const nodes = n.nodeType === FRAGMENT ? (n._children || []).slice() : [n];
    if (n.nodeType === FRAGMENT) { for (const c of nodes) c._parent = null; if (nodes.length) { n._children = []; queueRecord('childList', n, { removedNodes: nodes }); } }
    else if (n._parent) n._parent.removeChild(n);
    const list = this._children || (this._children = []);
    let at = ref ? list.indexOf(ref) : list.length;
    const prev = at > 0 ? list[at - 1] : null;
    for (const c of nodes) {
      list.splice(at++, 0, c);
      c._parent = this;
      adopt(c, this.nodeType === DOCUMENT ? this : this._doc);
    }
    if (nodes.length) {
      queueRecord('childList', this, { addedNodes: nodes, previousSibling: prev, nextSibling: ref });
      touch();
      if (this.isConnected) for (const c of nodes) connected(c);
    }
    return n;
  }
  removeChild(n) {
    if (!n || n._parent !== this) throw new DOMException("The node to be removed is not a child of this node.", 'NotFoundError');
    const list = this._children, i = list.indexOf(n);
    const wasConnected = this.isConnected;
    list.splice(i, 1);
    n._parent = null;
    queueRecord('childList', this, { removedNodes: [n], previousSibling: i > 0 ? list[i - 1] : null, nextSibling: list[i] || null });
    touch();
    if (wasConnected) disconnected(n);
    return n;
  }
  replaceChild(n, old) {
    if (!old || old._parent !== this) throw new DOMException("The node to be replaced is not a child of this node.", 'NotFoundError');
    if (n === old) return old;
    const next = old.nextSibling;
    this.removeChild(old);
    this.insertBefore(n, next);
    return old;
  }
  lookupNamespaceURI() { return HTML_NS; }
  isDefaultNamespace(ns) { return ns === HTML_NS; }
  [kParent](event) {
    if (this._parent) return this._parent;
    if (this._host) return event.composed ? this._host : null;
    if (this.nodeType === DOCUMENT) return event.type === 'load' ? null : g;
    return null;
  }
}
const nodeConsts = { ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4, ENTITY_REFERENCE_NODE: 5, ENTITY_NODE: 6, PROCESSING_INSTRUCTION_NODE: 7, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10, DOCUMENT_FRAGMENT_NODE: 11, NOTATION_NODE: 12,
  DOCUMENT_POSITION_DISCONNECTED: 1, DOCUMENT_POSITION_PRECEDING: 2, DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_CONTAINS: 8, DOCUMENT_POSITION_CONTAINED_BY: 16, DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC: 32 };
Object.assign(Node, nodeConsts); Object.assign(Node.prototype, nodeConsts);

function adopt(n, doc) {
  if (n._doc === doc) return;
  n._doc = doc;
  if (n._children) for (const c of n._children) adopt(c, doc);
}

function nodeList(arr) {
  const l = Object.create(NodeList.prototype);
  hide(l, '_a', arr);
  return new Proxy(l, listTraps);
}
const listTraps = {
  get(t, k, r) { if (typeof k === 'string' && /^[0-9]+$/.test(k)) return t._a[+k]; return Reflect.get(t, k, r); },
  has(t, k) { if (typeof k === 'string' && /^[0-9]+$/.test(k)) return +k < t._a.length; return Reflect.has(t, k); },
  ownKeys(t) { return t._a.map((_, i) => String(i)).concat(Reflect.ownKeys(t)); },
  getOwnPropertyDescriptor(t, k) { if (typeof k === 'string' && /^[0-9]+$/.test(k) && +k < t._a.length) return { value: t._a[+k], enumerable: true, configurable: true, writable: false }; return Reflect.getOwnPropertyDescriptor(t, k); },
};
class NodeList {
  get length() { return this._a.length; }
  item(i) { return this._a[i] || null; }
  forEach(fn, self) { this._a.slice().forEach((n, i) => fn.call(self, n, i, this)); }
  entries() { return this._a.entries(); }
  keys() { return this._a.keys(); }
  values() { return this._a.values(); }
  [Symbol.iterator]() { return this._a.slice()[Symbol.iterator](); }
}
class HTMLCollection {
  get length() { return this._f().length; }
  item(i) { return this._f()[i] || null; }
  namedItem(name) { return this._f().find(e => e.id === name || e.getAttribute('name') === name) || null; }
  [Symbol.iterator]() { return this._f()[Symbol.iterator](); }
}
// A live collection: `f` computes the elements each time it is read.
function liveCollection(f) {
  const c = Object.create(HTMLCollection.prototype);
  hide(c, '_f', f);
  return new Proxy(c, {
    get(t, k, r) {
      if (typeof k === 'string') {
        if (/^[0-9]+$/.test(k)) return t._f()[+k];
        if (!(k in t)) { const n = t.namedItem(k); if (n) return n; }
      }
      return Reflect.get(t, k, r);
    },
    has(t, k) { if (typeof k === 'string' && /^[0-9]+$/.test(k)) return +k < t._f().length; return Reflect.has(t, k); },
    ownKeys(t) { return t._f().map((_, i) => String(i)).concat(Reflect.ownKeys(t)); },
    getOwnPropertyDescriptor(t, k) { if (typeof k === 'string' && /^[0-9]+$/.test(k)) { const v = t._f()[+k]; if (v) return { value: v, enumerable: true, configurable: true, writable: false }; } return Reflect.getOwnPropertyDescriptor(t, k); },
  });
}

class CharacterData extends Node {
  constructor(data) { super(); hide(this, '_data', String(data)); }
  get data() { return this._data; }
  set data(v) { v = v === null ? '' : String(v); const old = this._data; this._data = v; queueRecord('characterData', this, { oldValue: old }); touch(); }
  get nodeValue() { return this._data; }
  set nodeValue(v) { this.data = v; }
  get textContent() { return this._data; }
  set textContent(v) { this.data = v; }
  get length() { return this._data.length; }
  substringData(o, c) { return this._data.substr(o, c); }
  appendData(s) { this.data = this._data + s; }
  insertData(o, s) { this.data = this._data.slice(0, o) + s + this._data.slice(o); }
  deleteData(o, c) { this.data = this._data.slice(0, o) + this._data.slice(o + c); }
  replaceData(o, c, s) { this.data = this._data.slice(0, o) + s + this._data.slice(o + c); }
  get previousElementSibling() { return siblingElement(this, -1); }
  get nextElementSibling() { return siblingElement(this, 1); }
  remove() { if (this._parent) this._parent.removeChild(this); }
  before(...n) { insertNodes(this._parent, this, n); }
  after(...n) { insertNodes(this._parent, this.nextSibling, n); }
  replaceWith(...n) { const p = this._parent; if (!p) return; const next = this.nextSibling; p.removeChild(this); insertNodes(p, next, n); }
}
class Text extends CharacterData {
  constructor(data = '') { super(data); this._doc = g.document || null; }
  get nodeType() { return TEXT; }
  get nodeName() { return '#text'; }
  get wholeText() { return this._data; }
  splitText(o) { const t = new Text(this._data.slice(o)); this.data = this._data.slice(0, o); if (this._parent) this._parent.insertBefore(t, this.nextSibling); return t; }
  _clone() { const t = new Text(this._data); t._doc = this._doc; return t; }
}
class CDATASection extends Text { get nodeType() { return CDATA; } get nodeName() { return '#cdata-section'; } }
class Comment extends CharacterData {
  constructor(data = '') { super(data); this._doc = g.document || null; }
  get nodeType() { return COMMENT; }
  get nodeName() { return '#comment'; }
  _clone() { const c = new Comment(this._data); c._doc = this._doc; return c; }
}
class ProcessingInstruction extends CharacterData {
  get nodeType() { return PI; }
  get nodeName() { return this.target; }
  _clone() { const p = new ProcessingInstruction(this._data); p.target = this.target; return p; }
}
class DocumentType extends Node {
  constructor(name) { super(); hide(this, '_name', name); }
  get nodeType() { return DOCTYPE; }
  get nodeName() { return this._name; }
  get name() { return this._name; }
  get publicId() { return ''; }
  get systemId() { return ''; }
  remove() { if (this._parent) this._parent.removeChild(this); }
  _clone() { return new DocumentType(this._name); }
}

function siblingElement(n, dir) {
  const p = n._parent;
  if (!p) return null;
  const c = p._children;
  for (let i = c.indexOf(n) + dir; i >= 0 && i < c.length; i += dir) if (c[i].nodeType === ELEMENT) return c[i];
  return null;
}
function toNodes(list, doc) {
  return list.map(x => x instanceof Node ? x : doc.createTextNode(String(x)));
}
function insertNodes(parent, ref, list) {
  if (!parent) return;
  const nodes = toNodes(list, parent._doc || parent);
  if (nodes.length === 1) { parent.insertBefore(nodes[0], ref); return; }
  const f = (parent._doc || parent).createDocumentFragment();
  for (const n of nodes) f.appendChild(n);
  parent.insertBefore(f, ref);
}

// Element/Document/Fragment children helpers.
const ParentNodeMixin = {
  get children() { const self = this; return liveCollection(() => (self._children || []).filter(c => c.nodeType === ELEMENT)); },
  get childElementCount() { return (this._children || []).filter(c => c.nodeType === ELEMENT).length; },
  get firstElementChild() { return (this._children || []).find(c => c.nodeType === ELEMENT) || null; },
  get lastElementChild() { const c = this._children || []; for (let i = c.length; i--;) if (c[i].nodeType === ELEMENT) return c[i]; return null; },
  append(...n) { insertNodes(this, null, n); },
  prepend(...n) { insertNodes(this, this.firstChild, n); },
  replaceChildren(...n) { while (this._children && this._children.length) this.removeChild(this._children[0]); insertNodes(this, null, n); },
  querySelector(s) { return querySelector(this, s); },
  querySelectorAll(s) { return nodeList(querySelectorAll(this, s)); },
  getElementsByTagName(name) { const self = this; name = String(name); return liveCollection(() => byTag(self, name)); },
  getElementsByTagNameNS(ns, name) { return this.getElementsByTagName(name); },
  getElementsByClassName(names) { const self = this; const want = String(names).split(/\s+/).filter(Boolean); return liveCollection(() => descendants(self).filter(e => want.every(c => e.classList.contains(c)))); },
};
function mix(C, m) { Object.defineProperties(C.prototype, Object.getOwnPropertyDescriptors(m)); }

function descendants(root) {
  const out = [];
  const walk = (n) => { if (!n._children) return; for (const c of n._children) if (c.nodeType === ELEMENT) { out.push(c); walk(c); } };
  walk(root);
  return out;
}
function byTag(root, name) {
  const all = descendants(root);
  if (name === '*') return all;
  const lower = name.toLowerCase();
  return all.filter(e => e._ns === HTML_NS ? e._local === lower : e._local === name);
}

// ── attributes, classList, dataset, style ─────────────────────────────────

class Attr extends Node {
  constructor(name, value, owner) { super(); hide(this, '_name', name); hide(this, '_value', value); hide(this, '_owner', owner || null); }
  get nodeType() { return ATTRIBUTE; }
  get nodeName() { return this._name; }
  get name() { return this._name; }
  get localName() { return this._name.replace(/^.*:/, ''); }
  get namespaceURI() { return null; }
  get prefix() { return null; }
  get value() { return this._owner ? this._owner.getAttribute(this._name) : this._value; }
  set value(v) { if (this._owner) this._owner.setAttribute(this._name, v); else this._value = String(v); }
  get nodeValue() { return this.value; }
  get textContent() { return this.value; }
  get ownerElement() { return this._owner; }
  get specified() { return true; }
  _clone() { return new Attr(this._name, this.value); }
}
class NamedNodeMap {
  get length() { return this._el._attrs.length; }
  item(i) { const a = this._el._attrs[i]; return a ? new Attr(a.name, a.value, this._el) : null; }
  getNamedItem(n) { return this._el.hasAttribute(n) ? new Attr(normName(this._el, n), this._el.getAttribute(n), this._el) : null; }
  getNamedItemNS(ns, n) { return this.getNamedItem(n); }
  setNamedItem(a) { this._el.setAttribute(a.name, a.value); return null; }
  removeNamedItem(n) { const a = this.getNamedItem(n); if (!a) throw new DOMException('Not found', 'NotFoundError'); this._el.removeAttribute(n); return a; }
  [Symbol.iterator]() { return this._el._attrs.map((a) => new Attr(a.name, a.value, this._el))[Symbol.iterator](); }
}
function attrMap(el) {
  const m = Object.create(NamedNodeMap.prototype);
  hide(m, '_el', el);
  return new Proxy(m, {
    get(t, k, r) {
      if (typeof k === 'string') {
        if (/^[0-9]+$/.test(k)) return t.item(+k) || undefined;
        if (!(k in t) && el.hasAttribute(k)) return t.getNamedItem(k);
      }
      return Reflect.get(t, k, r);
    },
  });
}
const normName = (el, n) => el._ns === HTML_NS ? String(n).toLowerCase() : String(n);

class DOMTokenList {
  constructor(el, attr) { hide(this, '_el', el); hide(this, '_attr', attr); }
  _get() { return (this._el.getAttribute(this._attr) || '').split(/[\t\n\f\r ]+/).filter(Boolean).filter((v, i, a) => a.indexOf(v) === i); }
  _set(l) { this._el.setAttribute(this._attr, l.join(' ')); }
  _check(t) { t = String(t); if (!t) throw new DOMException('The token provided must not be empty.', 'SyntaxError'); if (/\s/.test(t)) throw new DOMException(`The token provided ('${t}') contains HTML space characters, which are not valid in tokens.`, 'InvalidCharacterError'); return t; }
  get length() { return this._get().length; }
  get value() { return this._el.getAttribute(this._attr) || ''; }
  set value(v) { this._el.setAttribute(this._attr, v); }
  item(i) { return this._get()[i] ?? null; }
  contains(t) { return this._get().includes(String(t)); }
  add(...ts) { const l = this._get(); for (const t of ts) { const v = this._check(t); if (!l.includes(v)) l.push(v); } this._set(l); }
  remove(...ts) { const r = ts.map(t => this._check(t)); const l = this._get().filter(x => !r.includes(x)); if (this._el.hasAttribute(this._attr)) this._set(l); }
  toggle(t, force) {
    t = this._check(t);
    const has = this.contains(t);
    if (has && force !== true) { this.remove(t); return false; }
    if (!has && force !== false) { this.add(t); return true; }
    return has;
  }
  replace(a, z) { a = this._check(a); z = this._check(z); const l = this._get(); const i = l.indexOf(a); if (i < 0) return false; l[i] = z; this._set(l.filter((v, j, x) => x.indexOf(v) === j)); return true; }
  supports() { return true; }
  forEach(fn, self) { this._get().forEach((v, i) => fn.call(self, v, i, this)); }
  entries() { return this._get().entries(); }
  keys() { return this._get().keys(); }
  values() { return this._get().values(); }
  toString() { return this.value; }
  [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
}
function tokenList(el, attr) {
  return new Proxy(new DOMTokenList(el, attr), {
    get(t, k, r) { if (typeof k === 'string' && /^[0-9]+$/.test(k)) return t._get()[+k]; return Reflect.get(t, k, r); },
  });
}

const camelToKebab = (s) => s.replace(/[A-Z]/g, m => '-' + m.toLowerCase());
const kebabToCamel = (s) => s.replace(/-([a-z])/g, (_, c) => c.toUpperCase());
function dataset(el) {
  return new Proxy({}, {
    get(t, k) { if (typeof k !== 'string') return undefined; const v = el.getAttribute('data-' + camelToKebab(k)); return v === null ? undefined : v; },
    set(t, k, v) { el.setAttribute('data-' + camelToKebab(String(k)), v); return true; },
    deleteProperty(t, k) { el.removeAttribute('data-' + camelToKebab(String(k))); return true; },
    has(t, k) { return typeof k === 'string' && el.hasAttribute('data-' + camelToKebab(k)); },
    ownKeys() { return el._attrs.filter(a => a.name.startsWith('data-')).map(a => kebabToCamel(a.name.slice(5))); },
    getOwnPropertyDescriptor(t, k) { const v = el.getAttribute('data-' + camelToKebab(String(k))); return v === null ? undefined : { value: v, enumerable: true, configurable: true, writable: true }; },
  });
}

// Declarations "a: b; c: d" with !important kept in the value.
function parseDecls(text) {
  const out = [];
  let depth = 0, quote = '', start = 0;
  const push = (s) => {
    const c = s.indexOf(':');
    if (c > 0) {
      const name = s.slice(0, c).trim().toLowerCase();
      let value = s.slice(c + 1).trim(), important = '';
      const m = /\s*!\s*important\s*$/i.exec(value);
      if (m) { important = 'important'; value = value.slice(0, m.index); }
      if (name && value) out.push([name.startsWith('--') ? s.slice(0, c).trim() : name, value, important]);
    }
  };
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (quote) { if (ch === quote) quote = ''; else if (ch === '\\') i++; continue; }
    if (ch === '"' || ch === "'") quote = ch;
    else if (ch === '(') depth++;
    else if (ch === ')') depth = Math.max(0, depth - 1);
    else if (ch === ';' && !depth) { push(text.slice(start, i)); start = i + 1; }
  }
  push(text.slice(start));
  return out;
}
class CSSStyleDeclaration {
  constructor(el) { hide(this, '_el', el); hide(this, '_decls', el ? parseDecls(el.getAttribute('style') || '') : []); }
  _sync() { if (this._el) { this._el._styleSync = true; this._el.setAttribute('style', this.cssText); this._el._styleSync = false; } }
  get cssText() { return this._decls.map(([k, v, i]) => `${k}: ${v}${i ? ' !important' : ''};`).join(' '); }
  set cssText(t) { this._decls.splice(0, this._decls.length, ...parseDecls(String(t))); this._sync(); }
  get length() { return this._decls.length; }
  item(i) { return this._decls[i] ? this._decls[i][0] : ''; }
  getPropertyValue(n) { n = String(n); n = n.startsWith('--') ? n : n.toLowerCase(); const d = this._decls.find(x => x[0] === n); return d ? d[1] : ''; }
  getPropertyPriority(n) { const d = this._decls.find(x => x[0] === String(n).toLowerCase()); return d ? d[2] : ''; }
  setProperty(n, v, prio = '') {
    n = String(n); n = n.startsWith('--') ? n : n.toLowerCase();
    if (v === null || v === '') { this.removeProperty(n); return; }
    const d = this._decls.find(x => x[0] === n);
    if (d) { d[1] = String(v); d[2] = prio ? 'important' : ''; } else this._decls.push([n, String(v), prio ? 'important' : '']);
    this._sync();
  }
  removeProperty(n) { n = String(n).toLowerCase(); const i = this._decls.findIndex(x => x[0] === n); if (i < 0) return ''; const v = this._decls[i][1]; this._decls.splice(i, 1); this._sync(); return v; }
  get cssFloat() { return this.getPropertyValue('float'); }
  set cssFloat(v) { this.setProperty('float', v); }
}
const styleTraps = {
  get(t, k, r) {
    if (typeof k !== 'string' || k in t) return Reflect.get(t, k, r);
    if (/^[0-9]+$/.test(k)) return t.item(+k);
    return t.getPropertyValue(camelToKebab(k).replace(/^webkit-/, '-webkit-').replace(/^moz-/, '-moz-').replace(/^ms-/, '-ms-'));
  },
  set(t, k, v) {
    if (typeof k !== 'string' || k in t) { Reflect.set(t, k, v); return true; }
    const name = camelToKebab(k).replace(/^webkit-/, '-webkit-').replace(/^moz-/, '-moz-');
    if (typeof v === 'number' && v !== 0 && !/^(opacity|z-index|font-weight|line-height|flex|flex-grow|flex-shrink|order|zoom|orphans|widows|column-count)$/.test(name)) v = v + 'px';
    t.setProperty(name, v);
    return true;
  },
};

// ── selectors ─────────────────────────────────────────────────────────────

const selectorCache = new Map();
function parseSelector(text) {
  text = String(text).trim();
  let hit = selectorCache.get(text);
  if (hit) return hit;
  let i = 0;
  const fail = () => { throw new DOMException(`'${text}' is not a valid selector.`, 'SyntaxError'); };
  const ws = () => { while (i < text.length && /\s/.test(text[i])) i++; };
  const ident = () => {
    let s = '';
    while (i < text.length) {
      const c = text[i];
      if (/[A-Za-z0-9_\-\u00a0-\uffff]/.test(c)) { s += c; i++; }
      else if (c === '\\') { i++; const m = /^[0-9a-fA-F]{1,6} ?/.exec(text.slice(i)); if (m) { s += String.fromCodePoint(parseInt(m[0], 16)); i += m[0].length; } else s += text[i++]; }
      else break;
    }
    return s;
  };
  const str = () => { const q = text[i++]; let s = ''; while (i < text.length && text[i] !== q) { if (text[i] === '\\') i++; s += text[i++]; } i++; return s; };
  const parens = () => { let depth = 1, s = ''; i++; while (i < text.length && depth) { const c = text[i++]; if (c === '(') depth++; else if (c === ')') { if (!--depth) break; } s += c; } return s; };
  function compound() {
    const c = { tag: null, id: null, classes: [], attrs: [], pseudo: [] };
    let any = false;
    if (text[i] === '*') { i++; any = true; }
    else if (/[A-Za-z_\\\u00a0-\uffff-]/.test(text[i] || '')) { c.tag = ident(); any = true; }
    for (;;) {
      const ch = text[i];
      if (ch === '#') { i++; c.id = ident(); any = true; }
      else if (ch === '.') { i++; const n = ident(); if (!n) fail(); c.classes.push(n); any = true; }
      else if (ch === '[') {
        i++; ws();
        const name = ident().toLowerCase(); ws();
        let op = null, value = null, flag = '';
        if (text[i] !== ']') {
          const m = /^([~|^$*]?=)/.exec(text.slice(i));
          if (!m) fail();
          op = m[1]; i += op.length; ws();
          value = text[i] === '"' || text[i] === "'" ? str() : ident();
          ws();
          if (/[iIsS]/.test(text[i] || '') && text[i + 1] !== '=') { flag = text[i++].toLowerCase(); ws(); }
        }
        if (text[i++] !== ']') fail();
        c.attrs.push({ name, op, value, flag });
        any = true;
      } else if (ch === ':') {
        i++;
        if (text[i] === ':') { i++; const name = ident(); c.pseudo.push({ name: '::' + name.toLowerCase() }); any = true; continue; }
        const name = ident().toLowerCase();
        let arg = null;
        if (text[i] === '(') arg = parens();
        c.pseudo.push({ name, arg, sub: /^(not|is|where|matches|-webkit-any)$/.test(name) ? parseSelector(arg) : /^nth-/.test(name) ? parseNth(arg) : null });
        any = true;
      } else break;
    }
    if (!any) fail();
    return c;
  }
  const list = [];
  ws();
  while (i < text.length) {
    const parts = [];
    let comb = null;
    for (;;) {
      ws();
      parts.push({ comb, c: compound() });
      const before = i;
      ws();
      const ch = text[i];
      if (i >= text.length || ch === ',') break;
      if (ch === '>' || ch === '+' || ch === '~') { comb = ch; i++; }
      else if (i > before) comb = ' ';
      else fail();
    }
    list.push(parts);
    ws();
    if (text[i] === ',') { i++; ws(); if (i >= text.length) fail(); }
  }
  if (!list.length) fail();
  if (selectorCache.size > 200) selectorCache.clear();
  selectorCache.set(text, list);
  return list;
}
function parseNth(arg) {
  let s = String(arg).trim(), of = null;
  const m = /^(.*?)\s+of\s+(.*)$/i.exec(s);
  if (m) { s = m[1]; of = parseSelector(m[2]); }
  s = s.replace(/\s+/g, '').toLowerCase();
  if (s === 'odd') return { a: 2, b: 1, of };
  if (s === 'even') return { a: 2, b: 0, of };
  const k = /^([+-]?\d*)n([+-]\d+)?$/.exec(s);
  if (k) return { a: k[1] === '' || k[1] === '+' ? 1 : k[1] === '-' ? -1 : parseInt(k[1], 10), b: k[2] ? parseInt(k[2], 10) : 0, of };
  if (/^[+-]?\d+$/.test(s)) return { a: 0, b: parseInt(s, 10), of };
  throw new DOMException(`'${arg}' is not a valid selector.`, 'SyntaxError');
}
function nthMatch(n, pos) {
  if (n.a === 0) return pos === n.b;
  return (pos - n.b) / n.a >= 0 && (pos - n.b) % n.a === 0;
}
function matchCompound(el, c, scope) {
  if (c.tag && c.tag !== '*' && (el._ns === HTML_NS ? el._local !== c.tag.toLowerCase() : el._local !== c.tag)) return false;
  if (c.id !== null && el.id !== c.id) return false;
  if (c.classes.length) { const cl = (el.getAttribute('class') || '').split(/\s+/); for (const x of c.classes) if (!cl.includes(x)) return false; }
  for (const a of c.attrs) {
    let v = el.getAttribute(a.name);
    if (v === null) return false;
    if (!a.op) continue;
    let want = a.value;
    if (a.flag === 'i' || (a.name === 'type' && el._ns === HTML_NS)) { v = v.toLowerCase(); want = want.toLowerCase(); }
    switch (a.op) {
      case '=': if (v !== want) return false; break;
      case '~=': if (!v.split(/\s+/).includes(want)) return false; break;
      case '|=': if (v !== want && !v.startsWith(want + '-')) return false; break;
      case '^=': if (!want || !v.startsWith(want)) return false; break;
      case '$=': if (!want || !v.endsWith(want)) return false; break;
      case '*=': if (!want || !v.includes(want)) return false; break;
    }
  }
  for (const p of c.pseudo) if (!matchPseudo(el, p, scope)) return false;
  return true;
}
function siblingsOf(el) { return el._parent ? el._parent._children.filter(x => x.nodeType === ELEMENT) : [el]; }
function matchPseudo(el, p, scope) {
  switch (p.name) {
    case 'not': return !matchesList(el, p.sub, scope);
    case 'is': case 'matches': case 'where': case '-webkit-any': return matchesList(el, p.sub, scope);
    case 'has': return String(p.arg).split(',').some(part => {
      // Relative selectors: "> x", "+ x", "~ x", or a descendant.
      const a = part.trim();
      if (a[0] === '>') return (el._children || []).some(c => c.nodeType === ELEMENT && (c.matches(a.slice(1)) || !!querySelector(c, a.slice(1))));
      if (a[0] === '+') { const s = siblingElement(el, 1); return !!s && s.matches(a.slice(1)); }
      if (a[0] === '~') { for (let s = siblingElement(el, 1); s; s = siblingElement(s, 1)) if (s.matches(a.slice(1))) return true; return false; }
      return !!querySelector(el, a);
    });
    case 'root': return el === el._doc?.documentElement;
    case 'scope': return scope ? el === scope : el === el._doc?.documentElement;
    case 'empty': return !(el._children || []).some(c => c.nodeType === ELEMENT || ((c.nodeType === TEXT) && c._data));
    case 'first-child': return siblingsOf(el)[0] === el;
    case 'last-child': { const s = siblingsOf(el); return s[s.length - 1] === el; }
    case 'only-child': return siblingsOf(el).length === 1;
    case 'first-of-type': return siblingsOf(el).find(x => x._local === el._local) === el;
    case 'last-of-type': { const s = siblingsOf(el).filter(x => x._local === el._local); return s[s.length - 1] === el; }
    case 'only-of-type': return siblingsOf(el).filter(x => x._local === el._local).length === 1;
    case 'nth-child': { let s = siblingsOf(el); if (p.sub.of) s = s.filter(x => matchesList(x, p.sub.of, scope)); const i = s.indexOf(el); return i >= 0 && nthMatch(p.sub, i + 1); }
    case 'nth-last-child': { let s = siblingsOf(el); if (p.sub.of) s = s.filter(x => matchesList(x, p.sub.of, scope)); const i = s.indexOf(el); return i >= 0 && nthMatch(p.sub, s.length - i); }
    case 'nth-of-type': { const s = siblingsOf(el).filter(x => x._local === el._local); return nthMatch(p.sub, s.indexOf(el) + 1); }
    case 'nth-last-of-type': { const s = siblingsOf(el).filter(x => x._local === el._local); return nthMatch(p.sub, s.length - s.indexOf(el)); }
    case 'checked': return (el._local === 'input' && el.checked) || (el._local === 'option' && el.selected);
    case 'disabled': return isFormControl(el) && el.disabled;
    case 'enabled': return isFormControl(el) && !el.disabled;
    case 'required': return isFormControl(el) && el.hasAttribute('required');
    case 'optional': return isFormControl(el) && !el.hasAttribute('required');
    case 'read-only': return isFormControl(el) && el.hasAttribute('readonly');
    case 'read-write': return (el._local === 'input' || el._local === 'textarea') && !el.hasAttribute('readonly');
    case 'placeholder-shown': return (el._local === 'input' || el._local === 'textarea') && el.hasAttribute('placeholder') && !el.value;
    case 'link': case 'any-link': return (el._local === 'a' || el._local === 'area') && el.hasAttribute('href');
    case 'defined': return !el._local.includes('-') || customElements.get(el._local) !== undefined;
    case 'focus': case 'focus-visible': return el === el._doc?.activeElement;
    case 'focus-within': return el.contains(el._doc?.activeElement);
    case 'target': return !!location.hash && el.id === decodeURIComponent(location.hash.slice(1));
    case 'lang': return (el.closest('[lang]')?.getAttribute('lang') || '').toLowerCase().startsWith(String(p.arg).toLowerCase());
    case 'hover': case 'active': case 'visited': case 'indeterminate': case 'invalid': case 'fullscreen': case 'modal': case 'popover-open': return false;
    case 'valid': return isFormControl(el);
    default:
      if (p.name.startsWith('::') || p.name.startsWith('-')) return false;
      throw new DOMException(`'${p.name}' is not a supported pseudo-class.`, 'SyntaxError');
  }
}
const isFormControl = (el) => /^(input|button|select|textarea|option|optgroup|fieldset)$/.test(el._local);
function matchParts(el, parts, k, scope) {
  if (!matchCompound(el, parts[k].c, scope)) return false;
  if (k === 0) return true;
  const comb = parts[k].comb;
  if (comb === '>') { const p = el.parentElement; return !!p && matchParts(p, parts, k - 1, scope); }
  if (comb === ' ') { for (let p = el.parentElement; p; p = p.parentElement) if (matchParts(p, parts, k - 1, scope)) return true; return false; }
  if (comb === '+') { const s = siblingElement(el, -1); return !!s && matchParts(s, parts, k - 1, scope); }
  if (comb === '~') { for (let s = siblingElement(el, -1); s; s = siblingElement(s, -1)) if (matchParts(s, parts, k - 1, scope)) return true; return false; }
  return false;
}
function matchesList(el, list, scope) { return list.some(parts => matchParts(el, parts, parts.length - 1, scope)); }
function querySelectorAll(root, sel) {
  const list = parseSelector(sel);
  const scope = root.nodeType === ELEMENT ? root : null;
  return descendants(root).filter(e => matchesList(e, list, scope));
}
function querySelector(root, sel) {
  const list = parseSelector(sel);
  const scope = root.nodeType === ELEMENT ? root : null;
  // Fast path for "#id".
  if (list.length === 1 && list[0].length === 1) {
    const c = list[0][0].c;
    if (c.id && !c.tag && !c.classes.length && !c.attrs.length && !c.pseudo.length) {
      const doc = root.nodeType === DOCUMENT ? root : null;
      if (doc) return doc.getElementById(c.id);
    }
  }
  let found = null;
  const walk = (n) => {
    if (!n._children) return false;
    for (const c of n._children) {
      if (c.nodeType !== ELEMENT) continue;
      if (matchesList(c, list, scope)) { found = c; return true; }
      if (walk(c)) return true;
    }
    return false;
  };
  walk(root);
  return found;
}

// ── HTML serialisation ────────────────────────────────────────────────────

const escText = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/\u00a0/g, '&nbsp;');
const escAttr = (s) => s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/\u00a0/g, '&nbsp;');
function serializeChildren(n, out) { if (n._children) for (const c of n._children) serializeNode(c, out); }
function serializeNode(n, out) {
  switch (n.nodeType) {
    case ELEMENT: {
      const tag = n._ns === HTML_NS ? n._local : n.tagName;
      out.push('<', tag);
      for (const a of n._attrs) out.push(' ', a.name, '="', escAttr(a.value), '"');
      out.push('>');
      if (n._ns === HTML_NS && VOID.has(n._local)) return;
      if (n._local === 'template' && n._content) serializeChildren(n._content, out);
      else if (n._ns === HTML_NS && RAW.has(n._local) && n._local !== 'noscript') { if (n._children) for (const c of n._children) out.push(c.nodeType === TEXT ? c._data : ''); }
      else serializeChildren(n, out);
      out.push('</', tag, '>');
      return;
    }
    case TEXT: {
      const p = n._parent;
      out.push(p && p.nodeType === ELEMENT && p._ns === HTML_NS && RAW.has(p._local) && p._local !== 'noscript' ? n._data : escText(n._data));
      return;
    }
    case COMMENT: out.push('<!--', n._data, '-->'); return;
    case DOCTYPE: out.push('<!DOCTYPE ', n._name, '>'); return;
    case PI: out.push('<?', n.target, ' ', n._data, '>'); return;
    default: serializeChildren(n, out);
  }
}

// Build nodes from the browser's parser output: a flat list of ops,
// [1, tag, attrs, ns] opens an element, 2 closes it, a string is text,
// [8, text] a comment, [10, name] a doctype.  `into` receives them.
function build(ops, into, doc) {
  const stack = [into];
  let top = into;
  for (let i = 0; i < ops.length; i++) {
    const op = ops[i];
    if (typeof op === 'string') {
      const last = top._children && top._children[top._children.length - 1];
      if (last && last.nodeType === TEXT) last._data += op;
      else appendQuiet(top, makeText(op, doc));
      continue;
    }
    if (op === 2) { if (stack.length > 1) { stack.pop(); top = stack[stack.length - 1]; } continue; }
    switch (op[0]) {
      case 1: {
        const ns = op[3] === 1 ? SVG_NS : op[3] === 2 ? MATH_NS : HTML_NS;
        const el = createElementIn(doc, op[1], ns, op[2]);
        appendQuiet(top, el);
        stack.push(el);
        top = el._local === 'template' && el._ns === HTML_NS ? el.content : el;
        stack[stack.length - 1] = top;
        break;
      }
      case 8: appendQuiet(top, makeComment(op[1], doc)); break;
      case 10: appendQuiet(top, Object.assign(new DocumentType(op[1]), { _doc: doc })); break;
    }
  }
}
function makeText(s, doc) { const t = new Text(s); t._doc = doc; return t; }
function makeComment(s, doc) { const c = new Comment(s); c._doc = doc; return c; }
function appendQuiet(parent, child) {
  (parent._children || (parent._children = [])).push(child);
  child._parent = parent;
}
function parseFragment(html, context) {
  const doc = context._doc || context;
  const f = doc.createDocumentFragment();
  build(b.parse(String(html), true, context.nodeType === ELEMENT ? context._local : 'body'), f, doc);
  return f;
}

// ── elements ──────────────────────────────────────────────────────────────

const kUpgrade = Symbol('upgrade');
let pendingUpgrade = null;
class Element extends Node {
  constructor() {
    super();
    if (pendingUpgrade) { const el = pendingUpgrade; pendingUpgrade = null; return el; }
    hide(this, '_ns', HTML_NS); hide(this, '_local', ''); hide(this, '_prefix', null);
    hide(this, '_attrs', []); hide(this, '_children', []);
    hide(this, '_classList', null); hide(this, '_style', null); hide(this, '_dataset', null);
    hide(this, '_ce', 0); // custom element state: 0 none, 1 undefined, 2 defined
    elementState(this);
  }
  get nodeType() { return ELEMENT; }
  get nodeName() { return this.tagName; }
  get tagName() { const n = (this._prefix ? this._prefix + ':' : '') + this._local; return this._ns === HTML_NS && this._doc && !this._doc._xml ? n.toUpperCase() : n; }
  get localName() { return this._local; }
  get namespaceURI() { return this._ns; }
  get prefix() { return this._prefix; }
  get id() { return this.getAttribute('id') || ''; }
  set id(v) { this.setAttribute('id', v); }
  get className() { return this.getAttribute('class') || ''; }
  set className(v) { this.setAttribute('class', v); }
  get classList() { return this._classList || (this._classList = tokenList(this, 'class')); }
  set classList(v) { this.setAttribute('class', v); }
  get slot() { return this.getAttribute('slot') || ''; }
  get attributes() { return attrMap(this); }
  get dataset() { return this._dataset || (this._dataset = dataset(this)); }
  get style() { if (!this._style) this._style = new Proxy(new CSSStyleDeclaration(this), styleTraps); return this._style; }
  set style(v) { this.style.cssText = v; }
  get textContent() { let s = ''; const walk = (n) => { for (const c of n._children) { if (c.nodeType === TEXT) s += c._data; else if (c.nodeType === ELEMENT) walk(c); } }; walk(this); return s; }
  set textContent(v) { this.replaceChildren(); if (v !== null && v !== '' && v !== undefined) this.appendChild(this._doc.createTextNode(String(v))); }
  get innerText() { return innerText(this); }
  set innerText(v) {
    this.replaceChildren();
    const parts = String(v ?? '').split(/\r\n|\r|\n/);
    parts.forEach((p, i) => { if (i) this.appendChild(this._doc.createElement('br')); if (p) this.appendChild(this._doc.createTextNode(p)); });
  }
  get outerText() { return this.innerText; }
  get innerHTML() { const out = []; if (this._local === 'template' && this._content) serializeChildren(this._content, out); else serializeChildren(this, out); return out.join(''); }
  set innerHTML(html) {
    const target = this._local === 'template' && this._content ? this._content : this;
    target.replaceChildren();
    target.appendChild(parseFragment(html === null ? '' : html, this));
  }
  get outerHTML() { const out = []; serializeNode(this, out); return out.join(''); }
  set outerHTML(html) {
    const p = this._parent;
    if (!p) return;
    if (p.nodeType === DOCUMENT) throw new DOMException('This element has no parent node.', 'NoModificationAllowedError');
    p.replaceChild(parseFragment(html, p.nodeType === ELEMENT ? p : this), this);
  }
  getAttribute(n) { n = normName(this, n); const a = this._attrs.find(x => x.name === n); return a ? a.value : null; }
  getAttributeNS(ns, n) { return this.getAttribute(n); }
  getAttributeNames() { return this._attrs.map(a => a.name); }
  getAttributeNode(n) { return this.attributes.getNamedItem(n); }
  hasAttribute(n) { n = normName(this, n); return this._attrs.some(x => x.name === n); }
  hasAttributeNS(ns, n) { return this.hasAttribute(n); }
  hasAttributes() { return this._attrs.length > 0; }
  setAttribute(n, v) {
    n = String(n);
    if (!/^[^\s"'>/=\x00-\x1f]+$/.test(n)) throw new DOMException(`'${n}' is not a valid attribute name.`, 'InvalidCharacterError');
    n = normName(this, n);
    v = String(v);
    const a = this._attrs.find(x => x.name === n);
    const old = a ? a.value : null;
    if (a) a.value = v; else this._attrs.push({ name: n, value: v });
    this._attrChanged(n, old, v);
  }
  setAttributeNS(ns, n, v) { this.setAttribute(n, v); }
  setAttributeNode(a) { this.setAttribute(a.name, a.value); return null; }
  removeAttribute(n) {
    n = normName(this, n);
    const i = this._attrs.findIndex(x => x.name === n);
    if (i < 0) return;
    const old = this._attrs[i].value;
    this._attrs.splice(i, 1);
    this._attrChanged(n, old, null);
  }
  removeAttributeNS(ns, n) { this.removeAttribute(n); }
  removeAttributeNode(a) { this.removeAttribute(a.name); return a; }
  toggleAttribute(n, force) {
    const has = this.hasAttribute(n);
    if (has && force !== true) { this.removeAttribute(n); return false; }
    if (!has && force !== false) { this.setAttribute(n, ''); return true; }
    return has;
  }
  _attrChanged(name, old, value) {
    if (name === 'style' && this._style && !this._styleSync) this._style._decls.splice(0, this._style._decls.length, ...parseDecls(value || ''));
    if (name === 'id' && this._doc) this._doc._ids = null;
    queueRecord('attributes', this, { attributeName: name, oldValue: old });
    touch();
    if (this._ce === 2) {
      const def = customElements._defs.get(this._local);
      if (def && def.observed.includes(name) && typeof this.attributeChangedCallback === 'function') {
        try { this.attributeChangedCallback(name, old, value); } catch (e) { reportError(e); }
      }
    }
  }
  matches(s) { return matchesList(this, parseSelector(s), this); }
  webkitMatchesSelector(s) { return this.matches(s); }
  msMatchesSelector(s) { return this.matches(s); }
  closest(s) { const l = parseSelector(s); for (let e = this; e && e.nodeType === ELEMENT; e = e._parent) if (matchesList(e, l, this)) return e; return null; }
  get previousElementSibling() { return siblingElement(this, -1); }
  get nextElementSibling() { return siblingElement(this, 1); }
  remove() { if (this._parent) this._parent.removeChild(this); }
  before(...n) { insertNodes(this._parent, this, n); }
  after(...n) { insertNodes(this._parent, this.nextSibling, n); }
  replaceWith(...n) { const p = this._parent; if (!p) return; const next = this.nextSibling; p.removeChild(this); insertNodes(p, next, n); }
  insertAdjacentElement(where, el) {
    switch (String(where).toLowerCase()) {
      case 'beforebegin': if (!this._parent) return null; this._parent.insertBefore(el, this); return el;
      case 'afterbegin': this.insertBefore(el, this.firstChild); return el;
      case 'beforeend': this.appendChild(el); return el;
      case 'afterend': if (!this._parent) return null; this._parent.insertBefore(el, this.nextSibling); return el;
    }
    throw new DOMException(`The value provided ('${where}') is not one of 'beforeBegin', 'afterBegin', 'beforeEnd', or 'afterEnd'.`, 'SyntaxError');
  }
  insertAdjacentHTML(where, html) {
    const w = String(where).toLowerCase();
    const ctx = w === 'beforebegin' || w === 'afterend' ? (this._parent && this._parent.nodeType === ELEMENT ? this._parent : this) : this;
    this.insertAdjacentElement(w, parseFragment(html, ctx));
  }
  insertAdjacentText(where, text) { this.insertAdjacentElement(where, this._doc.createTextNode(String(text))); }
  getBoundingClientRect() { return rectFor(this); }
  getClientRects() { const r = rectFor(this); return r.width || r.height ? [r] : []; }
  get clientWidth() { return rectFor(this).width; }
  get clientHeight() { return rectFor(this).height; }
  get clientTop() { return 0; }
  get clientLeft() { return 0; }
  get scrollWidth() { return rectFor(this).width; }
  get scrollHeight() { return rectFor(this).height; }
  get scrollTop() { return 0; }
  set scrollTop(v) {}
  get scrollLeft() { return 0; }
  set scrollLeft(v) {}
  scrollIntoView() {}
  scroll() {} scrollTo() {} scrollBy() {}
  get shadowRoot() { return this._shadow && this._shadow.mode === 'open' ? this._shadow : null; }
  attachShadow(init) {
    if (this._shadow) throw new DOMException('Shadow root cannot be created on a host which already hosts a shadow tree.', 'NotSupportedError');
    const s = new ShadowRoot();
    s._doc = this._doc; s._host = this; s.mode = init && init.mode === 'closed' ? 'closed' : 'open';
    hide(this, '_shadow', s);
    touch();
    return s;
  }
  animate() { const a = { finished: Promise.resolve(), cancel() {}, finish() {}, play() {}, pause() {}, reverse() {}, onfinish: null, playState: 'finished' }; a.ready = Promise.resolve(a); return a; }
  getAnimations() { return []; }
  requestFullscreen() { return Promise.reject(new DOMException('Fullscreen is not supported.', 'NotSupportedError')); }
  setPointerCapture() {} releasePointerCapture() {} hasPointerCapture() { return false; }
  checkVisibility() { return !hiddenByStyle(this); }
  _clone() {
    const e = createElementIn(this._doc, this._local, this._ns, []);
    e._prefix = this._prefix;
    for (const a of this._attrs) e._attrs.push({ name: a.name, value: a.value });
    if (this._cloneState) this._cloneState(e);
    return e;
  }
}
mix(Element, ParentNodeMixin);
Object.defineProperty(Element.prototype, Symbol.unscopables, { value: { after: true, append: true, before: true, prepend: true, remove: true, replaceWith: true, replaceChildren: true } });

function innerText(el) {
  if (hiddenByStyle(el)) return '';
  let s = '';
  const block = /^(address|article|aside|blockquote|dd|details|dialog|div|dl|dt|fieldset|figcaption|figure|footer|form|h[1-6]|header|hgroup|hr|li|main|nav|ol|p|pre|section|table|tr|ul)$/;
  const walk = (n) => {
    for (const c of n._children) {
      if (c.nodeType === TEXT) s += c._data.replace(/\s+/g, ' ');
      else if (c.nodeType === ELEMENT) {
        if (c._local === 'script' || c._local === 'style' || c._local === 'template' || hiddenByStyle(c)) continue;
        if (c._local === 'br') { s += '\n'; continue; }
        const isBlock = block.test(c._local);
        if (isBlock && s && !s.endsWith('\n')) s += '\n';
        walk(c);
        if (c._local === 'td' || c._local === 'th') s += '\t';
        if (isBlock && !s.endsWith('\n')) s += '\n';
      }
    }
  };
  if (el._local === 'pre' || el._local === 'textarea') return el.textContent;
  walk(el);
  return s.replace(/[ \t]+\n/g, '\n').replace(/\n{3,}/g, '\n\n').replace(/^\s+|\s+$/g, '');
}
function hiddenByStyle(el) {
  if (el.hasAttribute('hidden')) return true;
  const st = el.getAttribute('style');
  return !!st && /(^|;)\s*display\s*:\s*none/i.test(st);
}

// Geometry: the browser lays out in character cells; an element gets a box
// of its text's width, which is enough for code that only asks whether an
// element is visible or how big it is roughly.
function rectFor(el) {
  const [cols, , cw, ch] = viewport();
  let w = 0, h = 0;
  if (el.isConnected && !hiddenByStyle(el)) {
    const t = el.textContent || '';
    const lines = Math.max(1, Math.ceil(t.length / Math.max(cols, 1)));
    w = Math.min(t.length, cols) * cw || (el._local === 'img' ? +(el.getAttribute('width') || 0) : 0);
    h = t.length ? lines * ch : (el._local === 'img' ? +(el.getAttribute('height') || 0) : 0);
  }
  return new DOMRect(0, 0, w, h);
}
let vp = null, vpAt = -1;
function viewport() {
  if (vpAt !== renderDepth || !vp) { vp = b.viewport(); vpAt = renderDepth; }
  return vp;
}
class DOMRect {
  constructor(x = 0, y = 0, width = 0, height = 0) { Object.assign(this, { x, y, width, height }); }
  get top() { return Math.min(this.y, this.y + this.height); }
  get left() { return Math.min(this.x, this.x + this.width); }
  get right() { return Math.max(this.x, this.x + this.width); }
  get bottom() { return Math.max(this.y, this.y + this.height); }
  toJSON() { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, left: this.left, right: this.right, bottom: this.bottom }; }
  static fromRect(r = {}) { return new DOMRect(r.x, r.y, r.width, r.height); }
}
install('DOMRect', DOMRect);
install('DOMRectReadOnly', DOMRect);

// Event handler content attributes: onclick="..." on any element, compiled
// the first time its event fires.
const handlerAttrs = ['click', 'dblclick', 'mousedown', 'mouseup', 'mouseover', 'mouseout', 'mousemove', 'mouseenter', 'mouseleave', 'keydown', 'keyup', 'keypress', 'input', 'change', 'submit', 'reset', 'focus', 'blur', 'load', 'error', 'scroll', 'resize', 'contextmenu', 'wheel', 'pointerdown', 'pointerup', 'touchstart', 'touchend', 'toggle', 'invalid', 'select', 'animationend', 'transitionend', 'beforeunload', 'unload', 'hashchange', 'popstate', 'pagehide', 'pageshow', 'message', 'online', 'offline', 'storage'];
const kCompiled = Symbol('compiled');
function contentHandler(el, type) {
  const src = el.getAttribute && el.getAttribute('on' + type);
  if (src === null || src === undefined) return null;
  const cache = el[kCompiled] || hide(el, kCompiled, new Map()) && el[kCompiled];
  let h = cache.get(type);
  if (!h || h.src !== src) {
    let fn;
    try {
      // `with` gives the handler the element, its form and the document as
      // scopes, as browsers do; strict mode would forbid it, so Function.
      fn = new Function('event', `with (this.ownerDocument || this) { with (this.form || {}) { with (this) { ${src}\n} } }`);
    } catch (e) { reportError(e); fn = null; }
    h = { src, fn };
    cache.set(type, h);
  }
  return h.fn;
}
class HTMLElement extends Element {
  constructor() {
    super();
    const ce = customElements._defs && customElements._byClass.get(new.target);
    if (ce && !this._local) { this._local = ce.name; this._doc = g.document; this._ce = 2; }
  }
  get title() { return this.getAttribute('title') || ''; }
  set title(v) { this.setAttribute('title', v); }
  get lang() { return this.getAttribute('lang') || ''; }
  set lang(v) { this.setAttribute('lang', v); }
  get dir() { return this.getAttribute('dir') || ''; }
  set dir(v) { this.setAttribute('dir', v); }
  get hidden() { return this.hasAttribute('hidden'); }
  set hidden(v) { if (v) this.setAttribute('hidden', ''); else this.removeAttribute('hidden'); }
  get inert() { return this.hasAttribute('inert'); }
  set inert(v) { this.toggleAttribute('inert', !!v); }
  get accessKey() { return this.getAttribute('accesskey') || ''; }
  get tabIndex() { const v = parseInt(this.getAttribute('tabindex'), 10); return isNaN(v) ? (/^(a|button|input|select|textarea)$/.test(this._local) ? 0 : -1) : v; }
  set tabIndex(v) { this.setAttribute('tabindex', v); }
  get contentEditable() { return this.getAttribute('contenteditable') || 'inherit'; }
  set contentEditable(v) { this.setAttribute('contenteditable', v); }
  get isContentEditable() { return this.contentEditable === 'true' || this.contentEditable === ''; }
  get draggable() { return this.getAttribute('draggable') === 'true'; }
  set draggable(v) { this.setAttribute('draggable', v ? 'true' : 'false'); }
  get spellcheck() { return this.getAttribute('spellcheck') !== 'false'; }
  set spellcheck(v) { this.setAttribute('spellcheck', v ? 'true' : 'false'); }
  get translate() { return this.getAttribute('translate') !== 'no'; }
  get autofocus() { return this.hasAttribute('autofocus'); }
  get nonce() { return this.getAttribute('nonce') || ''; }
  get popover() { return this.getAttribute('popover'); }
  get offsetParent() { return this.isConnected ? this._doc.body : null; }
  get offsetTop() { return 0; }
  get offsetLeft() { return 0; }
  get offsetWidth() { return rectFor(this).width; }
  get offsetHeight() { return rectFor(this).height; }
  click() {
    if (this.disabled) return;
    const e = new MouseEvent('click', { bubbles: true, cancelable: true, composed: true });
    this.dispatchEvent(e);
  }
  focus() { if (this._doc) { const old = this._doc._active; if (old === this) return; this._doc._active = this; if (old && old !== this._doc.body) old.dispatchEvent(new FocusEvent('blur')); this.dispatchEvent(new FocusEvent('focus')); this.dispatchEvent(new FocusEvent('focusin', { bubbles: true })); } }
  blur() { if (this._doc && this._doc._active === this) { this._doc._active = null; this.dispatchEvent(new FocusEvent('blur')); this.dispatchEvent(new FocusEvent('focusout', { bubbles: true })); } }
  showPopover() {} hidePopover() {} togglePopover() {}
  get [Symbol.toStringTag]() { return elementClassName(this); }
}
for (const t of handlerAttrs) {
  // on<type> properties reflect handlers set from script; content attributes
  // are compiled lazily by the dispatch hook below.
  eventHandler(HTMLElement.prototype, t);
}

// The dispatch path runs content-attribute handlers: wrapped as a listener
// in front of every element's own when its attribute is present.
const origInvoke = EventTarget.prototype.dispatchEvent;
const kAttrListener = Symbol('attrListener');
function ensureAttrListener(el, type) {
  if (!el._attrs || !el.hasAttribute('on' + type)) return;
  let set = el[kAttrListener];
  if (!set) { set = new Set(); hide(el, kAttrListener, set); }
  if (set.has(type)) return;
  set.add(type);
  el.addEventListener(type, function (event) {
    const fn = contentHandler(this, type);
    if (!fn) return;
    const r = fn.call(this, event);
    if (r === false) event.preventDefault();
  });
}
EventTarget.prototype.dispatchEvent = function (event) {
  if (event && typeof event.type === 'string') {
    for (let n = this; n; n = n._parent) ensureAttrListener(n, event.type);
  }
  return origInvoke.call(this, event);
};

const elementClassName = (el) => {
  const C = tagClasses[el._local];
  return C ? C.name : el._local.includes('-') ? 'HTMLElement' : 'HTMLUnknownElement';
};

class HTMLUnknownElement extends HTMLElement {}
class HTMLHtmlElement extends HTMLElement { get version() { return ''; } }
class HTMLHeadElement extends HTMLElement {}
class HTMLBodyElement extends HTMLElement {}
for (const t of ['load', 'unload', 'beforeunload', 'hashchange', 'popstate', 'message', 'resize', 'scroll', 'storage', 'online', 'offline', 'pageshow', 'pagehide'])
  Object.defineProperty(HTMLBodyElement.prototype, 'on' + t, { configurable: true, get() { return g['on' + t]; }, set(v) { g['on' + t] = v; } });
class HTMLTitleElement extends HTMLElement { get text() { return this.textContent; } set text(v) { this.textContent = v; } }
class HTMLDivElement extends HTMLElement { get align() { return this.getAttribute('align') || ''; } }
class HTMLSpanElement extends HTMLElement {}
class HTMLParagraphElement extends HTMLElement {}
class HTMLHeadingElement extends HTMLElement {}
class HTMLPreElement extends HTMLElement {}
class HTMLBRElement extends HTMLElement {}
class HTMLHRElement extends HTMLElement {}
class HTMLUListElement extends HTMLElement {}
class HTMLOListElement extends HTMLElement { get start() { return +(this.getAttribute('start') || 1); } }
class HTMLLIElement extends HTMLElement { get value() { return +(this.getAttribute('value') || 0); } }
class HTMLDListElement extends HTMLElement {}
class HTMLQuoteElement extends HTMLElement {}
class HTMLModElement extends HTMLElement {}
class HTMLTimeElement extends HTMLElement { get dateTime() { return this.getAttribute('datetime') || ''; } }
class HTMLDataElement extends HTMLElement { get value() { return this.getAttribute('value') || ''; } }
class HTMLPictureElement extends HTMLElement {}
class HTMLSourceElement extends HTMLElement {}
class HTMLTrackElement extends HTMLElement {}
class HTMLMapElement extends HTMLElement {}
class HTMLAreaElement extends HTMLElement {}
class HTMLMenuElement extends HTMLElement {}
class HTMLSlotElement extends HTMLElement { assignedNodes() { return []; } assignedElements() { return []; } get name() { return this.getAttribute('name') || ''; } }
class HTMLDetailsElement extends HTMLElement {
  get open() { return this.hasAttribute('open'); }
  set open(v) { const was = this.open; this.toggleAttribute('open', !!v); if (was !== !!v) setTimeout(() => this.dispatchEvent(new Event('toggle'))); }
}
class HTMLDialogElement extends HTMLElement {
  get open() { return this.hasAttribute('open'); }
  set open(v) { this.toggleAttribute('open', !!v); }
  show() { this.open = true; }
  showModal() { this.open = true; }
  close(rv) { if (!this.open) return; if (rv !== undefined) this.returnValue = String(rv); this.open = false; this.dispatchEvent(new Event('close')); }
}
class HTMLTableElement extends HTMLElement {
  get rows() { const self = this; return liveCollection(() => descendants(self).filter(e => e._local === 'tr' && e.closest('table') === self)); }
  get tBodies() { const self = this; return liveCollection(() => (self._children || []).filter(e => e._local === 'tbody')); }
  get tHead() { return (this._children || []).find(e => e._local === 'thead') || null; }
  get tFoot() { return (this._children || []).find(e => e._local === 'tfoot') || null; }
  get caption() { return (this._children || []).find(e => e._local === 'caption') || null; }
  createTBody() { const t = this._doc.createElement('tbody'); this.appendChild(t); return t; }
  createTHead() { let t = this.tHead; if (!t) { t = this._doc.createElement('thead'); this.insertBefore(t, this.firstChild); } return t; }
  insertRow(i = -1) {
    let body = this.tBodies[this.tBodies.length - 1];
    if (!body) body = this.createTBody();
    return body.insertRow(i);
  }
  deleteRow(i) { const r = this.rows[i < 0 ? this.rows.length + i : i]; if (r) r.remove(); }
}
class HTMLTableSectionElement extends HTMLElement {
  get rows() { const self = this; return liveCollection(() => (self._children || []).filter(e => e._local === 'tr')); }
  insertRow(i = -1) { const r = this._doc.createElement('tr'); const rows = this.rows; if (i < 0 || i >= rows.length) this.appendChild(r); else this.insertBefore(r, rows[i]); return r; }
  deleteRow(i) { const r = this.rows[i]; if (r) r.remove(); }
}
class HTMLTableRowElement extends HTMLElement {
  get cells() { const self = this; return liveCollection(() => (self._children || []).filter(e => e._local === 'td' || e._local === 'th')); }
  get rowIndex() { const t = this.closest('table'); return t ? Array.from(t.rows).indexOf(this) : -1; }
  get sectionRowIndex() { return this._parent ? Array.from(this._parent.rows || []).indexOf(this) : -1; }
  insertCell(i = -1) { const c = this._doc.createElement('td'); const cells = this.cells; if (i < 0 || i >= cells.length) this.appendChild(c); else this.insertBefore(c, cells[i]); return c; }
  deleteCell(i) { const c = this.cells[i]; if (c) c.remove(); }
}
class HTMLTableCellElement extends HTMLElement {
  get cellIndex() { return this._parent ? Array.from(this._parent.cells || []).indexOf(this) : -1; }
  get colSpan() { return +(this.getAttribute('colspan') || 1); }
  set colSpan(v) { this.setAttribute('colspan', v); }
  get rowSpan() { return +(this.getAttribute('rowspan') || 1); }
  set rowSpan(v) { this.setAttribute('rowspan', v); }
}
class HTMLTableCaptionElement extends HTMLElement {}
class HTMLTableColElement extends HTMLElement {}

function reflectUrl(C, prop, attr = prop) {
  Object.defineProperty(C.prototype, prop, {
    configurable: true, enumerable: true,
    get() { const v = this.getAttribute(attr); if (v === null) return ''; try { return new URL(v, this._doc ? this._doc.baseURI : undefined).href; } catch (e) { return v; } },
    set(v) { this.setAttribute(attr, v); },
  });
}
function reflectStr(C, props) {
  for (const p of props) {
    const attr = p.toLowerCase();
    Object.defineProperty(C.prototype, p, { configurable: true, enumerable: true, get() { return this.getAttribute(attr) ?? ''; }, set(v) { this.setAttribute(attr, v); } });
  }
}
function reflectBool(C, props) {
  for (const p of props) {
    const attr = p.toLowerCase();
    Object.defineProperty(C.prototype, p, { configurable: true, enumerable: true, get() { return this.hasAttribute(attr); }, set(v) { this.toggleAttribute(attr, !!v); } });
  }
}

class HTMLAnchorElement extends HTMLElement {
  get text() { return this.textContent; }
  set text(v) { this.textContent = v; }
  get relList() { return tokenList(this, 'rel'); }
  toString() { return this.href; }
  _url() { try { return new URL(this.getAttribute('href') ?? '', this._doc ? this._doc.baseURI : undefined); } catch (e) { return null; } }
}
reflectUrl(HTMLAnchorElement, 'href');
reflectStr(HTMLAnchorElement, ['target', 'download', 'rel', 'hreflang', 'type', 'referrerPolicy', 'ping', 'name']);
for (const part of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin', 'username', 'password']) {
  Object.defineProperty(HTMLAnchorElement.prototype, part, {
    configurable: true,
    get() { const u = this._url(); return u ? u[part] : ''; },
    set(v) { const u = this._url(); if (u && part !== 'origin') { u[part] = v; this.setAttribute('href', u.href); } },
  });
}
{ const d = Object.getOwnPropertyDescriptors(HTMLAnchorElement.prototype); delete d.constructor; Object.defineProperties(HTMLAreaElement.prototype, d); }

class HTMLImageElement extends HTMLElement {
  get complete() { return true; }
  get naturalWidth() { return +(this.getAttribute('width') || 0); }
  get naturalHeight() { return +(this.getAttribute('height') || 0); }
  get width() { return +(this.getAttribute('width') || 0); }
  set width(v) { this.setAttribute('width', v); }
  get height() { return +(this.getAttribute('height') || 0); }
  set height(v) { this.setAttribute('height', v); }
  get currentSrc() { return this.src; }
  decode() { return Promise.resolve(); }
  _attrChanged(name, old, value) {
    super._attrChanged(name, old, value);
    // Nothing waits for pixels here: scripts that wait for "load" get it.
    if (name === 'src' && value) setTimeout(() => this.dispatchEvent(new Event('load')));
  }
}
reflectUrl(HTMLImageElement, 'src');
reflectStr(HTMLImageElement, ['alt', 'srcset', 'sizes', 'crossOrigin', 'useMap', 'loading', 'decoding', 'referrerPolicy', 'fetchPriority']);
reflectBool(HTMLImageElement, ['isMap']);
class HTMLCanvasElement extends HTMLElement {
  getContext() { return null; }
  toDataURL() { return 'data:,'; }
  toBlob(cb) { setTimeout(() => cb(null)); }
  get width() { return +(this.getAttribute('width') || 300); }
  set width(v) { this.setAttribute('width', v); }
  get height() { return +(this.getAttribute('height') || 150); }
  set height(v) { this.setAttribute('height', v); }
}
class HTMLMediaElement extends HTMLElement {
  play() { return Promise.reject(new DOMException('Media is not supported here.', 'NotSupportedError')); }
  pause() {} load() {} canPlayType() { return ''; }
  get paused() { return true; }
  get duration() { return NaN; }
}
reflectUrl(HTMLMediaElement, 'src');
class HTMLVideoElement extends HTMLMediaElement {}
class HTMLAudioElement extends HTMLMediaElement {}
class HTMLIFrameElement extends HTMLElement { get contentWindow() { return null; } get contentDocument() { return null; } }
reflectUrl(HTMLIFrameElement, 'src');
reflectStr(HTMLIFrameElement, ['name', 'width', 'height', 'allow', 'loading', 'srcdoc']);
class HTMLEmbedElement extends HTMLElement {}
class HTMLObjectElement extends HTMLElement {}
class HTMLMetaElement extends HTMLElement {}
reflectStr(HTMLMetaElement, ['name', 'content', 'httpEquiv', 'charset', 'media']);
class HTMLBaseElement extends HTMLElement {}
reflectUrl(HTMLBaseElement, 'href');
class HTMLLinkElement extends HTMLElement {
  get relList() { return tokenList(this, 'rel'); }
  get sheet() { return null; }
}
reflectUrl(HTMLLinkElement, 'href');
reflectStr(HTMLLinkElement, ['rel', 'type', 'media', 'as', 'crossOrigin', 'integrity', 'hreflang', 'sizes', 'referrerPolicy', 'fetchPriority']);
reflectBool(HTMLLinkElement, ['disabled']);
class HTMLStyleElement extends HTMLElement { get sheet() { return { cssRules: [], insertRule() { return 0; }, deleteRule() {} }; } }
reflectStr(HTMLStyleElement, ['media', 'type']);
class HTMLScriptElement extends HTMLElement {
  get text() { return this.textContent; }
  set text(v) { this.textContent = v; }
  get async() { return this._forceAsync !== false || this.hasAttribute('async'); }
  set async(v) { this._forceAsync = false; this.toggleAttribute('async', !!v); }
  static supports(t) { return t === 'classic' || t === 'module'; }
}
reflectUrl(HTMLScriptElement, 'src');
reflectStr(HTMLScriptElement, ['type', 'charset', 'crossOrigin', 'integrity', 'referrerPolicy', 'event', 'htmlFor', 'fetchPriority']);
reflectBool(HTMLScriptElement, ['defer', 'noModule']);
class HTMLTemplateElement extends HTMLElement {
  get content() { if (!this._content) { const f = new DocumentFragment(); f._doc = this._doc; hide(this, '_content', f); } return this._content; }
}
class HTMLLabelElement extends HTMLElement {
  get control() { const f = this.getAttribute('for'); if (f) return this._doc.getElementById(f); return this.querySelector('input,select,textarea,button'); }
  get form() { const c = this.control; return c ? c.form : null; }
}
reflectStr(HTMLLabelElement, ['htmlFor']);
Object.defineProperty(HTMLLabelElement.prototype, 'htmlFor', { configurable: true, get() { return this.getAttribute('for') || ''; }, set(v) { this.setAttribute('for', v); } });
class HTMLFieldSetElement extends HTMLElement {
  get elements() { const self = this; return liveCollection(() => descendants(self).filter(isListed)); }
  get form() { return formOf(this); }
}
reflectBool(HTMLFieldSetElement, ['disabled']);
class HTMLLegendElement extends HTMLElement {}
class HTMLOutputElement extends HTMLElement { get value() { return this.textContent; } set value(v) { this.textContent = v; } }
class HTMLProgressElement extends HTMLElement {
  get value() { return +(this.getAttribute('value') || 0); } set value(v) { this.setAttribute('value', v); }
  get max() { return +(this.getAttribute('max') || 1); } set max(v) { this.setAttribute('max', v); }
}
class HTMLMeterElement extends HTMLProgressElement {}

// ── forms ─────────────────────────────────────────────────────────────────

const isListed = (e) => /^(input|button|select|textarea|fieldset|object|output)$/.test(e._local);
function formOf(el) {
  const id = el.getAttribute('form');
  if (id && el._doc) { const f = el._doc.getElementById(id); if (f && f._local === 'form') return f; }
  return el.closest ? el.closest('form') : null;
}
class HTMLFormElement extends HTMLElement {
  get elements() { const self = this; return liveCollection(() => descendants(self._doc || self).filter(e => isListed(e) && formOf(e) === self)); }
  get length() { return this.elements.length; }
  submit() { submitForm(this, null, false); }
  requestSubmit(submitter) { submitForm(this, submitter || null, true); }
  reset() {
    if (!this.dispatchEvent(new Event('reset', { bubbles: true, cancelable: true }))) return;
    for (const e of this.elements) if (e._reset) e._reset();
    touch();
  }
  checkValidity() { return true; }
  reportValidity() { return true; }
  _formData() { return formEntries(this, null); }
  get action() { const a = this.getAttribute('action'); try { return new URL(a || '', this._doc.baseURI).href; } catch (e) { return a || ''; } }
  set action(v) { this.setAttribute('action', v); }
  get method() { const m = (this.getAttribute('method') || '').toLowerCase(); return m === 'post' || m === 'dialog' ? m : 'get'; }
  set method(v) { this.setAttribute('method', v); }
}
reflectStr(HTMLFormElement, ['name', 'target', 'enctype', 'acceptCharset', 'autocomplete', 'rel']);
reflectBool(HTMLFormElement, ['noValidate']);
function formEntries(form, submitter) {
  const out = [];
  for (const e of form.elements) {
    if (e.disabled || !e.name) continue;
    const t = e._local === 'input' ? e.type : e._local;
    if (t === 'submit' || t === 'button' || t === 'reset' || t === 'image' || e._local === 'button') { if (e === submitter) out.push([e.name, e.value]); continue; }
    if ((t === 'checkbox' || t === 'radio') && !e.checked) continue;
    if (t === 'file') continue;
    if (e._local === 'select') { for (const o of e.options) if (o.selected && !o.disabled) out.push([e.name, o.value]); continue; }
    if (e._local === 'fieldset' || e._local === 'object' || e._local === 'output') continue;
    out.push([e.name, e.value]);
  }
  return out;
}
// What happens when a form is sent: the submit event, then the browser's own
// form submission, with the entries this DOM has (it knows what was typed).
function submitForm(form, submitter, events) {
  if (events) {
    const ev = new SubmitEvent('submit', { bubbles: true, cancelable: true, submitter });
    if (!form.dispatchEvent(ev)) return;
  }
  const action = submitter && submitter.hasAttribute('formaction') ? new URL(submitter.getAttribute('formaction'), document.baseURI).href : form.action;
  const method = (submitter && submitter.getAttribute('formmethod') || form.method).toLowerCase();
  if (method === 'dialog') { const d = form.closest('dialog'); if (d) d.close(submitter ? submitter.value : undefined); return; }
  const data = new URLSearchParams(formEntries(form, submitter)).toString();
  bridge.pendingSubmit = { action, method, data };
  b.submit(action, method, data);
}

function valueProp(C, extra) {
  Object.defineProperty(C.prototype, 'value', {
    configurable: true, enumerable: true,
    get() { return this._value !== undefined ? this._value : this._defaultValue(); },
    set(v) { v = v === null && this._local !== 'select' ? '' : String(v); const was = this.value; this._value = v; if (extra) extra.call(this, v); if (was !== v) touch(); },
  });
}
class HTMLInputElement extends HTMLElement {
  get type() { const t = (this.getAttribute('type') || 'text').toLowerCase(); return /^(hidden|text|search|tel|url|email|password|date|month|week|time|datetime-local|number|range|color|checkbox|radio|file|submit|image|reset|button)$/.test(t) ? t : 'text'; }
  set type(v) { this.setAttribute('type', v); }
  _defaultValue() { const v = this.getAttribute('value'); return v !== null ? v : (this.type === 'checkbox' || this.type === 'radio' ? 'on' : ''); }
  get defaultValue() { return this.getAttribute('value') || ''; }
  set defaultValue(v) { this.setAttribute('value', v); }
  get checked() { return this._checked !== undefined ? this._checked : this.hasAttribute('checked'); }
  set checked(v) {
    v = !!v;
    if (v === this.checked) { this._checked = v; return; }
    this._checked = v;
    if (v && this.type === 'radio' && this.name) {
      const root = this.form || this.getRootNode();
      for (const r of (root.querySelectorAll ? root.querySelectorAll('input[type=radio]') : []))
        if (r !== this && r.name === this.name && r.form === this.form) r._checked = false;
    }
    touch();
  }
  get defaultChecked() { return this.hasAttribute('checked'); }
  set defaultChecked(v) { this.toggleAttribute('checked', !!v); }
  get indeterminate() { return !!this._indeterminate; }
  set indeterminate(v) { this._indeterminate = !!v; }
  get form() { return formOf(this); }
  get files() { return []; }
  get valueAsNumber() { const n = parseFloat(this.value); return isNaN(n) ? NaN : n; }
  set valueAsNumber(n) { this.value = String(n); }
  get valueAsDate() { const d = new Date(this.value); return isNaN(d) ? null : d; }
  get selectionStart() { return this.value.length; }
  get selectionEnd() { return this.value.length; }
  set selectionStart(v) {} set selectionEnd(v) {}
  setSelectionRange() {} setRangeText() {} select() {}
  stepUp(n = 1) { this.valueAsNumber = (this.valueAsNumber || 0) + n * (+this.getAttribute('step') || 1); }
  stepDown(n = 1) { this.stepUp(-n); }
  checkValidity() { return true; }
  reportValidity() { return true; }
  setCustomValidity() {}
  get validity() { return { valid: true, valueMissing: false, typeMismatch: false, patternMismatch: false, tooLong: false, tooShort: false, rangeUnderflow: false, rangeOverflow: false, stepMismatch: false, badInput: false, customError: false }; }
  get validationMessage() { return ''; }
  get willValidate() { return true; }
  get labels() { const id = this.id; return nodeList(id && this._doc ? this._doc.querySelectorAll(`label[for="${id}"]`)._a : []); }
  showPicker() {}
  _reset() { this._value = undefined; this._checked = undefined; }
  _cloneState(e) { e._value = this._value; e._checked = this._checked; }
  [kActivation](event) {
    if (event.type !== 'click') return;
    const t = this.type;
    if ((t === 'submit' || t === 'image') && this.form) submitForm(this.form, this, true);
    else if (t === 'reset' && this.form) this.form.reset();
  }
}
valueProp(HTMLInputElement);
reflectStr(HTMLInputElement, ['name', 'placeholder', 'accept', 'alt', 'autocomplete', 'dirName', 'formAction', 'formEnctype', 'formMethod', 'formTarget', 'inputMode', 'max', 'min', 'pattern', 'step', 'list', 'enterKeyHint']);
reflectBool(HTMLInputElement, ['disabled', 'readOnly', 'required', 'multiple', 'autofocus', 'formNoValidate']);
Object.defineProperty(HTMLInputElement.prototype, 'maxLength', { get() { const v = parseInt(this.getAttribute('maxlength'), 10); return isNaN(v) ? -1 : v; }, set(v) { this.setAttribute('maxlength', v); } });
Object.defineProperty(HTMLInputElement.prototype, 'size', { get() { return +(this.getAttribute('size') || 20); }, set(v) { this.setAttribute('size', v); } });

class HTMLButtonElement extends HTMLElement {
  get type() { const t = (this.getAttribute('type') || '').toLowerCase(); return t === 'reset' || t === 'button' ? t : 'submit'; }
  set type(v) { this.setAttribute('type', v); }
  get value() { return this.getAttribute('value') || ''; }
  set value(v) { this.setAttribute('value', v); }
  get form() { return formOf(this); }
  checkValidity() { return true; }
  setCustomValidity() {}
  get labels() { return nodeList([]); }
  [kActivation](event) {
    if (event.type !== 'click' || this.disabled) return;
    if (this.type === 'submit' && this.form) submitForm(this.form, this, true);
    else if (this.type === 'reset' && this.form) this.form.reset();
  }
}
reflectStr(HTMLButtonElement, ['name', 'formAction', 'formMethod', 'formTarget', 'popoverTargetAction']);
reflectBool(HTMLButtonElement, ['disabled', 'autofocus', 'formNoValidate']);

class HTMLTextAreaElement extends HTMLElement {
  get type() { return 'textarea'; }
  _defaultValue() { return this.textContent; }
  get defaultValue() { return this.textContent; }
  set defaultValue(v) { this.textContent = v; }
  get form() { return formOf(this); }
  get textLength() { return this.value.length; }
  get selectionStart() { return this.value.length; }
  get selectionEnd() { return this.value.length; }
  set selectionStart(v) {} set selectionEnd(v) {}
  setSelectionRange() {} select() {} setRangeText() {}
  checkValidity() { return true; }
  setCustomValidity() {}
  get labels() { return nodeList([]); }
  _reset() { this._value = undefined; }
  _cloneState(e) { e._value = this._value; }
}
valueProp(HTMLTextAreaElement);
reflectStr(HTMLTextAreaElement, ['name', 'placeholder', 'wrap', 'autocomplete', 'dirName', 'inputMode']);
reflectBool(HTMLTextAreaElement, ['disabled', 'readOnly', 'required', 'autofocus']);
Object.defineProperty(HTMLTextAreaElement.prototype, 'rows', { get() { return +(this.getAttribute('rows') || 2); }, set(v) { this.setAttribute('rows', v); } });
Object.defineProperty(HTMLTextAreaElement.prototype, 'cols', { get() { return +(this.getAttribute('cols') || 20); }, set(v) { this.setAttribute('cols', v); } });

class HTMLSelectElement extends HTMLElement {
  get type() { return this.multiple ? 'select-multiple' : 'select-one'; }
  get options() {
    const self = this;
    const c = liveCollection(() => descendants(self).filter(e => e._local === 'option'));
    return new Proxy(c, {
      get(t, k, r) {
        if (k === 'add') return (o, before) => self.add(o, before);
        if (k === 'remove') return (i) => self.remove(i);
        if (k === 'selectedIndex') return self.selectedIndex;
        return Reflect.get(t, k, r);
      },
      set(t, k, v) {
        if (k === 'length') { const l = self.options; while (l.length > v) l[l.length - 1].remove(); return true; }
        if (k === 'selectedIndex') { self.selectedIndex = v; return true; }
        if (typeof k === 'string' && /^[0-9]+$/.test(k)) { const old = self.options[+k]; if (old) old.replaceWith(v); else self.appendChild(v); return true; }
        return Reflect.set(t, k, v);
      },
    });
  }
  get length() { return this.options.length; }
  set length(v) { this.options.length = v; }
  item(i) { return this.options[i] || null; }
  namedItem(n) { return this.options.namedItem(n); }
  add(o, before) { if (typeof before === 'number') before = this.options[before]; this.insertBefore(o, before || null); }
  remove(i) { if (i === undefined) { Element.prototype.remove.call(this); return; } const o = this.options[i]; if (o) o.remove(); }
  get selectedOptions() { const self = this; return liveCollection(() => Array.from(self.options).filter(o => o.selected)); }
  get selectedIndex() { const o = Array.from(this.options); const i = o.findIndex(x => x.selected); return i; }
  set selectedIndex(i) { const o = Array.from(this.options); o.forEach((x, k) => { x._selected = k === +i; }); touch(); }
  get value() { const o = Array.from(this.options).find(x => x.selected); return o ? o.value : ''; }
  set value(v) { const o = Array.from(this.options); let hit = false; for (const x of o) { x._selected = !hit && x.value === String(v); if (x._selected) hit = true; } touch(); }
  get form() { return formOf(this); }
  checkValidity() { return true; }
  setCustomValidity() {}
  get labels() { return nodeList([]); }
  _reset() { for (const o of this.options) o._selected = undefined; }
}
reflectStr(HTMLSelectElement, ['name', 'autocomplete']);
reflectBool(HTMLSelectElement, ['disabled', 'multiple', 'required', 'autofocus']);
Object.defineProperty(HTMLSelectElement.prototype, 'size', { get() { return +(this.getAttribute('size') || 0); }, set(v) { this.setAttribute('size', v); } });
class HTMLOptionElement extends HTMLElement {
  get value() { const v = this.getAttribute('value'); return v !== null ? v : this.text; }
  set value(v) { this.setAttribute('value', v); }
  get text() { return this.textContent.replace(/\s+/g, ' ').trim(); }
  set text(v) { this.textContent = v; }
  get label() { return this.getAttribute('label') || this.text; }
  set label(v) { this.setAttribute('label', v); }
  get selected() {
    if (this._selected !== undefined) return this._selected;
    if (this.hasAttribute('selected')) return true;
    const sel = this.closest('select');
    if (!sel || sel.multiple) return false;
    const opts = Array.from(sel.options);
    if (opts.some(o => o._selected === true || (o._selected === undefined && o.hasAttribute('selected')))) return false;
    return opts.find(o => !o.disabled) === this;
  }
  set selected(v) {
    const sel = this.closest('select');
    if (v && sel && !sel.multiple) for (const o of sel.options) o._selected = false;
    this._selected = !!v;
    touch();
  }
  get defaultSelected() { return this.hasAttribute('selected'); }
  set defaultSelected(v) { this.toggleAttribute('selected', !!v); }
  get index() { const s = this.closest('select'); return s ? Array.from(s.options).indexOf(this) : 0; }
  get form() { const s = this.closest('select'); return s ? s.form : null; }
}
reflectBool(HTMLOptionElement, ['disabled']);
class HTMLOptGroupElement extends HTMLElement {}
reflectStr(HTMLOptGroupElement, ['label']);
reflectBool(HTMLOptGroupElement, ['disabled']);
class HTMLDataListElement extends HTMLElement { get options() { const self = this; return liveCollection(() => descendants(self).filter(e => e._local === 'option')); } }

const tagClasses = {
  html: HTMLHtmlElement, head: HTMLHeadElement, body: HTMLBodyElement, title: HTMLTitleElement, div: HTMLDivElement, span: HTMLSpanElement,
  p: HTMLParagraphElement, h1: HTMLHeadingElement, h2: HTMLHeadingElement, h3: HTMLHeadingElement, h4: HTMLHeadingElement, h5: HTMLHeadingElement, h6: HTMLHeadingElement,
  pre: HTMLPreElement, listing: HTMLPreElement, xmp: HTMLPreElement, br: HTMLBRElement, hr: HTMLHRElement, ul: HTMLUListElement, ol: HTMLOListElement, li: HTMLLIElement, dl: HTMLDListElement,
  blockquote: HTMLQuoteElement, q: HTMLQuoteElement, ins: HTMLModElement, del: HTMLModElement, time: HTMLTimeElement, data: HTMLDataElement,
  a: HTMLAnchorElement, area: HTMLAreaElement, img: HTMLImageElement, picture: HTMLPictureElement, source: HTMLSourceElement, track: HTMLTrackElement, map: HTMLMapElement,
  canvas: HTMLCanvasElement, video: HTMLVideoElement, audio: HTMLAudioElement, iframe: HTMLIFrameElement, embed: HTMLEmbedElement, object: HTMLObjectElement,
  meta: HTMLMetaElement, base: HTMLBaseElement, link: HTMLLinkElement, style: HTMLStyleElement, script: HTMLScriptElement, template: HTMLTemplateElement, slot: HTMLSlotElement,
  form: HTMLFormElement, input: HTMLInputElement, button: HTMLButtonElement, textarea: HTMLTextAreaElement, select: HTMLSelectElement, option: HTMLOptionElement,
  optgroup: HTMLOptGroupElement, datalist: HTMLDataListElement, label: HTMLLabelElement, fieldset: HTMLFieldSetElement, legend: HTMLLegendElement,
  output: HTMLOutputElement, progress: HTMLProgressElement, meter: HTMLMeterElement, details: HTMLDetailsElement, dialog: HTMLDialogElement, menu: HTMLMenuElement,
  table: HTMLTableElement, thead: HTMLTableSectionElement, tbody: HTMLTableSectionElement, tfoot: HTMLTableSectionElement, tr: HTMLTableRowElement,
  td: HTMLTableCellElement, th: HTMLTableCellElement, caption: HTMLTableCaptionElement, col: HTMLTableColElement, colgroup: HTMLTableColElement,
};
const knownTags = new Set([...Object.keys(tagClasses), 'abbr', 'address', 'article', 'aside', 'b', 'bdi', 'bdo', 'big', 'center', 'cite', 'code', 'dd', 'dfn', 'dt', 'em', 'figcaption', 'figure', 'font', 'footer', 'header', 'hgroup', 'i', 'kbd', 'main', 'mark', 'nav', 'nobr', 'noscript', 'rp', 'rt', 'ruby', 's', 'samp', 'search', 'section', 'small', 'strike', 'strong', 'sub', 'summary', 'sup', 'tt', 'u', 'var', 'wbr', 'acronym', 'noembed', 'noframes', 'plaintext', 'marquee', 'frameset', 'frame', 'param', 'math', 'svg']);

class SVGElement extends Element {
  get className() { return { baseVal: this.getAttribute('class') || '', animVal: this.getAttribute('class') || '' }; }
  get ownerSVGElement() { return this.closest('svg'); }
  getBBox() { return new DOMRect(); }
  focus() {} blur() {}
}
for (const t of handlerAttrs) eventHandler(SVGElement.prototype, t);
class SVGSVGElement extends SVGElement { createSVGPoint() { return { x: 0, y: 0, matrixTransform() { return this; } }; } }
class MathMLElement extends Element {}

// What controls and scripts keep besides their attributes, out of sight of
// for-in and JSON.stringify.
function elementState(el) {
  for (const k of ['_value', '_checked', '_selected', '_indeterminate', '_content']) hide(el, k, undefined);
  hide(el, '_started', false); hide(el, '_styleSync', false); hide(el, '_forceAsync', undefined);
}
function createElementIn(doc, name, ns, attrs) {
  let el;
  const local = ns === HTML_NS ? String(name).toLowerCase() : String(name);
  if (ns === HTML_NS) {
    const C = tagClasses[local] || (knownTags.has(local) || local.includes('-') ? HTMLElement : HTMLUnknownElement);
    el = Object.create(C.prototype);
  } else if (ns === SVG_NS) el = Object.create((local === 'svg' ? SVGSVGElement : SVGElement).prototype);
  else el = Object.create((ns === MATH_NS ? MathMLElement : Element).prototype);
  // Initialise as the constructors would, without running a custom element's.
  hide(el, kListeners, null);
  hide(el, '_parent', null); hide(el, '_children', []); hide(el, '_doc', doc); hide(el, '_uid', nextNodeId++);
  hide(el, '_ns', ns); hide(el, '_local', local); hide(el, '_prefix', null);
  hide(el, '_attrs', []); hide(el, '_classList', null); hide(el, '_style', null); hide(el, '_dataset', null);
  hide(el, '_ce', local.includes('-') && ns === HTML_NS ? 1 : 0);
  elementState(el);
  if (attrs) for (let i = 0; i + 1 < attrs.length; i += 2) if (!el._attrs.some(a => a.name === attrs[i])) el._attrs.push({ name: attrs[i], value: attrs[i + 1] });
  if (local === 'script' && ns === HTML_NS) el._forceAsync = true;
  return el;
}

// ── custom elements ───────────────────────────────────────────────────────

const customElements = {
  _defs: new Map(), _byClass: new Map(), _waiting: new Map(),
  define(name, C, options) {
    name = String(name);
    if (!/^[a-z][-.0-9_a-z\u00b7\u00c0-\uffff]*-[-.0-9_a-z\u00b7\u00c0-\uffff]*$/.test(name)) throw new DOMException(`'${name}' is not a valid custom element name`, 'SyntaxError');
    if (this._defs.has(name)) throw new DOMException(`the name "${name}" has already been used with this registry`, 'NotSupportedError');
    if (typeof C !== 'function') throw new TypeError('The provided value is not a constructor');
    const observed = Array.isArray(C.observedAttributes) ? C.observedAttributes.map(String) : [];
    const def = { name, C, observed, ext: options && options.extends };
    this._defs.set(name, def);
    this._byClass.set(C, def);
    if (g.document) for (const el of descendants(g.document).filter(e => e._local === name && e._ce === 1)) upgrade(el, def);
    const w = this._waiting.get(name);
    if (w) { this._waiting.delete(name); w.forEach(r => r(C)); }
  },
  get(name) { const d = this._defs.get(String(name)); return d ? d.C : undefined; },
  getName(C) { const d = this._byClass.get(C); return d ? d.name : null; },
  whenDefined(name) {
    const d = this._defs.get(String(name));
    if (d) return Promise.resolve(d.C);
    return new Promise(r => { const l = this._waiting.get(name) || []; l.push(r); this._waiting.set(name, l); });
  },
  upgrade(root) { for (const el of [root, ...descendants(root)]) if (el._ce === 1) { const d = this._defs.get(el._local); if (d) upgrade(el, d); } },
};
function upgrade(el, def) {
  if (el._ce === 2) return;
  Object.setPrototypeOf(el, def.C.prototype);
  pendingUpgrade = el;
  try { const r = Reflect.construct(def.C, []); if (r !== el) throw new TypeError('A custom element constructor must not return a different object.'); }
  catch (e) { reportError(e); pendingUpgrade = null; return; }
  pendingUpgrade = null;
  el._ce = 2;
  if (typeof el.attributeChangedCallback === 'function')
    for (const a of el._attrs.slice()) if (def.observed.includes(a.name)) { try { el.attributeChangedCallback(a.name, null, a.value); } catch (e) { reportError(e); } }
  if (el.isConnected && typeof el.connectedCallback === 'function') { try { el.connectedCallback(); } catch (e) { reportError(e); } }
}
install('customElements', customElements);

function connected(n) {
  if (n.nodeType !== ELEMENT) return;
  const run = (el) => {
    if (el._ce === 1) { const d = customElements._defs.get(el._local); if (d) { upgrade(el, d); } }
    else if (el._ce === 2 && typeof el.connectedCallback === 'function') { try { el.connectedCallback(); } catch (e) { reportError(e); } }
    if (el._local === 'script' && el._ns === HTML_NS && !el._started && !parsing) startScript(el);
  };
  run(n);
  for (const d of descendants(n)) run(d);
}
function disconnected(n) {
  if (n.nodeType !== ELEMENT) return;
  if (n._doc && n._doc._active && n.contains(n._doc._active)) n._doc._active = null;
  for (const el of [n, ...descendants(n)])
    if (el._ce === 2 && typeof el.disconnectedCallback === 'function') { try { el.disconnectedCallback(); } catch (e) { reportError(e); } }
  if (n._doc) n._doc._ids = null;
}

// Scripts added after the page was parsed run when they are connected:
// inline ones at once, external ones when the browser has fetched them.
let parsing = true;
const pendingScripts = new Map();
let nextScriptId = 1;
function scriptKind(el) {
  const t = (el.getAttribute('type') || '').trim().toLowerCase();
  if (!t || /^(text|application)\/(x-)?(java|ecma)script$/.test(t) || t === 'text/jscript' || t === 'text/livescript') return 'classic';
  if (t === 'module') return 'module';
  return null;
}
function startScript(el) {
  el._started = true;
  const kind = scriptKind(el);
  if (!kind) return;
  if (el.hasAttribute('src')) {
    const id = nextScriptId++;
    pendingScripts.set(id, el);
    b.loadScript(el.src, id, kind === 'module');
  } else {
    const prev = document._current;
    document._current = kind === 'module' ? null : el;
    b.evalScript(el.textContent, document.URL, kind === 'module');
    document._current = prev;
  }
}

// ── documents ─────────────────────────────────────────────────────────────

class DocumentFragment extends Node {
  constructor() { super(); this._children = []; this._doc = g.document || null; }
  get nodeType() { return FRAGMENT; }
  get nodeName() { return '#document-fragment'; }
  get textContent() { let s = ''; for (const c of this._children) if (c.nodeType !== COMMENT) s += c.textContent || ''; return s; }
  set textContent(v) { this.replaceChildren(); if (v) this.appendChild(this._doc.createTextNode(String(v))); }
  getElementById(id) { return descendants(this).find(e => e.id === id) || null; }
  _clone() { const f = new DocumentFragment(); f._doc = this._doc; return f; }
}
mix(DocumentFragment, ParentNodeMixin);
class ShadowRoot extends DocumentFragment {
  get host() { return this._host; }
  get innerHTML() { const out = []; serializeChildren(this, out); return out.join(''); }
  set innerHTML(h) { this.replaceChildren(); this.appendChild(parseFragment(h, this._host)); }
  get activeElement() { return null; }
  get adoptedStyleSheets() { return []; }
  set adoptedStyleSheets(v) {}
}

class Document extends Node {
  constructor() {
    super();
    this._children = [];
    hide(this, '_doc', this);
    hide(this, '_url', 'about:blank');
    hide(this, '_ids', null);
    hide(this, '_active', null);
    hide(this, '_current', null);
    hide(this, '_ready', 'complete');
    hide(this, '_cookies', null);
    hide(this, '_xml', false);
  }
  get nodeType() { return DOCUMENT; }
  get nodeName() { return '#document'; }
  get documentElement() { return this._children.find(c => c.nodeType === ELEMENT) || null; }
  get doctype() { return this._children.find(c => c.nodeType === DOCTYPE) || null; }
  get head() { const h = this.documentElement; return h ? (h._children.find(c => c._local === 'head') || null) : null; }
  get body() { const h = this.documentElement; return h ? (h._children.find(c => c._local === 'body' || c._local === 'frameset') || null) : null; }
  set body(el) { const h = this.documentElement; const old = this.body; if (old) h.replaceChild(el, old); else h.appendChild(el); }
  get title() { const t = this.querySelector('title'); return t ? t.textContent.replace(/\s+/g, ' ').trim() : ''; }
  set title(v) {
    let t = this.querySelector('title');
    if (!t) { t = this.createElement('title'); (this.head || this.documentElement || this).appendChild(t); }
    t.textContent = v;
  }
  get URL() { return this._url; }
  get documentURI() { return this._url; }
  get baseURI() { const base = this.querySelector('base[href]'); if (base) { try { return new URL(base.getAttribute('href'), this._url).href; } catch (e) {} } return this._url; }
  get location() { return this === g.document ? location : null; }
  set location(v) { location.href = v; }
  get domain() { try { return new URL(this._url).hostname; } catch (e) { return ''; } }
  get referrer() { return ''; }
  get readyState() { return this._ready; }
  get currentScript() { return this._current; }
  get activeElement() { return this._active && this._active.isConnected ? this._active : this.body; }
  get defaultView() { return this === g.document ? g : null; }
  get characterSet() { return 'UTF-8'; }
  get charset() { return 'UTF-8'; }
  get inputEncoding() { return 'UTF-8'; }
  get contentType() { return this._xml ? 'application/xml' : 'text/html'; }
  get compatMode() { return this.doctype ? 'CSS1Compat' : 'BackCompat'; }
  get visibilityState() { return 'visible'; }
  get hidden() { return false; }
  get designMode() { return 'off'; }
  set designMode(v) {}
  get dir() { return this.documentElement ? this.documentElement.dir : ''; }
  get lastModified() { const d = new Date(); return d.toISOString(); }
  get cookie() { return cookieString(this); }
  set cookie(v) { setCookie(this, String(v)); }
  get implementation() { return implementation; }
  get forms() { const self = this; return liveCollection(() => byTag(self, 'form')); }
  get images() { const self = this; return liveCollection(() => byTag(self, 'img')); }
  get links() { const self = this; return liveCollection(() => descendants(self).filter(e => (e._local === 'a' || e._local === 'area') && e.hasAttribute('href'))); }
  get scripts() { const self = this; return liveCollection(() => byTag(self, 'script')); }
  get embeds() { const self = this; return liveCollection(() => byTag(self, 'embed')); }
  get plugins() { return this.embeds; }
  get anchors() { const self = this; return liveCollection(() => byTag(self, 'a').filter(a => a.hasAttribute('name'))); }
  get all() { const self = this; return liveCollection(() => descendants(self)); }
  get styleSheets() { return []; }
  get fonts() { return { ready: Promise.resolve(), check: () => true, load: () => Promise.resolve([]), add() {}, forEach() {}, status: 'loaded' }; }
  get scrollingElement() { return this.documentElement; }
  get fullscreenElement() { return null; }
  get pointerLockElement() { return null; }
  get timeline() { return { currentTime: performance.now() }; }
  getElementById(id) {
    id = String(id);
    if (!this._ids) {
      const m = new Map();
      for (const e of descendants(this)) { const v = e.getAttribute('id'); if (v && !m.has(v)) m.set(v, e); }
      this._ids = m;
    }
    const e = this._ids.get(id);
    if (e && e.isConnected && e.getAttribute('id') === id) return e;
    this._ids = null;
    return descendants(this).find(x => x.getAttribute('id') === id) || null;
  }
  getElementsByName(n) { const self = this; return liveCollection(() => descendants(self).filter(e => e.getAttribute('name') === String(n))); }
  createElement(name, options) {
    name = String(name);
    if (!/^[A-Za-z][^\s/>\x00]*$/.test(name)) throw new DOMException(`The tag name provided ('${name}') is not a valid name.`, 'InvalidCharacterError');
    const is = options && typeof options === 'object' ? options.is : null;
    const local = name.toLowerCase();
    const def = customElements._defs.get(is || local);
    if (def && !is) {
      const el = new def.C();
      el._doc = this;
      el._local = local;
      el._ce = 2;
      return el;
    }
    const el = createElementIn(this, name, HTML_NS, null);
    if (is) el.setAttribute('is', is);
    return el;
  }
  createElementNS(ns, qname) {
    ns = ns === null ? null : String(ns);
    const [prefix, local] = String(qname).includes(':') ? String(qname).split(':') : [null, String(qname)];
    if (ns === HTML_NS) return this.createElement(local);
    const el = createElementIn(this, local, ns, null);
    el._prefix = prefix;
    return el;
  }
  createTextNode(s) { return makeText(String(s), this); }
  createComment(s) { return makeComment(String(s), this); }
  createCDATASection(s) { const t = new CDATASection(String(s)); t._doc = this; return t; }
  createProcessingInstruction(target, data) { const p = new ProcessingInstruction(data); p.target = target; p._doc = this; return p; }
  createDocumentFragment() { const f = new DocumentFragment(); f._doc = this; return f; }
  createAttribute(n) { return new Attr(String(n).toLowerCase(), ''); }
  createEvent(type) {
    const t = String(type).toLowerCase();
    const C = { event: Event, events: Event, htmlevents: Event, customevent: CustomEvent, mouseevent: MouseEvent, mouseevents: MouseEvent, uievent: UIEvent, uievents: UIEvent, keyboardevent: KeyboardEvent, messageevent: MessageEvent, focusevent: FocusEvent }[t];
    if (!C) throw new DOMException(`The provided event type ('${type}') is invalid.`, 'NotSupportedError');
    const e = new C('');
    e.initMouseEvent = e.initEvent; e.initUIEvent = e.initEvent; e.initKeyboardEvent = e.initEvent;
    return e;
  }
  createRange() { return new Range(); }
  createTreeWalker(root, whatToShow = 0xFFFFFFFF, filter = null) { return new TreeWalker(root, whatToShow, filter); }
  createNodeIterator(root, whatToShow = 0xFFFFFFFF, filter = null) { return new NodeIterator(root, whatToShow, filter); }
  importNode(n, deep) { const c = n.cloneNode(deep); adopt(c, this); return c; }
  adoptNode(n) { if (n._parent) n._parent.removeChild(n); adopt(n, this); return n; }
  hasFocus() { return true; }
  getSelection() { return selection; }
  elementFromPoint() { return null; }
  elementsFromPoint() { return []; }
  caretRangeFromPoint() { return null; }
  execCommand() { return false; }
  queryCommandSupported() { return false; }
  exitFullscreen() { return Promise.resolve(); }
  startViewTransition(cb) { const p = Promise.resolve().then(() => cb && cb()); return { finished: p, ready: Promise.resolve(), updateCallbackDone: p, skipTransition() {} }; }
  open() { if (this._ready !== 'loading') { this.replaceChildren(); } return this; }
  close() {}
  write(...parts) { documentWrite(this, parts.join('')); }
  writeln(...parts) { documentWrite(this, parts.join('') + '\n'); }
  _clone() { const d = new Document(); d._url = this._url; return d; }
}
mix(Document, ParentNodeMixin);
for (const t of ['readystatechange', 'DOMContentLoaded', 'visibilitychange', ...handlerAttrs]) if (t !== 'load') eventHandler(Document.prototype, t);
class HTMLDocument extends Document {}
class XMLDocument extends Document {}

// document.write: while the page is being read, the text goes in after the
// running script, read as HTML; afterwards it replaces the document.
function documentWrite(doc, html) {
  const s = doc._current;
  if (doc._ready === 'loading' && s && s.isConnected) {
    const ref = s.nextSibling;
    const parent = s._parent;
    const f = parseFragment(html, parent.nodeType === ELEMENT ? parent : doc.body);
    const wasParsing = parsing;
    parsing = false; // scripts it writes run at once
    parent.insertBefore(f, ref);
    parsing = wasParsing;
    return;
  }
  if (doc._ready === 'loading' && doc.body) { doc.body.insertAdjacentHTML('beforeend', html); return; }
  // After the page loaded: a new document from the text.
  doc.replaceChildren();
  build(b.parse(html, false, ''), doc, doc);
  touch();
}

const implementation = {
  createHTMLDocument(title) {
    const d = new HTMLDocument();
    build(b.parse(`<!DOCTYPE html><html><head><title></title></head><body></body></html>`, false, ''), d, d);
    if (title !== undefined) d.title = title; else d.querySelector('title').remove();
    return d;
  },
  createDocument(ns, qname) { const d = new XMLDocument(); d._xml = true; if (qname) d.appendChild(d.createElementNS(ns, qname)); return d; },
  createDocumentType(name) { return new DocumentType(name); },
  hasFeature() { return true; },
};
class DOMParser {
  parseFromString(text, type) {
    const d = new HTMLDocument();
    if (/xml/.test(type) && !/html/.test(type)) d._xml = true;
    build(b.parse(String(text), false, ''), d, d);
    d._url = document.URL;
    return d;
  }
}
class XMLSerializer { serializeToString(n) { const out = []; serializeNode(n, out); return out.join(''); } }

class Range {
  constructor() { this.startContainer = this.endContainer = this.commonAncestorContainer = g.document; this.startOffset = this.endOffset = 0; this.collapsed = true; }
  setStart(n, o) { this.startContainer = n; this.startOffset = o; }
  setEnd(n, o) { this.endContainer = n; this.endOffset = o; this.collapsed = false; }
  setStartBefore(n) {} setStartAfter(n) {} setEndBefore(n) {} setEndAfter(n) {}
  selectNode(n) { this.startContainer = this.endContainer = n; }
  selectNodeContents(n) { this.startContainer = this.endContainer = n; }
  collapse() { this.collapsed = true; }
  cloneRange() { return Object.assign(new Range(), this); }
  deleteContents() {} detach() {}
  getBoundingClientRect() { return new DOMRect(); }
  getClientRects() { return []; }
  toString() { return ''; }
  createContextualFragment(html) { return parseFragment(html, g.document.body || g.document); }
  cloneContents() { return g.document.createDocumentFragment(); }
}
const selection = { rangeCount: 0, isCollapsed: true, type: 'None', anchorNode: null, focusNode: null, removeAllRanges() {}, addRange() {}, getRangeAt() { return new Range(); }, toString() { return ''; }, collapse() {}, selectAllChildren() {}, empty() {} };
const NodeFilter = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2, SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, SHOW_PROCESSING_INSTRUCTION: 0x40, SHOW_COMMENT: 0x80, SHOW_DOCUMENT: 0x100, SHOW_DOCUMENT_TYPE: 0x200, SHOW_DOCUMENT_FRAGMENT: 0x400 };
function filterNode(w, n) {
  if (!(w.whatToShow & (1 << (n.nodeType - 1)))) return 3;
  if (!w.filter) return 1;
  return typeof w.filter === 'function' ? w.filter(n) : w.filter.acceptNode(n);
}
class TreeWalker {
  constructor(root, what, filter) { this.root = root; this.whatToShow = what; this.filter = filter; this.currentNode = root; }
  _order() { const out = []; const walk = (n) => { out.push(n); if (n._children) n._children.forEach(walk); }; walk(this.root); return out; }
  nextNode() { const o = this._order(); for (let i = o.indexOf(this.currentNode) + 1; i < o.length; i++) if (filterNode(this, o[i]) === 1) return this.currentNode = o[i]; return null; }
  previousNode() { const o = this._order(); for (let i = o.indexOf(this.currentNode) - 1; i >= 0; i--) if (filterNode(this, o[i]) === 1) return this.currentNode = o[i]; return null; }
  parentNode() { for (let n = this.currentNode._parent; n && n !== this.root._parent; n = n._parent) if (filterNode(this, n) === 1) return this.currentNode = n; return null; }
  firstChild() { for (const c of this.currentNode._children || []) if (filterNode(this, c) === 1) return this.currentNode = c; return null; }
  lastChild() { const c = this.currentNode._children || []; for (let i = c.length; i--;) if (filterNode(this, c[i]) === 1) return this.currentNode = c[i]; return null; }
  nextSibling() { for (let n = this.currentNode.nextSibling; n; n = n.nextSibling) if (filterNode(this, n) === 1) return this.currentNode = n; return null; }
  previousSibling() { for (let n = this.currentNode.previousSibling; n; n = n.previousSibling) if (filterNode(this, n) === 1) return this.currentNode = n; return null; }
}
class NodeIterator extends TreeWalker {
  constructor(root, what, filter) { super(root, what, filter); this._before = true; this.referenceNode = root; }
  nextNode() {
    if (this._before) { this._before = false; if (filterNode(this, this.root) === 1) return this.root; }
    const n = TreeWalker.prototype.nextNode.call(this); this.referenceNode = this.currentNode; return n;
  }
  previousNode() { const n = TreeWalker.prototype.previousNode.call(this); this.referenceNode = this.currentNode; return n; }
  detach() {}
}

// ── more events ───────────────────────────────────────────────────────────

class UIEvent extends Event { constructor(t, i) { super(t, i); this.view = (i && i.view) || null; this.detail = (i && i.detail) || 0; this.which = 0; } }
class MouseEvent extends UIEvent {
  constructor(t, i = {}) {
    super(t, i);
    for (const k of ['screenX', 'screenY', 'clientX', 'clientY', 'pageX', 'pageY', 'offsetX', 'offsetY', 'movementX', 'movementY', 'button', 'buttons']) this[k] = i[k] || 0;
    for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey']) this[k] = !!i[k];
    this.relatedTarget = i.relatedTarget || null;
    this.x = this.clientX; this.y = this.clientY;
  }
  getModifierState() { return false; }
}
class PointerEvent extends MouseEvent { constructor(t, i = {}) { super(t, i); this.pointerId = i.pointerId || 1; this.pointerType = i.pointerType || 'mouse'; this.isPrimary = true; this.width = this.height = 1; this.pressure = 0; } }
class WheelEvent extends MouseEvent { constructor(t, i = {}) { super(t, i); this.deltaX = i.deltaX || 0; this.deltaY = i.deltaY || 0; this.deltaZ = 0; this.deltaMode = 0; } }
class KeyboardEvent extends UIEvent {
  constructor(t, i = {}) {
    super(t, i);
    this.key = i.key || ''; this.code = i.code || ''; this.location = i.location || 0; this.repeat = !!i.repeat; this.isComposing = false;
    for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey']) this[k] = !!i[k];
    this.keyCode = i.keyCode || 0; this.charCode = i.charCode || 0; this.which = this.keyCode;
  }
  getModifierState(k) { return !!this[{ Control: 'ctrlKey', Shift: 'shiftKey', Alt: 'altKey', Meta: 'metaKey' }[k]]; }
}
Object.assign(KeyboardEvent, { DOM_KEY_LOCATION_STANDARD: 0, DOM_KEY_LOCATION_LEFT: 1, DOM_KEY_LOCATION_RIGHT: 2, DOM_KEY_LOCATION_NUMPAD: 3 });
class FocusEvent extends UIEvent { constructor(t, i = {}) { super(t, i); this.relatedTarget = i.relatedTarget || null; } }
class InputEvent extends UIEvent { constructor(t, i = {}) { super(t, i); this.data = i.data ?? null; this.inputType = i.inputType || ''; this.isComposing = false; } }
class SubmitEvent extends Event { constructor(t, i = {}) { super(t, i); this.submitter = i.submitter || null; } }
class HashChangeEvent extends Event { constructor(t, i = {}) { super(t, i); this.oldURL = i.oldURL || ''; this.newURL = i.newURL || ''; } }
class PopStateEvent extends Event { constructor(t, i = {}) { super(t, i); this.state = i.state ?? null; } }
class StorageEvent extends Event { constructor(t, i = {}) { super(t, i); Object.assign(this, { key: null, oldValue: null, newValue: null, url: '', storageArea: null }, i); } }
class TouchEvent extends UIEvent { constructor(t, i = {}) { super(t, i); this.touches = []; this.targetTouches = []; this.changedTouches = []; } }
class AnimationEvent extends Event {}
class TransitionEvent extends Event {}
class ClipboardEvent extends Event { constructor(t, i = {}) { super(t, i); this.clipboardData = i.clipboardData || null; } }
class DragEvent extends MouseEvent {}
class CompositionEvent extends UIEvent {}
class PageTransitionEvent extends Event { constructor(t, i = {}) { super(t, i); this.persisted = !!i.persisted; } }
class BeforeUnloadEvent extends Event {}

// ── window ────────────────────────────────────────────────────────────────

// The window is the global object; give it EventTarget's behaviour.
const winTarget = new EventTarget();
for (const m of ['addEventListener', 'removeEventListener', 'dispatchEvent']) install(m, EventTarget.prototype[m].bind(winTarget));
for (const t of [...handlerAttrs, 'DOMContentLoaded', 'unhandledrejection', 'rejectionhandled', 'beforeprint', 'afterprint', 'languagechange', 'devicemotion', 'deviceorientation']) {
  let slot = null;
  Object.defineProperty(g, 'on' + t, {
    configurable: true, enumerable: true,
    get() { return slot ? slot.fn : null; },
    set(fn) {
      if (typeof fn !== 'function') fn = null;
      if (slot) { slot.fn = fn; return; }
      if (!fn) return;
      slot = { fn };
      winTarget.addEventListener(t, (e) => {
        if (!slot.fn) return;
        if (t === 'error' && e instanceof ErrorEvent) { const r = slot.fn.call(g, e.message, e.filename, e.lineno, e.colno, e.error); if (r === true) e.preventDefault(); return; }
        const r = slot.fn.call(g, e);
        if (r === false) e.preventDefault();
      });
    },
  });
}
// Events dispatched up to the document go on to the window.
winTarget[kParent] = () => null;
const origDispatch = EventTarget.prototype.dispatchEvent;
// A document's parent in the event path is the window's listener holder.
Document.prototype[kParent] = function (event) { return this === g.document && event.type !== 'load' ? winTarget : null; };
Object.defineProperty(winTarget, 'document', { get: () => g.document });

const history = {
  _entries: [], _index: 0,
  get length() { return this._entries.length; },
  get state() { return this._entries[this._index] ? this._entries[this._index].state : null; },
  get scrollRestoration() { return 'auto'; },
  set scrollRestoration(v) {},
  pushState(state, title, url) {
    const u = url !== undefined && url !== null ? new URL(String(url), document.URL).href : document.URL;
    this._entries.splice(this._index + 1);
    this._entries.push({ state: structuredClone(state), url: u });
    this._index = this._entries.length - 1;
    setUrl(u);
  },
  replaceState(state, title, url) {
    const u = url !== undefined && url !== null ? new URL(String(url), document.URL).href : document.URL;
    this._entries[this._index] = { state: structuredClone(state), url: u };
    setUrl(u);
  },
  go(delta = 0) {
    const to = this._index + delta;
    if (!delta) { location.reload(); return; }
    if (to < 0 || to >= this._entries.length) { b.history(delta); return; }
    const old = document.URL;
    this._index = to;
    setUrl(this._entries[to].url);
    setTimeout(() => {
      winTarget.dispatchEvent(new PopStateEvent('popstate', { state: this.state }));
      if (old.split('#')[0] === document.URL.split('#')[0] && old !== document.URL) winTarget.dispatchEvent(new HashChangeEvent('hashchange', { oldURL: old, newURL: document.URL }));
    });
  },
  back() { this.go(-1); },
  forward() { this.go(1); },
};
function setUrl(u) { document._url = u; b.setUrl(u); }

const location = {
  get href() { return document.URL; },
  set href(v) { this.assign(v); },
  assign(v) {
    const u = new URL(String(v), document.baseURI);
    const cur = new URL(document.URL);
    if (u.href.split('#')[0] === cur.href.split('#')[0] && u.hash !== cur.hash) {
      const old = document.URL;
      history.pushState(null, '', u.href);
      winTarget.dispatchEvent(new HashChangeEvent('hashchange', { oldURL: old, newURL: u.href }));
      return;
    }
    b.navigate(u.href, false);
  },
  replace(v) { b.navigate(new URL(String(v), document.baseURI).href, true); },
  reload() { b.navigate(document.URL, true); },
  toString() { return document.URL; },
  get ancestorOrigins() { return []; },
};
for (const part of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin']) {
  Object.defineProperty(location, part, {
    enumerable: true,
    get() { try { return new URL(document.URL)[part]; } catch (e) { return ''; } },
    set(v) { if (part === 'origin') return; const u = new URL(document.URL); u[part] = v; this.assign(u.href); },
  });
}

// Storage: kept by the browser per origin across page loads (for its session).
class Storage {
  constructor(kind) { hide(this, '_kind', kind); hide(this, '_m', null); }
  _map() {
    if (!this._m) {
      let m = new Map();
      try { const j = b.storage(this._kind, origin(), null); if (j) m = new Map(Object.entries(JSON.parse(j))); } catch (e) {}
      this._m = m;
    }
    return this._m;
  }
  _save() { b.storage(this._kind, origin(), JSON.stringify(Object.fromEntries(this._map()))); }
  get length() { return this._map().size; }
  key(i) { return [...this._map().keys()][i] ?? null; }
  getItem(k) { const v = this._map().get(String(k)); return v === undefined ? null : v; }
  setItem(k, v) { this._map().set(String(k), String(v)); this._save(); }
  removeItem(k) { this._map().delete(String(k)); this._save(); }
  clear() { this._map().clear(); this._save(); }
}
const storageTraps = {
  get(t, k, r) { if (typeof k !== 'string' || k in t) return Reflect.get(t, k, r); return t.getItem(k) ?? undefined; },
  set(t, k, v) { if (typeof k !== 'string' || k in t) return Reflect.set(t, k, v); t.setItem(k, v); return true; },
  deleteProperty(t, k) { t.removeItem(k); return true; },
  has(t, k) { return typeof k === 'string' && (k in t || t.getItem(k) !== null); },
  ownKeys(t) { return [...t._map().keys()]; },
  getOwnPropertyDescriptor(t, k) { const v = t.getItem(k); return v === null ? undefined : { value: v, enumerable: true, configurable: true, writable: true }; },
};
const origin = () => { try { return new URL(document.URL).origin; } catch (e) { return 'null'; } };
const localStorage = new Proxy(new Storage('local'), storageTraps);
const sessionStorage = new Proxy(new Storage('session'), storageTraps);

function cookieString(doc) {
  if (!doc._cookies) { try { doc._cookies = JSON.parse(b.storage('cookie', origin(), null) || '{}'); } catch (e) { doc._cookies = {}; } }
  const now = Date.now();
  return Object.entries(doc._cookies).filter(([, c]) => !c.expires || c.expires > now).map(([k, c]) => `${k}=${c.value}`).join('; ');
}
function setCookie(doc, text) {
  cookieString(doc);
  const parts = text.split(';');
  const eq = parts[0].indexOf('=');
  if (eq < 0) return;
  const name = parts[0].slice(0, eq).trim(), value = parts[0].slice(eq + 1).trim();
  let expires = 0;
  for (const p of parts.slice(1)) {
    const [k, v = ''] = p.split('=').map(s => s.trim());
    if (/^expires$/i.test(k)) expires = Date.parse(v) || 0;
    if (/^max-age$/i.test(k)) expires = Date.now() + (+v) * 1000;
  }
  if (expires && expires <= Date.now()) delete doc._cookies[name];
  else doc._cookies[name] = { value, expires };
  b.storage('cookie', origin(), JSON.stringify(doc._cookies));
}

function matchMedia(q) {
  const [cols, rows, cw, ch, dark] = viewport();
  const width = cols * cw, height = rows * ch;
  const test = (s) => s.split(',').some(part => {
    part = part.trim().toLowerCase();
    if (!part) return false;
    let neg = false;
    if (part.startsWith('not ')) { neg = true; part = part.slice(4); }
    const ok = part.split(/\s+and\s+/).every(c => {
      c = c.trim();
      if (c === 'all' || c === 'screen' || c === 'only screen') return true;
      if (c === 'print' || c === 'speech') return false;
      const m = /^\(\s*([a-z-]+)\s*(?::\s*([^)]+))?\)$/.exec(c);
      if (!m) return false;
      const v = (m[2] || '').trim();
      const px = (x) => /em$/.test(x) ? parseFloat(x) * 16 : parseFloat(x);
      switch (m[1]) {
        case 'min-width': return width >= px(v);
        case 'max-width': return width <= px(v);
        case 'min-height': return height >= px(v);
        case 'max-height': return height <= px(v);
        case 'width': return width === px(v);
        case 'orientation': return v === (width >= height ? 'landscape' : 'portrait');
        case 'prefers-color-scheme': return v === (dark ? 'dark' : 'light');
        case 'prefers-reduced-motion': return v === 'reduce';
        case 'prefers-contrast': return v === 'no-preference';
        case 'hover': case 'any-hover': return v === 'hover';
        case 'pointer': case 'any-pointer': return v === 'fine';
        case 'color': return true;
        case 'display-mode': return v === 'browser';
        case 'forced-colors': return v === 'none';
        default: return false;
      }
    });
    return neg ? !ok : ok;
  });
  const mql = new EventTarget();
  Object.assign(mql, { media: String(q), matches: test(String(q)), onchange: null, addListener(f) { mql.addEventListener('change', f); }, removeListener(f) { mql.removeEventListener('change', f); } });
  return mql;
}

function getComputedStyle(el) {
  const s = new CSSStyleDeclaration(el);
  const proxy = new Proxy(s, {
    get(t, k, r) {
      if (k === 'getPropertyValue') return (n) => computed(el, t, String(n));
      if (typeof k !== 'string' || k in t) return Reflect.get(t, k, r);
      return computed(el, t, camelToKebab(k));
    },
  });
  return proxy;
}
const blockTags = /^(html|body|address|article|aside|blockquote|details|dialog|div|dl|dd|dt|fieldset|figcaption|figure|footer|form|h[1-6]|header|hgroup|hr|main|nav|ol|p|pre|section|ul|summary|center|menu|search)$/;
function computed(el, decls, name) {
  const v = decls.getPropertyValue(name);
  if (v) return v;
  switch (name) {
    case 'display':
      if (hiddenByStyle(el)) return 'none';
      if (/^(script|style|head|title|meta|link|template|noscript)$/.test(el._local)) return 'none';
      if (el._local === 'li') return 'list-item';
      if (/^(table)$/.test(el._local)) return 'table';
      if (el._local === 'tr') return 'table-row';
      if (el._local === 'td' || el._local === 'th') return 'table-cell';
      return blockTags.test(el._local) ? 'block' : 'inline';
    case 'visibility': return 'visible';
    case 'position': return 'static';
    case 'opacity': return '1';
    case 'color': return 'rgb(0, 0, 0)';
    case 'background-color': return 'rgba(0, 0, 0, 0)';
    case 'font-size': return '16px';
    case 'font-family': return 'monospace';
    case 'font-weight': return /^(b|strong|h[1-6]|th)$/.test(el._local) ? '700' : '400';
    case 'line-height': return 'normal';
    case 'width': case 'height': return `${rectFor(el)[name]}px`;
    case 'overflow': case 'overflow-x': case 'overflow-y': return 'visible';
    case 'box-sizing': return 'content-box';
    case 'pointer-events': return 'auto';
    case 'z-index': return 'auto';
    case 'transform': return 'none';
    case 'direction': return 'ltr';
    default: return /^(margin|padding|border)/.test(name) ? (/width$/.test(name) || /^(margin|padding)/.test(name) ? '0px' : 'none') : '';
  }
}

class IntersectionObserver {
  constructor(cb, opts = {}) { hide(this, '_cb', cb); hide(this, '_t', []); this.root = opts.root || null; this.rootMargin = opts.rootMargin || '0px'; this.thresholds = [].concat(opts.threshold || 0); }
  // Everything is "on screen": lazy loaders load at once.
  observe(t) {
    if (this._t.includes(t)) return;
    this._t.push(t);
    setTimeout(() => {
      if (!this._t.includes(t)) return;
      const r = rectFor(t);
      try { this._cb([{ target: t, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r, intersectionRect: r, rootBounds: null, time: performance.now() }], this); } catch (e) { reportError(e); }
    });
  }
  unobserve(t) { const i = this._t.indexOf(t); if (i >= 0) this._t.splice(i, 1); }
  disconnect() { this._t.length = 0; }
  takeRecords() { return []; }
}
class ResizeObserver {
  constructor(cb) { hide(this, '_cb', cb); hide(this, '_t', []); }
  observe(t) {
    this._t.push(t);
    setTimeout(() => {
      if (!this._t.includes(t)) return;
      const r = rectFor(t);
      const size = [{ inlineSize: r.width, blockSize: r.height }];
      try { this._cb([{ target: t, contentRect: r, borderBoxSize: size, contentBoxSize: size, devicePixelContentBoxSize: size }], this); } catch (e) { reportError(e); }
    });
  }
  unobserve(t) { const i = this._t.indexOf(t); if (i >= 0) this._t.splice(i, 1); }
  disconnect() { this._t.length = 0; }
}
class PerformanceObserver { constructor() {} observe() {} disconnect() {} takeRecords() { return []; } static get supportedEntryTypes() { return []; } }

function makeImage(w, h) { const el = g.document.createElement('img'); if (w !== undefined) el.width = w; if (h !== undefined) el.height = h; return el; }
function makeOption(text = '', value, defSel, sel) { const o = g.document.createElement('option'); o.text = text; if (value !== undefined) o.value = value; if (defSel) o.defaultSelected = true; if (sel) o.selected = true; return o; }
function makeAudio(src) { const a = g.document.createElement('audio'); if (src) a.src = src; return a; }

const CSS = {
  supports(prop, value) { return value === undefined ? /^\(?\s*[a-z-]+\s*:/.test(String(prop)) && !/grid|subgrid/.test(prop) : /^[a-z-]+$/.test(String(prop)); },
  escape(s) { return String(s).replace(/([^\w-])/g, '\\$1').replace(/^(\d)/, '\\3$1 '); },
};

const screen = {
  get width() { const [c, , cw] = viewport(); return c * cw; },
  get height() { const [, r, , ch] = viewport(); return r * ch; },
  get availWidth() { return this.width; }, get availHeight() { return this.height; },
  colorDepth: 4, pixelDepth: 4, orientation: { type: 'landscape-primary', angle: 0, addEventListener() {}, removeEventListener() {} },
};

const windowProps = {
  window: g, frames: g, parent: g, top: g, opener: null, frameElement: null, closed: false, length: 0, name: '', status: '',
  location, history, localStorage, sessionStorage, screen, customElements, CSS,
  alert(m) { b.alert(m === undefined ? '' : String(m)); },
  confirm(m) { b.alert(String(m ?? '')); return true; },
  prompt(m, d) { b.alert(String(m ?? '')); return d === undefined ? null : String(d); },
  print() {}, focus() {}, blur() {}, stop() {}, close() {}, moveTo() {}, resizeTo() {},
  open(url) { if (url) b.open(new URL(String(url), document.baseURI).href); return null; },
  postMessage(data, origin) { setTimeout(() => winTarget.dispatchEvent(new MessageEvent('message', { data: structuredClone(data), origin: location.origin, source: g }))); },
  scroll() {}, scrollTo() {}, scrollBy() {},
  getComputedStyle, matchMedia,
  getSelection() { return selection; },
  MutationObserver, IntersectionObserver, ResizeObserver, PerformanceObserver,
  Node, Element, HTMLElement, SVGElement, SVGSVGElement, MathMLElement, Text, Comment, CDATASection, CharacterData, ProcessingInstruction, Document, HTMLDocument, XMLDocument, DocumentFragment, DocumentType, ShadowRoot, Attr,
  NodeList, HTMLCollection, NamedNodeMap, DOMTokenList, CSSStyleDeclaration, DOMParser, XMLSerializer, Range, NodeFilter, TreeWalker, NodeIterator,
  UIEvent, MouseEvent, PointerEvent, WheelEvent, KeyboardEvent, FocusEvent, InputEvent, SubmitEvent, HashChangeEvent, PopStateEvent, StorageEvent, TouchEvent, AnimationEvent, TransitionEvent, ClipboardEvent, DragEvent, CompositionEvent, PageTransitionEvent, BeforeUnloadEvent,
  Storage, Image: function Image(w, h) { return makeImage(w, h); }, Option: function Option(t, v, d, s) { return makeOption(t, v, d, s); }, Audio: function Audio(src) { return makeAudio(src); },
  HTMLUnknownElement, HTMLHtmlElement, HTMLHeadElement, HTMLBodyElement, HTMLTitleElement, HTMLDivElement, HTMLSpanElement, HTMLParagraphElement, HTMLHeadingElement,
  HTMLPreElement, HTMLBRElement, HTMLHRElement, HTMLUListElement, HTMLOListElement, HTMLLIElement, HTMLDListElement, HTMLQuoteElement, HTMLModElement, HTMLTimeElement, HTMLDataElement,
  HTMLAnchorElement, HTMLAreaElement, HTMLImageElement, HTMLPictureElement, HTMLSourceElement, HTMLTrackElement, HTMLMapElement, HTMLCanvasElement, HTMLMediaElement, HTMLVideoElement,
  HTMLAudioElement, HTMLIFrameElement, HTMLEmbedElement, HTMLObjectElement, HTMLMetaElement, HTMLBaseElement, HTMLLinkElement, HTMLStyleElement, HTMLScriptElement, HTMLTemplateElement,
  HTMLSlotElement, HTMLFormElement, HTMLInputElement, HTMLButtonElement, HTMLTextAreaElement, HTMLSelectElement, HTMLOptionElement, HTMLOptGroupElement, HTMLDataListElement,
  HTMLLabelElement, HTMLFieldSetElement, HTMLLegendElement, HTMLOutputElement, HTMLProgressElement, HTMLMeterElement, HTMLDetailsElement, HTMLDialogElement, HTMLMenuElement,
  HTMLTableElement, HTMLTableSectionElement, HTMLTableRowElement, HTMLTableCellElement, HTMLTableCaptionElement, HTMLTableColElement,
  isSecureContext: false, crossOriginIsolated: false, origin: '',
  visualViewport: null,
};
for (const [k, v] of Object.entries(windowProps)) install(k, v);
Object.defineProperty(g, 'origin', { configurable: true, get: () => location.origin });
for (const p of ['innerWidth', 'outerWidth']) Object.defineProperty(g, p, { configurable: true, get: () => screen.width });
for (const p of ['innerHeight', 'outerHeight']) Object.defineProperty(g, p, { configurable: true, get: () => screen.height });
Object.defineProperty(g, 'devicePixelRatio', { configurable: true, get: () => 1 });
for (const p of ['scrollX', 'scrollY', 'pageXOffset', 'pageYOffset', 'screenX', 'screenY', 'screenLeft', 'screenTop']) Object.defineProperty(g, p, { configurable: true, get: () => 0 });
Object.defineProperty(g, 'event', { configurable: true, get: () => undefined });

// ── rendering for the browser ─────────────────────────────────────────────

// The tree as HTML for web/doc.cpp: every link, form control and element
// with click behaviour carries onclick="r2:N", N its node's id (stable from
// one render to the next, so the browser keeps its focus), and controls
// show their current state.  Scripts, templates and hidden
// elements are left out; a shadow root is drawn in place of its host's
// children.
let rendered = new Map();
function renderHtml(doc) {
  rendered = new Map();
  const out = [];
  const mark = (n) => { rendered.set(n._uid, n); return n._uid; };
  const clickable = (el) => {
    if (el._local === 'a' || el._local === 'button' || el._local === 'input' || el._local === 'select' || el._local === 'textarea' || el._local === 'summary' || el._local === 'area') return true;
    return false;
  };
  const listensClick = (el) => (el[kListeners] && (el[kListeners].get('click') || el[kListeners].get('mousedown') || el[kListeners].get('pointerdown'))) || el.hasAttribute('onclick');
  const attrs = (el, extra) => {
    for (const a of el._attrs) {
      if (a.name === 'onclick' || (a.name.startsWith('on') && a.name.length > 2)) continue;
      if (extra && extra.skip && extra.skip.includes(a.name)) continue;
      out.push(' ', a.name, '="', escAttr(a.value), '"');
    }
  };
  const walk = (n, inLink) => {
    for (const c of (n._shadow ? n._shadow._children : n._children) || []) {
      if (c.nodeType === TEXT) { out.push(escText(c._data)); continue; }
      if (c.nodeType !== ELEMENT) continue;
      const el = c, t = el._local;
      if (el._ns !== HTML_NS) {
        if (t === 'svg') { const tx = el.getAttribute('aria-label') || el.querySelector('title')?.textContent; if (tx) out.push(escText(tx)); }
        continue;
      }
      if (t === 'script' || t === 'template' || t === 'noscript' || hiddenByStyle(el)) continue;
      if (t === 'meta' && (el.hasAttribute('charset') || /content-type/i.test(el.getAttribute('http-equiv') || ''))) continue;
      if (t === 'slot') { walk(el, inLink); continue; }
      const isCtl = clickable(el);
      // An element that only a listener makes clickable becomes a link, when
      // it holds no link or control of its own.
      const pseudoLink = !isCtl && !inLink && t !== 'body' && t !== 'html' && t !== 'form' && listensClick(el) && !el.querySelector('a,button,input,select,textarea');
      if (pseudoLink) {
        out.push('<', t); attrs(el); out.push('><a href="#r2" onclick="r2:', String(mark(el)), '">');
        walk(el, true);
        out.push('</a></', t, '>');
        continue;
      }
      out.push('<', t);
      if (isCtl) {
        const skip = ['value', 'checked', 'selected'];
        attrs(el, { skip: t === 'a' || t === 'area' || t === 'summary' ? [] : skip });
        out.push(' onclick="r2:', String(mark(el)), '"');
        if (t === 'a' && !el.hasAttribute('href') && listensClick(el)) out.push(' href="#r2"');
        if (t === 'input') {
          const ty = el.type;
          if (ty === 'checkbox' || ty === 'radio') { if (el.checked) out.push(' checked'); out.push(' value="', escAttr(el.value), '"'); }
          else out.push(' value="', escAttr(el.value), '"');
        } else if (t === 'button' && el.hasAttribute('value')) out.push(' value="', escAttr(el.value), '"');
      } else if (t === 'option') {
        attrs(el, { skip: ['selected'] });
        if (el.selected) out.push(' selected');
      } else attrs(el);
      out.push('>');
      if (VOID.has(t)) continue;
      if (t === 'textarea') out.push(escText(el.value));
      else if (t === 'style' || t === 'xmp' || t === 'plaintext') { for (const x of el._children) if (x.nodeType === TEXT) out.push(x._data); }
      else if (t === 'title') out.push(escText(el.textContent));
      else walk(el, inLink || t === 'a');
      out.push('</', t, '>');
    }
  };
  for (const c of doc._children) {
    if (c.nodeType === DOCTYPE) out.push('<!DOCTYPE ', c._name, '>');
    else if (c.nodeType === ELEMENT) {
      out.push('<', c._local);
      attrs(c);
      out.push('>');
      walk(c, false);
      out.push('</', c._local, '>');
    }
  }
  return out.join('');
}

// Elements with an id are properties of the window (window.foo, or just
// foo, in old pages), unless the name is taken by a real global.
const named = new Set();
function namedAccess() {
  for (const el of descendants(document)) {
    const id = el.getAttribute('id');
    if (!id || named.has(id) || !/^[A-Za-z_$][\w$-]*$/.test(id) || id in g) continue;
    named.add(id);
    Object.defineProperty(g, id, {
      configurable: true, enumerable: false,
      get() { return document.getElementById(id) || undefined; },
      set(v) { Object.defineProperty(g, id, { value: v, writable: true, configurable: true, enumerable: true }); },
    });
  }
}

// What the browser calls.
const bridge = {
  pendingSubmit: null,
  // A new page: its tree from the parser's ops.
  load(url, ops) {
    const doc = new HTMLDocument();
    doc._url = url;
    doc._ready = 'loading';
    install('document', doc);
    history._entries = [{ state: null, url }];
    history._index = 0;
    build(ops, doc, doc);
    if (!doc.documentElement) build(b.parse('<html><head></head><body></body></html>', false, ''), doc, doc);
    parsing = true;
    dirty = true;
  },
  // The page's own scripts, in document order, for the browser to run:
  // [kind, src, inline text, async, defer] with kind 0 classic, 1 module.
  scripts() {
    namedAccess();
    const out = [];
    for (const el of byTag(document, 'script')) {
      const k = scriptKind(el);
      el._started = true;
      if (!k) continue;
      if (k === 'classic' && el.hasAttribute('nomodule')) continue;
      out.push([k === 'module' ? 1 : 0, el.hasAttribute('src') ? el.src : '', el.textContent, el.hasAttribute('async'), el.hasAttribute('defer'), el]);
    }
    hide(bridge, '_scripts', out.map(x => x.pop()));
    return out;
  },
  // Script i of scripts() is about to run (-1: none is).
  current(i) { document._current = i >= 0 && bridge._scripts ? bridge._scripts[i] : null; },
  // Every parser script ran: the document is interactive, then complete.
  parsed() {
    parsing = false;
    document._current = null;
    // <body onload="..."> and the like are the window's handlers.
    const body = document.body;
    if (body) for (const t of ['load', 'unload', 'beforeunload', 'hashchange', 'popstate', 'message', 'resize', 'scroll', 'storage', 'online', 'offline', 'pageshow', 'pagehide'])
      if (body.hasAttribute('on' + t) && !g['on' + t]) g['on' + t] = function (e) { const fn = contentHandler(body, t); return fn ? fn.call(body, e) : undefined; };
    document._ready = 'interactive';
    document.dispatchEvent(new Event('readystatechange'));
    document.dispatchEvent(new Event('DOMContentLoaded', { bubbles: true }));
    // Scripts may have been added while the page was read: run them now.
    for (const el of byTag(document, 'script')) if (!el._started) startScript(el);
    setTimeout(() => {
      document._ready = 'complete';
      document.dispatchEvent(new Event('readystatechange'));
      winTarget.dispatchEvent(new Event('load'));
      winTarget.dispatchEvent(new PageTransitionEvent('pageshow', { persisted: false }));
    });
  },
  // An external script the page added has arrived (or not).
  scriptLoaded(id, ok) {
    const el = pendingScripts.get(id);
    pendingScripts.delete(id);
    if (el) el.dispatchEvent(new Event(ok ? 'load' : 'error'));
  },
  scriptElement(id) { const el = pendingScripts.get(id); return el || null; },
  // The page changed since the last render; the HTML to lay out, or null.
  render(force) {
    if (!dirty && !force) return null;
    dirty = false;
    renderDepth++;
    namedAccess();
    return [renderHtml(document), document.title];
  },
  isDirty() { return dirty; },
  // The user clicked rendered node n: true when the browser should go on
  // with what the link or control does by itself.
  click(n) {
    // 1: the browser goes on with what the link or control does itself (a
    // link's href, typing into a field, the next option of a list); 0: the
    // page handled it, or the DOM did (a submit button sends its form
    // through b.submit).
    const el = rendered.get(n);
    if (!el || !el.isConnected) return 1;
    const t = el._local;
    if (t === 'input' && (el.type === 'checkbox' || el.type === 'radio')) return 1; // state() follows
    const ev = new MouseEvent('click', { bubbles: true, cancelable: true, composed: true, view: g, detail: 1 });
    el.dispatchEvent(ev);
    if (ev.defaultPrevented) return 0;
    if (t === 'a' || t === 'area') {
      const h = el.getAttribute('href');
      if (h === null || h === '#r2') return 0;
      if (h.startsWith('#')) { location.assign(h); return 0; }
      if (/^javascript:/i.test(h)) { b.evalScript(decodeURIComponent(h.slice(11)), document.URL, false); return 0; }
      return 1;
    }
    if (t === 'summary') { const d = el.closest('details'); if (d) d.open = !d.open; return 0; }
    if (t === 'input') return /^(submit|image|reset|button)$/.test(el.type) ? 0 : 1;
    if (t === 'button') return 0;
    return 1;
  },
  // The user changed rendered control n: its text, its check or its choice.
  input(n, value) {
    const el = rendered.get(n);
    if (!el || el.value === value) return;
    el._value = value;
    el.dispatchEvent(new InputEvent('input', { bubbles: true, data: null, inputType: 'insertText' }));
  },
  changed(n) { const el = rendered.get(n); if (el) el.dispatchEvent(new Event('change', { bubbles: true })); },
  state(n, checked, selected) {
    const el = rendered.get(n);
    if (!el) return true;
    if (el._local === 'select') {
      if (el.selectedIndex === selected) return true;
      el.selectedIndex = selected;
      el.dispatchEvent(new InputEvent('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
      return true;
    }
    if (el.checked === !!checked) return true;
    el.checked = !!checked;
    const ev = new MouseEvent('click', { bubbles: true, cancelable: true, view: g });
    el.dispatchEvent(ev);
    if (ev.defaultPrevented) { el.checked = !checked; return false; }
    el.dispatchEvent(new InputEvent('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
    return true;
  },
  focus(n) { const el = rendered.get(n); if (el && el.focus) el.focus(); },
  // Enter in a text field: the form's implicit submission.
  submitFrom(n) {
    const el = rendered.get(n);
    const form = el && el.form;
    if (!form) return true;
    const btn = form.querySelector('button:not([type=button]):not([type=reset]),input[type=submit],input[type=image]');
    bridge.pendingSubmit = null;
    if (btn) btn.click(); else submitForm(form, null, true);
    return false;
  },
  key(type, key, code, keyCode, ctrl, shift, alt) {
    const target = document.activeElement || document.body || document;
    const ev = new KeyboardEvent(type, { bubbles: true, cancelable: true, key, code, keyCode, ctrlKey: ctrl, shiftKey: shift, altKey: alt, view: g });
    target.dispatchEvent(ev);
    return ev.defaultPrevented;
  },
  unload() {
    try { winTarget.dispatchEvent(new PageTransitionEvent('pagehide', { persisted: false })); winTarget.dispatchEvent(new Event('unload')); } catch (e) {}
  },
  hashchange(oldURL, newURL) { winTarget.dispatchEvent(new HashChangeEvent('hashchange', { oldURL, newURL })); },
};
install('document', new HTMLDocument());
return bridge;
})
