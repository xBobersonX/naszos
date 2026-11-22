// Biblioteka: tablice, teksty, regexy, JSON, Date, Map/Set, bledy
var fails = 0, count = 0;
function eq(a, b, msg) {
  count++;
  if (a !== b && !(a !== a && b !== b)) {
    fails++;
    console.log("FAIL " + msg + ": got " + JSON.stringify(a) + " expected " + JSON.stringify(b));
  }
}

// tablice
var arr = [3, 1, 4, 1, 5, 9, 2, 6];
eq(arr.length, 8, "length");
eq(arr.indexOf(1), 1, "indexOf");
eq(arr.lastIndexOf(1), 3, "lastIndexOf");
eq(arr.includes(9), true, "includes");
eq(arr.slice(2, 4).join(), "4,1", "slice");
eq(arr.slice(-2).join(), "2,6", "slice neg");
eq([...arr].sort().join(), "1,1,2,3,4,5,6,9", "sort default");
eq([10, 9, 1, 100].sort().join(), "1,10,100,9", "sort strings");
eq([10, 9, 1, 100].sort((a, b) => a - b).join(), "1,9,10,100", "sort numeric");
eq(arr.map(x => x * 2).join(), "6,2,8,2,10,18,4,12", "map");
eq(arr.filter(x => x > 3).join(), "4,5,9,6", "filter");
eq(arr.reduce((a, b) => a + b, 0), 31, "reduce");
eq(arr.reduce((a, b) => a + b), 31, "reduce no init");
eq(arr.find(x => x > 4), 5, "find");
eq(arr.findIndex(x => x > 4), 4, "findIndex");
eq(arr.some(x => x > 8), true, "some");
eq(arr.every(x => x > 0), true, "every");
var sp = [1, 2, 3, 4, 5];
var removed = sp.splice(1, 2, "a", "b", "c");
eq(sp.join(), "1,a,b,c,4,5", "splice");
eq(removed.join(), "2,3", "splice removed");
var pp = [1, 2];
pp.push(3, 4);
eq(pp.pop(), 4, "pop");
eq(pp.shift(), 1, "shift");
pp.unshift(0);
eq(pp.join("-"), "0-2-3", "unshift/join");
eq([1, [2, [3, [4]]]].flat(Infinity).join(), "1,2,3,4", "flat");
eq([1, 2].flatMap(x => [x, x * 10]).join(), "1,10,2,20", "flatMap");
eq(Array.from("abc").join(), "a,b,c", "Array.from string");
eq(Array.from({ length: 3 }, (_, i) => i * i).join(), "0,1,4", "Array.from length");
eq(Array.isArray([]), true, "isArray");
eq(new Array(3).length, 3, "new Array(n)");
eq(Array(3).fill(7).join(), "7,7,7", "fill");
eq([1, 2, 3].reverse().join(), "3,2,1", "reverse");
eq([1, 2].concat([3], 4).join(), "1,2,3,4", "concat");
eq([1, 2, 3].at(-1), 3, "at");
eq([3, 1, 2].toString(), "3,1,2", "toString");
var holes = [];
holes[5] = 1;
eq(holes.length, 6, "sparse length");
var trunc = [1, 2, 3, 4];
trunc.length = 2;
eq(trunc.join(), "1,2", "length truncate");
eq([..."héllo"].length, 5, "spread string");
eq([...[1, 2, 3].entries()].map(e => e.join(":")).join(), "0:1,1:2,2:3", "entries");
var fe = 0;
[1, 2, 3].forEach(function (v) { fe += v * this.m; }, { m: 10 });
eq(fe, 60, "forEach thisArg");
var objs = [{ n: "b", v: 2 }, { n: "a", v: 2 }, { n: "c", v: 1 }];
eq(objs.sort((x, y) => x.v - y.v).map(o => o.n).join(""), "cba", "stable sort");

