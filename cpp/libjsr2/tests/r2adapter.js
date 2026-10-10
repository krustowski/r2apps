// r2adapter.js --- the host runner's natives, on js.elf's r2 object, so the
// same test files run on r2 (make test-r2).  Tests that need the host's
// scripted network (fetch, EventSource) stay on the host.
let __passed = 0, __failed = 0;
function __report(name, why) {
  if (why) { __failed++; console.log(`FAIL ${name}\n  ${why}`); }
  else { __passed++; console.log(`ok   ${name}`); }
}
function __finish() {
  console.log(`${__passed} passed, ${__failed} failed (heap ${r2.heap()} bytes)`);
  r2.exit(__failed ? 1 : 0);
}
function __probe(what) {
  if (what === 'error') return r2.lastError();
  if (what === 'log') return r2.log();
  if (what === 'heap') return r2.heap();
  return undefined;
}
