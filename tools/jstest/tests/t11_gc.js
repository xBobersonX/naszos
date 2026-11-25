// Sprzatanie pamieci (GC): duzo smieci, a to, co zywe, musi przetrwac nienaruszone.
// Najlepiej uruchamiac tez z -gc N (sprzatanie co N alokacji).
var fails = 0, passed = 0;
function check(name, got, exp) {
  if (got === exp) passed++;
  else { fails++; console.log("FAIL " + name + "\n  got: " + got + "\n  exp: " + exp); }
}

// 1. zywa lista + smieci po drodze
var keep = [];
for (var i = 0; i < 20000; i++) {
  var junk = { a: i, b: "x" + i, c: [i, i + 1] };
  if (i % 100 === 0) keep.push({ n: i, s: "k" + i, arr: [i * 2] });
}
var ok = keep.length === 200;
for (var i = 0; i < keep.length; i++)
  if (keep[i].n !== i * 100 || keep[i].s !== "k" + i * 100 || keep[i].arr[0] !== i * 200) ok = false;
check("lista przetrwala", ok, true);

// 2. domkniecia trzymaja srodowisko
function counter() { var c = 0; return function () { return ++c; }; }
var cs = [];
for (var i = 0; i < 50; i++) cs.push(counter());
for (var r = 0; r < 200; r++) for (var i = 0; i < 50; i++) cs[i]();
check("domkniecia", cs[7](), 201);

// 3. dlugie teksty sklejane i wielkie tablice (duze bloki)
var s = "";
for (var i = 0; i < 3000; i++) s += String.fromCharCode(65 + i % 26);
check("tekst", s.length + ":" + s.slice(0, 5) + s.slice(-3), "3000:ABCDEHIJ");
var big = new Array(10000).fill(0).map(function (_, k) { return k * k; });
for (var i = 0; i < 30; i++) new Array(5000).fill(i);
check("duza tablica", big[9999], 99980001);

// 4. Map / Set z obiektami jako kluczami
var m = new Map(), keys = [];
for (var i = 0; i < 500; i++) { var k = { id: i }; keys.push(k); m.set(k, "v" + i); }
for (var i = 0; i < 5000; i++) ({ smiec: i });
check("Map", m.get(keys[321]), "v321");

// 5. JSON w kolko
var acc = 0;
for (var i = 0; i < 300; i++) acc += JSON.parse(JSON.stringify({ a: [1, 2, { b: i }] })).a[2].b;
check("JSON", acc, 299 * 300 / 2);

// 6. obiekt z wieloma wlasciwosciami (tablica mieszajaca) i wyrazenia regularne
var h = {};
for (var i = 0; i < 300; i++) h["p" + i] = i;
for (var i = 0; i < 2000; i++) "a1b22c333".replace(/\d+/g, function (x) { return "<" + x + ">"; });
check("hash", h.p250 + h.p3, 253);
check("regex", "a1b22".replace(/\d+/g, "#"), "a#b#");

// 7. async / timery po sprzataniu
var log = [];
async function aw(v) { await null; for (var i = 0; i < 2000; i++) ({ x: i }); return v * 2; }
setTimeout(function () {
  var t = { msg: "timer" };
  for (var i = 0; i < 3000; i++) [i];
  log.push(t.msg);
}, 1);
aw(21).then(function (v) {
  log.push("async " + v);
  setTimeout(function () {
    check("async+timer", log.join(","), "async 42,timer");
    if (fails) console.log("FAILED gc: " + fails + " bledow, " + passed + " ok");
    else console.log("OK gc (" + passed + " checks)");
  }, 20);
});