// teksty
var s = "Hello, World";
eq(s.length, 12, "str length");
eq(s.toUpperCase(), "HELLO, WORLD", "toUpperCase");
eq(s.indexOf("o"), 4, "str indexOf");
eq(s.lastIndexOf("o"), 8, "str lastIndexOf");
eq(s.slice(-5), "World", "str slice");
eq(s.substring(7, 5), ", ", "substring swap");
eq(s.substring(5, 7), ", ", "substring");
eq(s.substr(7, 3), "Wor", "substr");
eq(s.charAt(1), "e", "charAt");
eq(s.charCodeAt(0), 72, "charCodeAt");
eq(s[4], "o", "index");
eq(s.split(", ").join("|"), "Hello|World", "split");
eq("a,b,,c".split(",").length, 4, "split empty");
eq("abc".split("").join("-"), "a-b-c", "split chars");
eq("  pad ".trim(), "pad", "trim");
eq("5".padStart(3, "0"), "005", "padStart");
eq("ab".padEnd(5, "xy"), "abxyx", "padEnd");
eq("ab".repeat(3), "ababab", "repeat");
eq("Hello".startsWith("He"), true, "startsWith");
eq("Hello".endsWith("lo"), true, "endsWith");
eq("Hello".includes("ell"), true, "str includes");
eq("a-b-c".replace("-", "+"), "a+b-c", "replace str");
eq("a-b-c".replaceAll("-", "+"), "a+b+c", "replaceAll");
eq(String.fromCharCode(72, 105), "Hi", "fromCharCode");
eq("zażółć gęślą jaźń".length, 17, "polish length");
eq("ŻÓŁW".toLowerCase(), "żółw", "polish lower");
eq("łódź".toUpperCase(), "ŁÓDŹ", "polish upper");
eq("😀".length, 2, "surrogate length");
eq("😀".codePointAt(0), 128512, "codePointAt");
eq(String.fromCodePoint(128512), "😀", "fromCodePoint");
eq(["b", "a", "ą", "c"].sort((x, y) => x.localeCompare(y)).join(""), "aąbc", "localeCompare pl");
eq("abc".at(-1), "c", "str at");
eq("x".concat(1, 2), "x12", "str concat");

// wyrazenia regularne
eq(/ab+c/.test("xabbbcx"), true, "regex test");
eq(/^\d{3}-\d{4}$/.test("555-1234"), true, "regex digits");
eq(/colou?r/i.test("COLOR"), true, "regex icase");
var m = "2024-03-15".match(/(\d+)-(\d+)-(\d+)/);
eq(m[0] + "|" + m[1] + "|" + m[3] + "|" + m.index, "2024-03-15|2024|15|0", "match groups");
eq("a1b22c333".match(/\d+/g).join(), "1,22,333", "match global");
eq("a1b22c333".replace(/\d+/g, "#"), "a#b#c#", "replace regex");
eq("John Smith".replace(/(\w+)\s(\w+)/, "$2, $1"), "Smith, John", "replace groups");
eq("abc".replace(/b/, m => m.toUpperCase()), "aBc", "replace fn");
eq("a, b;c".split(/[,;]\s*/).join("|"), "a|b|c", "split regex");
eq("one two  three".split(/\s+/).length, 3, "split ws");
var re = /o/g, found = [], mm;
while ((mm = re.exec("foo boo")) !== null) found.push(mm.index);
eq(found.join(), "1,2,5,6", "exec loop");
eq(/(?<year>\d{4})/.exec("in 1999").groups.year, "1999", "named group");
eq("aaa".replace(/a*?/g, "-"), "-a-a-a-", "lazy empty");
eq(/^(?:(a)|b)+$/.test("abab"), true, "alternation loop");
eq(/(\w)\1/.test("hello"), true, "backref");
eq(/foo(?=bar)/.test("foobar"), true, "lookahead");
eq(/foo(?!bar)/.test("foobar"), false, "neg lookahead");
eq(/(?<=\$)\d+/.exec("cost $42")[0], "42", "lookbehind");
eq("x.y.z".split(".").length, 3, "split dot");
eq(/[^a-c]+/.exec("abcdef")[0], "def", "negated class");
eq(/\bcat\b/.test("a cat!"), true, "word boundary");
eq(/a.c/s.test("a\nc"), true, "dotAll");
eq(/^b/m.test("a\nb"), true, "multiline");
eq(String(/a\/b/g), "/a\\/b/g", "regex toString");
eq("Tel: 12 345".replace(/\D/g, ""), "12345", "non-digit");
eq("CamelCaseString".replace(/([A-Z])/g, " $1").trim(), "Camel Case String", "insert spaces");
eq("aBc".search(/[A-Z]/), 1, "search");
eq([..."a1b2".matchAll(/\d/g)].map(x => x[0]).join(), "1,2", "matchAll");
var catastrophic = /^(a+)+$/.test("aaaaaaaaaaaaaaaaaaaa!");
eq(catastrophic, false, "backtracking finishes");

