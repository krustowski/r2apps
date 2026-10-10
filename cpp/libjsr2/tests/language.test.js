// The engine itself: modern syntax, the standard library, numbers.
test('modern syntax', async () => {
  class A { #x = 1; static s = 2; get x() { return this.#x; } inc() { this.#x++; return this; } }
  eq(new A().inc().x, 2);
  eq(A.s, 2);
  const o = { a: { b: null } };
  eq(o?.a?.b ?? 'dflt', 'dflt');
  eq(o.z?.y, undefined);
  let n = null; n ??= 5; eq(n, 5);
  const [x, ...rest] = [1, 2, 3]; deepEq(rest, [2, 3]);
  const { a: { b = 7 } = {} } = { a: {} }; eq(b, 7);
  eq(`${1 + 1}px`, '2px');
  eq(2 ** 10, 1024);
  eq(10n ** 20n, 100000000000000000000n);
  eq([1, [2, [3]]].flat(Infinity).join(), '1,2,3');
  eq(Object.fromEntries([['a', 1]]).a, 1);
  eq('abc'.at(-1), 'c');
  eq([3, 1, 2].toSorted().join(), '1,2,3');
  eq([1, 2, 3].findLast(v => v < 3), 2);
  eq('a-b-c'.replaceAll('-', '+'), 'a+b+c');
  eq(Object.groupBy([1, 2, 3], v => v % 2 ? 'odd' : 'even').odd.length, 2);
  const r = await Promise.allSettled([Promise.resolve(1), Promise.reject(new Error('x'))]);
  eq(r[1].status, 'rejected');
  eq(await Promise.any([Promise.reject(1), Promise.resolve(2)]), 2);
  function* gen() { yield 1; yield 2; }
  eq([...gen()].length, 2);
  async function* agen() { yield 1; await null; yield 2; }
  let sum = 0; for await (const v of agen()) sum += v; eq(sum, 3);
  eq(new Set([1, 2]).union(new Set([2, 3])).size, 3);
  eq(Iterator.from([1, 2, 3]).map(v => v * 2).toArray().join(), '2,4,6');
  const { promise, resolve } = Promise.withResolvers(); resolve(9); eq(await promise, 9);
});
test('regexps', () => {
  const m = /(?<y>\d{4})-(?<m>\d\d)/u.exec('on 2026-10 ok');
  eq(m.groups.y, '2026');
  eq('aBc'.replace(/b/i, 'x'), 'axc');
  assert(/\p{L}+/u.test('žluťoučký'));
  eq([...'a1b2'.matchAll(/\d/g)].length, 2);
  eq(/(?<=\$)\d+/.exec('cost $42')[0], '42');
});
test('numbers and Math', () => {
  eq((1.25).toFixed(1), '1.3');
  eq((0.1 + 0.2).toString(), '0.30000000000000004');
  eq((1e21).toString(), '1e+21');
  eq((255).toString(16), 'ff');
  eq(parseFloat('3.14abc'), 3.14);
  eq(Number('0x10'), 16);
  eq((123.456).toPrecision(4), '123.5');
  eq((1234.5678).toExponential(2), '1.23e+3');
  assert(Math.abs(Math.sin(Math.PI / 6) - 0.5) < 1e-15, 'sin');
  assert(Math.abs(Math.cos(Math.PI / 3) - 0.5) < 1e-15, 'cos');
  near(Math.sqrt(2), 1.4142135623730951);
  near(Math.exp(1), Math.E);
  near(Math.log(Math.E), 1);
  near(Math.pow(2, 0.5), Math.SQRT2);
  near(Math.atan2(1, 1), Math.PI / 4);
  near(Math.cbrt(27), 3);
  eq(Math.hypot(3, 4), 5);
  near(Math.log10(1000), 3);
  near(Math.log2(8), 3);
  eq(Math.round(-2.5), -2);
  eq(Math.trunc(-4.7), -4);
  eq(Math.sign(-3), -1);
  eq(Math.fround(5.5), 5.5);
  eq(Math.clz32(1), 31);
  assert(Math.random() >= 0 && Math.random() < 1);
});
test('dates', () => {
  const d = new Date(Date.UTC(2026, 9, 10, 12, 30, 0));
  eq(d.toISOString(), '2026-10-10T12:30:00.000Z');
  eq(d.getTimezoneOffset(), new Date().getTimezoneOffset());
  eq(Date.parse('2026-10-10T12:30:00Z'), d.getTime());
  eq(new Date('2000-01-01').getUTCDay(), 6);
  assert(Date.now() > 1.7e12, 'clock');
});
test('JSON and strings', () => {
  const s = JSON.stringify({ a: [1, 'x', null, true], b: { c: 'ž' } });
  eq(s, '{"a":[1,"x",null,true],"b":{"c":"ž"}}');
  eq(JSON.parse(s).b.c, 'ž');
  eq('ŽLUŤOUČKÝ'.toLowerCase(), 'žluťoučký');
  eq('ß'.toUpperCase(), 'SS');
  eq('é'.normalize('NFC'), 'é');
  eq([...'👍🏽'].length, 2);
  eq('abc'.localeCompare('abd'), -1);
});
