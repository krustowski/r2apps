test('setTimeout order and arguments', async () => {
  const seen = [];
  setTimeout(() => seen.push('b'), 20);
  setTimeout((x, y) => seen.push(x + y), 5, 'a', '!');
  setTimeout(() => seen.push('c'), 20);
  await sleep(40);
  deepEq(seen, ['a!', 'b', 'c']);
});
test('clearTimeout and setInterval', async () => {
  let fired = false, count = 0;
  const t = setTimeout(() => { fired = true; }, 5);
  clearTimeout(t);
  const iv = setInterval(() => { if (++count === 3) clearInterval(iv); }, 3);
  await sleep(40);
  eq(fired, false);
  eq(count, 3);
});
test('a string timer is evaluated', async () => {
  globalThis.stringTimer = 0;
  setTimeout('stringTimer = 42', 1);
  await sleep(10);
  eq(stringTimer, 42);
});
test('microtasks run before the next timer', async () => {
  const order = [];
  setTimeout(() => order.push('timer'), 0);
  queueMicrotask(() => order.push('micro'));
  Promise.resolve().then(() => order.push('promise'));
  await sleep(10);
  deepEq(order, ['micro', 'promise', 'timer']);
});
test('requestAnimationFrame', async () => {
  let ts = 0;
  const id = requestAnimationFrame(t => { ts = t; });
  let cancelled = true;
  cancelAnimationFrame(requestAnimationFrame(() => { cancelled = false; }));
  await sleep(20);
  assert(ts > 0, 'frame ran');
  assert(cancelled, 'cancelled frame did not run');
  assert(id > 0);
});
test('errors in a callback are reported and do not stop the loop', async () => {
  setTimeout(() => { throw new TypeError('boom'); }, 1);
  let after = false;
  setTimeout(() => { after = true; }, 5);
  await sleep(15);
  assert(after, 'later timer ran');
  assert(/TypeError: boom/.test(__probe('error')), __probe('error'));
});
test('unhandled rejections are reported', async () => {
  Promise.reject(new RangeError('nobody caught me'));
  const handled = Promise.reject(new Error('caught later'));
  handled.catch(() => {});
  await sleep(5);
  assert(/in promise\) RangeError: nobody caught me/.test(__probe('error')), __probe('error'));
});
test('console formats values', () => {
  console.log('fmt %s=%d', 'x', 42, { a: [1, 'b'] }, null);
  assert(__probe('log').includes(`fmt x=42 { a: [ 1, "b" ] } null`), __probe('log'));
  console.error(new Error('shown'));
  assert(__probe('log').includes('Error: shown'));
});
test('performance.now is monotonic', () => {
  const a = performance.now(), b = performance.now();
  assert(b >= a && a >= 0);
  assert(Math.abs(performance.timeOrigin + a - Date.now()) < 1000);
});