// JSON
var jo = { a: 1, b: [1, 2, { c: "x" }], d: null, e: true, f: "q\"uote\n" };
var js = JSON.stringify(jo);
eq(js, '{"a":1,"b":[1,2,{"c":"x"}],"d":null,"e":true,"f":"q\\"uote\\n"}', "stringify");
eq(JSON.stringify(JSON.parse(js)), js, "roundtrip");
eq(JSON.stringify({ u: undefined, fn() {}, n: NaN }), '{"n":null}', "stringify skip");
eq(JSON.stringify([1, 2], null, 2), "[\n  1,\n  2\n]", "stringify indent");
eq(JSON.parse("[1, 2.5e1, \"\\u0041\"]")[2], "A", "parse unicode");
eq(JSON.parse('{"x": {"y": [true, false, null]}}').x.y[1], false, "parse nested");
var jerr = false;
try { JSON.parse("{bad}"); } catch (e) { jerr = e instanceof SyntaxError; }
eq(jerr, true, "parse error");
eq(JSON.stringify({ date: new Date(0) }), '{"date":"1970-01-01T00:00:00.000Z"}', "stringify date");
eq(JSON.stringify("ą"), '"ą"', "stringify polish");
eq(JSON.parse('{"a":1}', (k, v) => typeof v === "number" ? v * 2 : v).a, 2, "reviver");

// Date
var dt = new Date(2024, 0, 31, 10, 30, 15);
eq(dt.getFullYear(), 2024, "date year");
eq(dt.getMonth(), 0, "date month");
eq(dt.getDate(), 31, "date date");
eq(dt.getHours(), 10, "date hours");
eq(dt.getDay(), 3, "date weekday");
dt.setMonth(1);
eq(dt.getMonth() + "/" + dt.getDate(), "2/2", "month overflow");
eq(new Date("2024-03-15T12:00:00Z").toISOString(), "2024-03-15T12:00:00.000Z", "ISO roundtrip");
eq(new Date(Date.UTC(2020, 1, 29)).getUTCDate(), 29, "leap day");
eq(new Date("2024-01-01").getTime(), 1704067200000, "date-only is UTC");
eq(typeof Date.now(), "number", "Date.now");
eq(Date.now() > 1600000000000, true, "Date.now sane");
eq(new Date(2024, 6, 1).getTimezoneOffset(), -120, "summer offset");
eq(new Date(2024, 0, 1).getTimezoneOffset(), -60, "winter offset");
eq(new Date(2024, 2, 5).toLocaleDateString("pl-PL"), "05.03.2024", "pl date");
eq(new Date(2024, 2, 5, 9, 7, 3).toLocaleTimeString("pl-PL"), "09:07:03", "pl time");
eq(new Date(2024, 2, 5).toLocaleDateString("pl-PL", { day: "numeric", month: "long", year: "numeric" }), "5 marca 2024", "pl long date");
eq(new Date(2024, 2, 5).toLocaleDateString("en-US"), "3/5/2024", "en date");
eq(new Date("invalid").getTime(), NaN, "invalid date");
eq(String(new Date(NaN)), "Invalid Date", "invalid string");
eq(new Date("March 7, 2021").getDate(), 7, "parse month name");
eq(new Date(2000, 0, 1) < new Date(2001, 0, 1), true, "date compare");
eq(new Date(86400000) - new Date(0), 86400000, "date subtract");

// Map, Set
var map = new Map([["a", 1], ["b", 2]]);
map.set("c", 3).set(NaN, "nan");
eq(map.size, 4, "map size");
eq(map.get("b"), 2, "map get");
eq(map.get(NaN), "nan", "map NaN key");
eq(map.has("z"), false, "map has");
map.delete("a");
eq([...map.keys()].join(), "b,c,NaN", "map keys order");
var kk = [];
map.forEach((v, k) => kk.push(k + "=" + v));
eq(kk.join(";"), "b=2;c=3;NaN=nan", "map forEach");
var objKey = {};
map.set(objKey, "obj");
eq(map.get(objKey), "obj", "map object key");
var set = new Set([1, 2, 2, 3, 3, 3]);
eq(set.size, 3, "set size");
eq([...set].join(), "1,2,3", "set spread");
set.add(4);
eq(set.has(4), true, "set has");
eq([...new Set("hello")].join(""), "helo", "set string");
var big = new Map();
for (var i = 0; i < 5000; i++) big.set("k" + i, i);
eq(big.get("k4321"), 4321, "big map");

