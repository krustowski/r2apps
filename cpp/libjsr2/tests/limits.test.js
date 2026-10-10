test('an endless loop is stopped and the page goes on', async () => {
  setTimeout(() => { for (;;) {} }, 1);
  let after = false;
  setTimeout(() => { after = true; }, 5);
  await waitFor(() => after, 3000);
  assert(/execution limit/.test(__probe('error')), __probe('error'));
});
test('an endless loop with a catch cannot catch the stop', async () => {
  let caught = false;
  setTimeout(() => { for (;;) { try { for (;;) {} } catch (e) { caught = true; } } }, 1);
  await sleep(700);
  eq(caught, false);
  assert(/execution limit/.test(__probe('error')));
});
test('running out of memory is an error, not a crash', () => {
  throws(() => { let s = 'x'; for (;;) s += s; }, e => assert(e instanceof RangeError || e instanceof InternalError || /memory|length/i.test(String(e)), String(e)));
  const keep = [];
  throws(() => { for (;;) keep.push(new Array(10000).fill(1)); });
  keep.length = 0;
  eq(1 + 1, 2);
});
test('deep recursion is a RangeError', () => {
  const f = (n) => f(n + 1) + 1;
  throws(() => f(0), e => assert(e instanceof RangeError || /stack/i.test(String(e)), String(e)));
});
