test('fetch text, in pieces', async () => {
  const r = await fetch('/hello.txt');
  eq(r.status, 200); eq(r.ok, true); eq(r.url, 'http://test/hello.txt');
  eq(r.headers.get('Content-Type'), 'text/plain');
  eq(await r.text(), 'hello world');
  await throwsAsync(() => r.text(), e => assert(e instanceof TypeError));
});
test('fetch json across UTF-8 piece boundaries', async () => {
  const j = await (await fetch('http://test/data.json')).json();
  eq(j.a, 1); eq(j.list.length, 3); eq(j.text, 'žluťoučký');
});
test('relative URLs resolve against the page', async () => {
  const r = await fetch('../echo');
  eq(r.url, 'http://test/echo');
});
test('POST with headers and a body', async () => {
  const r = await fetch('/echo', { method: 'post', headers: { 'X-Test': 'yes' }, body: JSON.stringify({ k: 'ž' }) });
  const j = await r.json();
  eq(j.method, 'POST');
  assert(/x-test: yes/.test(j.headers), j.headers);
  assert(/content-type: text\/plain;charset=UTF-8/.test(j.headers), j.headers);
  eq(JSON.parse(j.body).k, 'ž');
  const form = await (await fetch('/echo', { method: 'POST', body: new URLSearchParams({ a: 'b c' }) })).json();
  eq(form.body, 'a=b+c');
});
test('status and failures', async () => {
  const r = await fetch('/missing');
  eq(r.status, 404); eq(r.ok, false); eq(await r.text(), 'not here');
  await throwsAsync(() => fetch('/broken'), e => { assert(e instanceof TypeError); assert(/connection refused/.test(e.message), e.message); });
  await throwsAsync(() => fetch('ftp://x/'), e => assert(e instanceof TypeError));
  await throwsAsync(() => fetch('http://elsewhere/'), e => assert(/only http:\/\/test/.test(e.message), e.message));
});
test('redirects are reported', async () => {
  const r = await fetch('/redirect');
  eq(r.redirected, true); eq(r.url, 'http://test/hello.txt');
});
test('abort', async () => {
  const c = new AbortController();
  const p = fetch('/slow', { signal: c.signal });
  const r = await p;
  const body = r.text();
  c.abort();
  await throwsAsync(() => body, e => eq(e.name, 'AbortError'));
  await waitFor(() => __probe('openRequests') === 0);
  const c2 = new AbortController();
  c2.abort();
  await throwsAsync(() => fetch('/hello.txt', { signal: c2.signal }), e => eq(e.name, 'AbortError'));
});
test('data: URLs and Response objects', async () => {
  eq(await (await fetch('data:text/plain,a%20b')).text(), 'a b');
  eq(await (await fetch('data:;base64,aGk=')).text(), 'hi');
  const r = new Response('{"x":2}', { status: 201, headers: { 'content-type': 'application/json' } });
  eq(r.status, 201); eq((await r.clone().json()).x, 2); eq(await r.text(), '{"x":2}');
  eq((await Response.json({ y: 3 }).json()).y, 3);
  const h = new Headers([['a', '1'], ['A', '2']]);
  eq(h.get('a'), '1, 2');
  deepEq([...new Headers({ b: '1', a: '2' }).keys()], ['a', 'b']);
});
test('XMLHttpRequest', async () => {
  const x = new XMLHttpRequest();
  const states = [];
  x.onreadystatechange = () => states.push(x.readyState);
  let loaded = false;
  x.addEventListener('load', () => { loaded = true; });
  x.open('GET', '/data.json');
  x.responseType = 'json';
  x.send();
  await waitFor(() => loaded);
  eq(x.status, 200);
  eq(x.response.text, 'žluťoučký');
  eq(states[0], 1); eq(states[states.length - 1], 4);
  eq(x.getResponseHeader('content-type'), 'application/json');
  const y = new XMLHttpRequest();
  let failed = false;
  y.onerror = () => { failed = true; };
  y.open('GET', '/broken'); y.send();
  await waitFor(() => failed);
  eq(y.status, 0);
  const z = new XMLHttpRequest();
  let aborted = false;
  z.onabort = () => { aborted = true; };
  z.open('GET', '/slow'); z.send();
  await sleep(1);
  z.abort();
  assert(aborted);
  throws(() => new XMLHttpRequest().open('GET', '/x', false), e => eq(e.name, 'InvalidAccessError'));
});