// bledy
try { null.x; } catch (e) { eq(e instanceof TypeError, true, "null prop TypeError"); eq(e.message, "Cannot read properties of null (reading 'x')", "null prop message"); }
try { undefinedFn(); } catch (e) { eq(e.name, "ReferenceError", "ref error"); eq(e.message, "undefinedFn is not defined", "ref message"); }
try { var q = {}; q.nope(); } catch (e) { eq(e.message, "q.nope is not a function", "not a function message"); }
try { throw new RangeError("zly zakres"); } catch (e) { eq(String(e), "RangeError: zly zakres", "throw custom"); }
var fin = [];
function tf() { try { fin.push(1); return "r"; } finally { fin.push(2); } }
eq(tf() + fin.join(), "r1,2", "finally with return");
function tf2() { try { throw 1; } catch (e) { return "c" + e; } finally { fin.push(3); } }
eq(tf2(), "c1", "catch return");
var nested = "";
try { try { throw "in"; } finally { nested += "f"; } } catch (e) { nested += e; }
eq(nested, "fin", "nested try");
try { throw { code: 42 }; } catch ({ code }) { eq(code, 42, "catch destructuring"); }
eq(new Error("m", { cause: "c" }).cause, "c", "error cause");

// switch
function sw(x) {
  switch (x) {
    case 1: return "one";
    case "1": return "str";
    case 2:
    case 3: return "two-three";
    default: return "other";
  }
}
eq(sw(1) + sw("1") + sw(3) + sw(9), "onestrtwo-threeother", "switch");
var fall = "";
switch (2) { case 1: fall += "a"; case 2: fall += "b"; case 3: fall += "c"; break; case 4: fall += "d"; }
eq(fall, "bc", "switch fallthrough");

// petle
var sum = 0;
for (var k in { a: 1, b: 2, c: 3 }) sum += k.length;
eq(sum, 3, "for-in");
var keys = [];
for (var ix in [7, 8, 9]) keys.push(ix);
eq(keys.join(), "0,1,2", "for-in array");
var w = 0, dw = 0;
while (w < 10) w += 3;
do { dw++; } while (dw < 0);
eq(w + dw, 13, "while/do");
var ofs = "";
for (const ch of "ab") ofs += ch;
for (const [kk2, vv] of new Map([["x", 1]])) ofs += kk2 + vv;
eq(ofs, "abx1", "for-of");

// Object.*
var ob = { b: 2, a: 1, 1: "x", 0: "y" };
eq(Object.keys(ob).join(), "0,1,b,a", "key order");
eq(Object.values({ x: 1, y: 2 }).join(), "1,2", "values");
eq(Object.entries({ x: 1 })[0].join(), "x,1", "entries obj");
eq(Object.assign({}, { a: 1 }, { b: 2 }).b, 2, "assign");
var fr = Object.freeze({ z: 1 });
fr.z = 2;
eq(fr.z, 1, "freeze");
var dp = {};
Object.defineProperty(dp, "hidden", { value: 5, enumerable: false });
eq(dp.hidden + Object.keys(dp).length, 5, "defineProperty");
Object.defineProperty(dp, "comp", { get() { return 9; } });
eq(dp.comp, 9, "defineProperty getter");
var proto = { hi() { return "hi"; } };
var child = Object.create(proto);
eq(child.hi(), "hi", "Object.create");
eq("hi" in child, true, "in proto");
eq(child.hasOwnProperty("hi"), false, "hasOwnProperty");
eq(Object.fromEntries([["a", 1]]).a, 1, "fromEntries");
eq(Object.prototype.toString.call([]), "[object Array]", "toString tag");
eq(Object.is(NaN, NaN), true, "Object.is");
eq(Object.getOwnPropertyNames([1]).join(), "0,length", "own names array");

// iteratory z next() (kod stron po Babel/TypeScript wola next() recznie)
var ia = [1, 2], it = ia[Symbol.iterator](), r1 = it.next();
ia.push(3);
eq(typeof it.next, "function", "array iterator next");
eq(r1.value + ":" + r1.done, "1:false", "iterator result");
eq([...it].join(), "2,3", "iterator live + spread rest");
eq(it.next().done, true, "iterator done");
eq(it[Symbol.iterator]() === it, true, "iterator is iterable");
eq([...[7, 8].entries()].join("|"), "0,7|1,8", "entries");
eq(Array.from(["a", "b"].keys()).join(), "0,1", "keys");
var mi = new Map([["k", 1]]).entries().next();
eq(mi.value.join() + mi.done, "k,1false", "map entries next");
eq(new Set([5]).values().next().value, 5, "set values next");
eq("ab"[Symbol.iterator]().next().value, "a", "string iterator next");
var manual = [], mit = new Map([[1, "a"], [2, "b"]]).keys(), step;
while (!(step = mit.next()).done) manual.push(step.value);
eq(manual.join(), "1,2", "manual loop");
var got = [];
for (var [k, v] of new Map([["x", 1]]).entries()) got.push(k + v);
eq(got.join(), "x1", "for-of over iterator");

done();
function done() { console.log((fails ? "FAILED " + fails : "OK") + " library (" + count + " tests)"); }
