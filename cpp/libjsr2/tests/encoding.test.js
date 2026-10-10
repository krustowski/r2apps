test('TextEncoder / TextDecoder', () => {
  const b = new TextEncoder().encode('ž€😀');
  deepEq(Array.from(b), [0xc5, 0xbe, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80]);
  eq(new TextDecoder().decode(b), 'ž€😀');
  eq(new TextDecoder().decode(new Uint8Array([0x61, 0xff, 0x62, 0xe2, 0x82])), 'a�b�');
  throws(() => new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xff])), e => assert(e instanceof TypeError));
  const d = new TextDecoder();
  eq(d.decode(new Uint8Array([0xf0, 0x9f]), { stream: true }), '');
  eq(d.decode(new Uint8Array([0x98, 0x80, 0x21])), '😀!');
  eq(new TextDecoder().decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x41])), 'A', 'BOM');
  eq(new TextDecoder('latin1').decode(new Uint8Array([0xe9, 0x80])), 'é€');
  eq(new TextEncoder().encode('\ud800').join(), '239,191,189', 'lone surrogate');
  const dest = new Uint8Array(3);
  deepEq(new TextEncoder().encodeInto('až€', dest), { read: 2, written: 3 });
});
test('atob / btoa', () => {
  eq(btoa('hello'), 'aGVsbG8=');
  eq(btoa('\xff\xfe'), '//4=');
  eq(atob('aGVsbG8='), 'hello');
  eq(atob(' aGVs bG8 '), 'hello');
  eq(atob('//4='), '\xff\xfe');
  throws(() => btoa('ž'), e => eq(e.name, 'InvalidCharacterError'));
  throws(() => atob('a'), e => eq(e.name, 'InvalidCharacterError'));
  throws(() => atob('a===='));
});
test('crypto and structuredClone', () => {
  const a = crypto.getRandomValues(new Uint8Array(16));
  assert(a.some(x => x !== 0));
  assert(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(crypto.randomUUID()));
  const o = { d: new Date(5), m: new Map([[1, { x: [1, 2] }]]), s: new Set([1]), u: new Uint8Array([1, 2]) };
  o.self = o;
  const c = structuredClone(o);
  assert(c !== o && c.self === c && c.d.getTime() === 5 && c.m.get(1).x[1] === 2 && c.s.has(1) && c.u[1] === 2);
  throws(() => structuredClone(() => 1), e => eq(e.name, 'DataCloneError'));
});
test('Blob and FormData', async () => {
  const b = new Blob(['ab', new Uint8Array([99]), new Blob(['d'])], { type: 'Text/Plain' });
  eq(b.size, 4); eq(b.type, 'text/plain');
  eq(await b.text(), 'abcd');
  eq(await b.slice(1, -1).text(), 'bc');
  const f = new FormData();
  f.append('a', '1'); f.append('a', '2'); f.set('b', new Blob(['x']), 'x.txt');
  deepEq(f.getAll('a'), ['1', '2']);
  eq(f.get('b').name, 'x.txt');
});
