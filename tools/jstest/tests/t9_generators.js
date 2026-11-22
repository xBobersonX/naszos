// Generatory: function*, yield, yield*, next/return/throw, for-of, spread
var fails = 0, passed = 0;
function check(name, got, exp) {
  if (got === exp) passed++;
  else { fails++; console.log("FAIL " + name + "\n  got: " + got + "\n  exp: " + exp); }
}

function* count(n) { for (var i = 0; i < n; i++) yield i; return "koniec"; }
var g = count(2);
var a = g.next(), b = g.next(), c = g.next(), d = g.next();
check("kroki", [a.value, a.done, b.value, c.value, c.done, d.value, d.done].join(), "0,false,1,koniec,true,,true");
check("spread", [...count(4)].join(), "0,1,2,3");
check("Array.from", Array.from(count(3)).join(), "0,1,2");
var [x, y] = count(5);
check("destrukturyzacja", x + "," + y, "0,1");

// wartosc wyslana przez next(v)
function* echo() { var got = []; while (true) { var v = yield got.length; if (v === "stop") return got.join(); got.push(v); } }
var e = echo(); e.next(); e.next("a"); e.next("b");
check("next(v)", e.next("stop").value, "a,b");

// nieskonczony generator + break (for-of krok po kroku)
function* naturalne() { var n = 1; try { while (true) yield n++; } finally { naturalne.zamkniety = true; } }
var suma = 0;
for (var n of naturalne()) { if (n > 10) break; suma += n; }
check("nieskonczony + break", suma, 55);
check("break wola finally", naturalne.zamkniety, true);

// return() - finally tak, catch nie
var log = [];
function* zFinally() { try { yield 1; yield 2; } catch (e) { log.push("catch"); } finally { log.push("finally"); } }
var zf = zFinally(); zf.next();
var r = zf.return(42);
check("return()", r.value + "," + r.done, "42,true");
check("return() przez finally", log.join(), "finally");
check("po return", zf.next().done, true);

// throw() - lapane w generatorze
function* lapie() { while (true) { try { yield "czekam"; } catch (e) { yield "zlapano " + e; } } }
var lg = lapie(); lg.next();
check("throw()", lg.throw("blad").value, "zlapano blad");
var niezlapany = "";
try { count(3).throw(new Error("x")); } catch (er) { niezlapany = er.message; }
check("throw przed startem", niezlapany, "x");

// yield*
function* wew() { yield "a"; yield "b"; return "wynik"; }
function* zew() { var r = yield* wew(); yield r; yield* [1, 2]; yield* "xy"; }
check("yield*", [...zew()].join(), "a,b,wynik,1,2,x,y");

// metody generatorowe, Symbol.iterator, this
var obj = { dane: [3, 4], *[Symbol.iterator]() { for (var v of this.dane) yield v * 10; } };
check("obiekt iterowalny", [...obj].join(), "30,40");
class Drzewo {
  constructor(v, l, p) { this.v = v; this.l = l; this.p = p; }
  *wglab() { if (this.l) yield* this.l.wglab(); yield this.v; if (this.p) yield* this.p.wglab(); }
  static *zakres(a, b) { while (a < b) yield a++; }
}
var t = new Drzewo(2, new Drzewo(1), new Drzewo(3));
check("rekurencyjny yield*", [...t.wglab()].join(), "1,2,3");
check("static *", [...Drzewo.zakres(5, 8)].join(), "5,6,7");
var wyr = function* () { yield arguments.length; };
check("function* wyrazenie + arguments", wyr(1, 2, 3).next().value, 3);

// iterator generatora jest iterowalny, typy, new
var gg = count(1);
check("iterable", gg[Symbol.iterator]() === gg, true);
check("typeof", typeof count, "function");
var nowy = "";
try { new count(1); } catch (er) { nowy = er.name; }
check("new generator", nowy, "TypeError");
check("toString obiektu", typeof gg.next, "function");

// yield jako zwykla nazwa poza generatorem, yield bez wartosci, w wyrazeniach
var yield_ok = (function () { var yield = 5; return yield; })();
check("yield jako nazwa", yield_ok, 5);
function* pusty() { yield; yield (yield 1) + 1; }
var pg = pusty();
check("yield bez wartosci", pg.next().value, undefined);
pg.next();
check("yield w wyrazeniu", pg.next(10).value, 11);

// Map/Set z generatora, wiele generatorow naraz (pula stosow)
check("new Map(gen)", new Map((function* () { yield ["k", 1]; })()).get("k"), 1);
var gens = [];
for (var i = 0; i < 300; i++) { var gi = count(5); gi.next(); gens.push(gi); }
var s2 = 0;
for (var j = 0; j < gens.length; j++) for (var v of gens[j]) s2 += v;
check("300 naraz", s2, 300 * (1 + 2 + 3 + 4));

// generator + async
async function zAsync() { var out = []; for (var v of count(3)) out.push(await v); return out.join(); }
zAsync().then(function (v) {
  check("generator w async", v, "0,1,2");
  if (fails) console.log("FAILED generators: " + fails + " bledow, " + passed + " ok");
  else console.log("OK generators (" + passed + " checks)");
});
