// async / await - kolejnosc jak w przegladarce, bledy, petle, metody, strzalki
var fails = 0, passed = 0;
function check(name, got, exp) {
  if (got === exp) passed++;
  else { fails++; console.log("FAIL " + name + "\n  got: " + got + "\n  exp: " + exp); }
}
var sleep = (ms, v) => new Promise(r => setTimeout(() => r(v), ms));

// 1. kolejnosc: czesc synchroniczna, await, mikrozadania
var log = [];
async function a1() { log.push("a1"); await null; log.push("a2"); await 0; log.push("a3"); }
a1();
Promise.resolve().then(() => log.push("p1")).then(() => log.push("p2"));
log.push("sync");

// 2. wynik i bledy
async function twice(x) { return (await x) * 2; }
async function boom() { throw new TypeError("zle"); }
async function boomLater() { await null; null.x; }
async function catcher() {
  try { await boom(); return "nie"; }
  catch (e) { return "zlapano " + e.name + ":" + e.message; }
  finally { log2.push("finally"); }
}
var log2 = [];
async function nested() {
  try {
    try { await Promise.reject(1); }
    catch (e) { throw e + 1; }
  } catch (e) { return e + 1; }
}

// 3. strzalki, metody, klasy, this
var arrow1 = async x => x + 1;
var arrow2 = async (a, b = 2, ...rest) => a + b + rest.length;
var arrow3 = async () => { await null; return "blok"; };
var obj = {
  base: 10,
  async add(n) { await null; return this.base + n; },
  async ["com" + "puted"]() { return "c"; },
  async: 5,                         // zwykla wlasciwosc o nazwie async
  get async2() { return 6; },
};
class Counter {
  constructor() { this.n = 0; }
  async inc() { await sleep(1); this.n++; return this.n; }
  static async make() { var c = new Counter(); await c.inc(); return c; }
  async lexical() { var f = async () => { await null; return this.n; }; return f(); }
}

// 4. petle
async function loops() {
  var s = 0;
  for (var i = 0; i < 5; i++) s += await i;
  var j = 0;
  while (j < 3) { s += await Promise.resolve(10); j++; }
  for (const v of [100, 200]) s += await sleep(1, v);
  for await (const v of [Promise.resolve(1000), 2000, sleep(2, 3000)]) s += v;
  do { s += 1; } while (await false);
  return s;
}

// 5. wywolania zagniezdzone i rekurencja
async function fib(n) { if (n < 2) return n; return (await fib(n - 1)) + (await fib(n - 2)); }
async function chain(n) { if (!n) return 0; await null; return 1 + await chain(n - 1); }
function deep(n) { return n ? deep(n - 1) + 1 : 0; }
async function deepInside() { await null; return deep(300); }
async function overflow() { await null; function r() { return r() + 1; } try { r(); } catch (e) { return e.name; } }

// 6. await w roznych miejscach wyrazen
async function places() {
  var arr = [await 1, await 2];
  var o = { k: await "v" };
  var t = `${await "a"}-${await "b"}`;
  var c = (await true) ? await "tak" : "nie";
  var f = Math.max(await 3, await 7);
  var u = typeof await 5;
  var m = -await 4;
  return [arr.join(), o.k, t, c, f, u, m].join("|");
}

// 7. wiele naraz, Promise.all, arguments
async function many() {
  var r = await Promise.all([1, 2, 3, 4, 5].map(async (x, i) => { await sleep(5 - i); return x * x; }));
  return r.join();
}
async function lots() {
  var ps = [];
  for (var i = 0; i < 150; i++) ps.push((async k => { await sleep(1); return k; })(i));
  var r = await Promise.all(ps);
  return r.reduce((a, b) => a + b, 0);
}
async function withArgs() { await null; return arguments.length + ":" + arguments[1]; }
async function longLoop() { var s = 0; for (var i = 0; i < 20000; i++) s += await i; return s; }

// 8. async i await jako zwykle nazwy (poza funkcjami async)
var async = function (x) { return "fn" + x; };
var await = 3;
check("async jako nazwa", async(2), "fn2");
check("await jako nazwa", await + 1, 4);
check("typeof", typeof twice, "function");
var threw = "";
try { new twice(1); } catch (e) { threw = e.name; }
check("new async", threw, "TypeError");
check("wynik to Promise", twice(1) instanceof Promise, true);
var syncThrow = "nie";
try { boom().catch(() => {}); } catch (e) { syncThrow = "tak"; }
check("blad nie wylatuje synchronicznie", syncThrow, "nie");

(async () => {
  await sleep(0);
  check("kolejnosc", log.join(), "a1,sync,a2,p1,a3,p2");
  check("twice", await twice(21), 42);
  check("twice z obietnica", await twice(sleep(3, 5)), 10);
  check("catcher", await catcher(), "zlapano TypeError:zle");
  check("finally", log2.join(), "finally");
  check("nested", await nested(), 3);
  var e1 = await boomLater().catch(e => e.name);
  check("blad po await", e1, "TypeError");
  check("arrow1", await arrow1(1), 2);
  check("arrow2", await arrow2(1, undefined, 7, 8), 5);
  check("arrow3", await arrow3(), "blok");
  check("metoda this", await obj.add(5), 15);
  check("metoda computed", await obj.computed(), "c");
  check("wlasciwosc async", obj.async + obj.async2, 11);
  var c = await Counter.make();
  check("static async", c.n, 1);
  check("inc", await c.inc(), 2);
  check("strzalka this", await c.lexical(), 2);
  check("petle", await loops(), 6000 + 300 + 30 + 10 + 1);
  check("fib", await fib(12), 144);
  check("chain", await chain(60), 60);
  check("rekurencja w async", await deepInside(), 300);
  check("przepelnienie stosu", await overflow(), "RangeError");
  check("miejsca", await places(), "1,2|v|a-b|tak|7|number|-4");
  check("many", await many(), "1,4,9,16,25");
  check("lots", await lots(), 149 * 150 / 2);
  check("arguments", await withArgs(1, "x", 3), "3:x");
  check("dluga petla", await longLoop(), 20000 * 19999 / 2);
  var thenable = { then(r) { r("z thenable"); } };
  check("await thenable", await thenable, "z thenable");
  check("return obietnicy", await (async () => sleep(2, "pozno"))(), "pozno");
  var order = [];
  var pa = (async () => { await sleep(10); order.push("A"); })();
  var pb = (async () => { await sleep(1); order.push("B"); })();
  await pa; await pb;
  check("rownolegle", order.join(), "B,A");
  if (fails) console.log("FAILED await: " + fails + " bledow, " + passed + " ok");
  else console.log("OK await (" + passed + " checks)");
})().catch(e => console.log("FAILED await - wyjatek: " + e + "\n" + e.stack));
