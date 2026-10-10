// harness.js --- the little test framework every *.test.js runs under.
// test(name, fn) registers a test (fn may be async); __run() runs them in
// order and reports each to the C++ runner.
const __tests = [];
function test(name, fn) { __tests.push([name, fn]); }
function assert(cond, message) { if (!cond) throw new Error(message || 'assertion failed'); }
function eq(actual, expected, message) {
  const same = actual === expected || (actual !== actual && expected !== expected);
  if (!same) throw new Error(`${message ? message + ': ' : ''}expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}
function near(actual, expected, message) {
  if (!(Math.abs(actual - expected) <= Math.abs(expected) * 4e-16)) throw new Error(`${message ? message + ': ' : ''}expected ${expected}, got ${actual}`);
}
function deepEq(actual, expected, message) {
  const a = JSON.stringify(actual), b = JSON.stringify(expected);
  if (a !== b) throw new Error(`${message ? message + ': ' : ''}expected ${b}, got ${a}`);
}
async function throwsAsync(fn, check) {
  try { await fn(); } catch (e) { if (check) check(e); return; }
  throw new Error('expected an exception');
}
function throws(fn, check) {
  try { fn(); } catch (e) { if (check) check(e); return; }
  throw new Error('expected an exception');
}
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
function waitFor(cond, ms = 2000) {
  return new Promise((resolve, reject) => {
    const until = performance.now() + ms;
    (function poll() {
      if (cond()) resolve();
      else if (performance.now() > until) reject(new Error('timed out waiting'));
      else setTimeout(poll, 2);
    })();
  });
}
async function __run() {
  for (const [name, fn] of __tests) {
    try { await fn(); __report(name, ''); }
    catch (e) { __report(name, String(e && e.stack ? `${e.message}\n${e.stack}` : e)); }
  }
  __finish();
}
