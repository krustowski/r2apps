test('parse and serialise', () => {
  const u = new URL('https://user:pw@Example.COM:8080/a/./b/../c%20d?q=1&r=a+b#frag');
  eq(u.protocol, 'https:'); eq(u.username, 'user'); eq(u.password, 'pw');
  eq(u.hostname, 'example.com'); eq(u.port, '8080'); eq(u.host, 'example.com:8080');
  eq(u.pathname, '/a/c%20d'); eq(u.search, '?q=1&r=a+b'); eq(u.hash, '#frag');
  eq(u.origin, 'https://example.com:8080');
  eq(u.href, 'https://user:pw@example.com:8080/a/c%20d?q=1&r=a+b#frag');
  eq(new URL('http://x.org:80/').href, 'http://x.org/');
  eq(new URL('HTTP://x.org').href, 'http://x.org/');
  eq(new URL('http://x.org/a b').pathname, '/a%20b');
  eq(new URL('http://x.org/ž').pathname, '/%C5%BE');
  eq(new URL('http://0x7f.1/').hostname, '127.0.0.1');
  eq(new URL('mailto:me@x.org').pathname, 'me@x.org');
  eq(new URL('data:text/plain,hi').protocol, 'data:');
  eq(new URL('file:/mnt/fat/a.htm').href, 'file:///mnt/fat/a.htm');
  eq(new URL('b.js', 'file:/mnt/fat/a.htm').href, 'file:///mnt/fat/b.js');
  eq(new URL('file:///mnt/x').pathname, '/mnt/x');
  eq(new URL('file://srv/share').hostname, 'srv');
  throws(() => new URL('not a url'), e => assert(e instanceof TypeError));
  throws(() => new URL('http://'));
  eq(URL.canParse('/rel', 'http://a/'), true);
  eq(URL.canParse('/rel'), false);
});
test('relative resolution (RFC 3986 examples)', () => {
  const base = 'http://a/b/c/d;p?q';
  const cases = {
    'g': 'http://a/b/c/g', './g': 'http://a/b/c/g', 'g/': 'http://a/b/c/g/', '/g': 'http://a/g',
    '//g': 'http://g/', '?y': 'http://a/b/c/d;p?y', 'g?y': 'http://a/b/c/g?y', '#s': 'http://a/b/c/d;p?q#s',
    'g#s': 'http://a/b/c/g#s', ';x': 'http://a/b/c/;x', '': 'http://a/b/c/d;p?q', '.': 'http://a/b/c/',
    './': 'http://a/b/c/', '..': 'http://a/b/', '../': 'http://a/b/', '../g': 'http://a/b/g',
    '../..': 'http://a/', '../../g': 'http://a/g', '../../../g': 'http://a/g', '/./g': 'http://a/g',
    'g.': 'http://a/b/c/g.', '..g': 'http://a/b/c/..g', './../g': 'http://a/b/g', 'g;x=1/../y': 'http://a/b/c/y',
    'http:g': 'http://a/b/c/g',
  };
  for (const [rel, want] of Object.entries(cases)) eq(new URL(rel, base).href, want, rel);
});
test('setters', () => {
  const u = new URL('http://a.org/x?y=1#z');
  u.pathname = 'p q'; eq(u.href, 'http://a.org/p%20q?y=1#z');
  u.search = 'k=v'; eq(u.search, '?k=v');
  u.hash = ''; eq(u.href, 'http://a.org/p%20q?k=v');
  u.port = '8080'; u.hostname = 'B.org'; eq(u.host, 'b.org:8080');
  u.protocol = 'https'; eq(u.href, 'https://b.org:8080/p%20q?k=v');
  u.searchParams.append('n', 'a b');
  eq(u.search, '?k=v&n=a+b');
});
test('URLSearchParams', () => {
  const p = new URLSearchParams('?a=1&b=%C5%BE&a=3&c=x+y&d');
  eq(p.get('a'), '1'); deepEq(p.getAll('a'), ['1', '3']); eq(p.get('b'), 'ž'); eq(p.get('c'), 'x y'); eq(p.get('d'), '');
  p.set('a', '9'); p.delete('d'); p.append('e', '&=');
  eq(p.toString(), 'a=9&b=%C5%BE&c=x+y&e=%26%3D');
  eq(p.size, 4);
  p.sort(); deepEq([...p.keys()], ['a', 'b', 'c', 'e']);
  eq(new URLSearchParams({ x: 1, y: 'z' }).toString(), 'x=1&y=z');
  eq(new URLSearchParams([['q', 'r']]).toString(), 'q=r');
});
