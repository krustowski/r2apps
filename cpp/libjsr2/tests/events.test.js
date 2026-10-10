test('listeners, options and order', () => {
  const t = new EventTarget(), seen = [];
  const f = (e) => seen.push('f:' + e.type);
  t.addEventListener('x', f);
  t.addEventListener('x', f); // duplicate ignored
  t.addEventListener('x', { handleEvent: (e) => seen.push('obj') });
  t.addEventListener('x', () => seen.push('once'), { once: true });
  t.dispatchEvent(new Event('x'));
  t.dispatchEvent(new Event('x'));
  deepEq(seen, ['f:x', 'obj', 'once', 'f:x', 'obj']);
  t.removeEventListener('x', f);
  seen.length = 0;
  t.dispatchEvent(new Event('x'));
  deepEq(seen, ['obj']);
});
test('preventDefault, passive and stopImmediatePropagation', () => {
  const t = new EventTarget();
  t.addEventListener('c', e => e.preventDefault(), { passive: true });
  eq(t.dispatchEvent(new Event('c', { cancelable: true })), true, 'passive cannot cancel');
  t.addEventListener('c', e => { e.preventDefault(); e.stopImmediatePropagation(); });
  let reached = false;
  t.addEventListener('c', () => { reached = true; });
  eq(t.dispatchEvent(new Event('c', { cancelable: true })), false);
  eq(reached, false);
});
test('a listener that throws does not stop the others', () => {
  const t = new EventTarget();
  let second = false;
  t.addEventListener('e', () => { throw new Error('first fails'); });
  t.addEventListener('e', () => { second = true; });
  t.dispatchEvent(new Event('e'));
  assert(second);
  assert(/first fails/.test(__probe('error')));
});
test('AbortController', async () => {
  const c = new AbortController();
  let fired = 0;
  c.signal.onabort = () => fired++;
  c.signal.addEventListener('abort', () => fired++);
  const t = new EventTarget();
  let calls = 0;
  t.addEventListener('ping', () => calls++, { signal: c.signal });
  t.dispatchEvent(new Event('ping'));
  c.abort();
  c.abort();
  t.dispatchEvent(new Event('ping'));
  eq(fired, 2); eq(calls, 1);
  eq(c.signal.reason.name, 'AbortError');
  throws(() => c.signal.throwIfAborted(), e => eq(e.name, 'AbortError'));
  const s = AbortSignal.timeout(5);
  await sleep(20);
  eq(s.reason.name, 'TimeoutError');
});
test('CustomEvent and DOMException', () => {
  const e = new CustomEvent('hi', { detail: { n: 1 }, bubbles: true });
  eq(e.detail.n, 1); eq(e.bubbles, true);
  const d = new DOMException('nope', 'NotFoundError');
  eq(d.code, 8); eq(d.name, 'NotFoundError'); assert(d instanceof Error);
  eq(DOMException.NOT_FOUND_ERR, 8);
});
