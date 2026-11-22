// Funkcje, domkniecia, this, klasy, destrukturyzacja
var fails = 0, count = 0;
function eq(a, b, msg) {
  count++;
  if (a !== b && !(a !== a && b !== b)) {
    fails++;
    console.log("FAIL " + msg + ": got " + JSON.stringify(a) + " expected " + JSON.stringify(b));
  }
}

function add(a, b) { return a + b; }
eq(add(2, 3), 5, "call");
eq(hoisted(), "ok", "hoisting");
function hoisted() { return "ok"; }
eq(typeof laterVar, "undefined", "var hoisting");
var laterVar = 1;

function counter() { var n = 0; return function () { return ++n; }; }
var c1 = counter(), c2 = counter();
c1(); c1();
eq(c1(), 3, "closure");
eq(c2(), 1, "closure separate");

function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); }
eq(fact(10), 3628800, "recursion");
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
eq(fib(20), 6765, "fib");

function args() { return arguments.length + ":" + arguments[1]; }
eq(args(1, 2, 3), "3:2", "arguments");
function defs(a, b = a * 2, c = b + 1) { return a + b + c; }
eq(defs(1), 6, "defaults");
eq(defs(1, 5), 12, "defaults override");
function rest(a, ...r) { return a + r.length; }
eq(rest(1, 2, 3, 4), 4, "rest");
eq(Math.max(...[1, 9, 3]), 9, "spread call");
eq(add(...[4, 5]), 9, "spread call 2");

var arrow = (x, y) => x * y;
eq(arrow(3, 4), 12, "arrow");
var arrowBlock = x => { return x + 1; };
eq(arrowBlock(1), 2, "arrow block");
var obj = { v: 42, get() { return () => this.v; } };
eq(obj.get()(), 42, "arrow this");
var objLit = () => ({ a: 1 });
eq(objLit().a, 1, "arrow object literal");

var o = { name: "x", greet: function () { return "hi " + this.name; } };
eq(o.greet(), "hi x", "method this");
var g = o.greet;
eq(g.call({ name: "y" }), "hi y", "call");
eq(g.apply({ name: "z" }, []), "hi z", "apply");
var bound = g.bind({ name: "w" });
eq(bound(), "hi w", "bind");
function partial(a, b, c) { return a + b + c; }
eq(partial.bind(null, 1, 2)(3), 6, "bind args");
eq(partial.length, 3, "fn length");
eq(partial.name, "partial", "fn name");
var anon = function () {};
eq(anon.name, "anon", "inferred name");

var named = function inner(n) { return n ? inner(n - 1) + 1 : 0; };
eq(named(5), 5, "named fn expr");

// IIFE
eq((function () { return 7; })(), 7, "iife");
eq((() => 8)(), 8, "arrow iife");

// new, prototypes
function Point(x, y) { this.x = x; this.y = y; }
Point.prototype.len = function () { return Math.sqrt(this.x * this.x + this.y * this.y); };
var p = new Point(3, 4);
eq(p.len(), 5, "prototype method");
eq(p instanceof Point, true, "instanceof");
eq(p.constructor === Point, true, "constructor");
eq(Object.getPrototypeOf(p) === Point.prototype, true, "getPrototypeOf");

// klasy
class Animal {
  constructor(name) { this.name = name; }
  speak() { return this.name + " makes a sound"; }
  get upper() { return this.name.toUpperCase(); }
  static create(n) { return new this(n); }
}
class Dog extends Animal {
  constructor(name) { super(name); this.kind = "dog"; }
  speak() { return super.speak() + " (woof)"; }
}
var d = new Dog("Rex");
eq(d.speak(), "Rex makes a sound (woof)", "super method");
eq(d.upper, "REX", "getter");
eq(d instanceof Animal, true, "class instanceof");
eq(Dog.create("Ace").speak(), "Ace makes a sound (woof)", "static inherited");
eq(typeof Dog, "function", "class typeof");
class Fields { x = 1; y = this.x + 1; static s = 5; #p = 9; getP() { return this.#p; } }
var f = new Fields();
eq(f.y, 2, "class fields");
eq(Fields.s, 5, "static field");
eq(f.getP(), 9, "private field");
class NoCtor extends Animal {}
eq(new NoCtor("n").name, "n", "default derived ctor");
var threw = false;
try { Animal("x"); } catch (e) { threw = e instanceof TypeError; }
eq(threw, true, "class call without new");
class MyErr extends Error { constructor(m) { super(m); this.name = "MyErr"; } }
var me = new MyErr("bad");
eq(me.message, "bad", "extends Error message");
eq(me instanceof Error, true, "extends Error instanceof");
eq(String(me), "MyErr: bad", "extends Error toString");

// destrukturyzacja
var [a1, b1 = 5, ...r1] = [1, undefined, 3, 4];
eq(a1 + b1 + r1.length, 8, "array destructuring");
var { x: px, y: py = 10, ...others } = { x: 1, z: 3, w: 4 };
eq(px + py, 11, "object destructuring");
eq(Object.keys(others).join(), "z,w", "object rest");
function dparam({ a, b } = {}, [c, d] = [0, 0]) { return (a || 0) + (b || 0) + c + d; }
eq(dparam({ a: 1, b: 2 }, [3, 4]), 10, "param destructuring");
eq(dparam(), 0, "param destructuring defaults");
var sw1 = 1, sw2 = 2;
[sw1, sw2] = [sw2, sw1];
eq(sw1 * 10 + sw2, 21, "swap");
var { a: { b: deep } } = { a: { b: 7 } };
eq(deep, 7, "nested destructuring");
for (var [k, v] of [["a", 1], ["b", 2]]) { }
eq(k + v, "b2", "for-of destructuring");

// getter/setter w literale
var gs = { _v: 1, get v() { return this._v * 10; }, set v(n) { this._v = n; } };
gs.v = 5;
eq(gs.v, 50, "get/set literal");
var comp = "dyn";
var lit = { [comp + "Key"]: 1, short: 2, "quoted key": 3, 4: "four" };
eq(lit.dynKey + lit.short + lit["quoted key"], 6, "literal keys");
eq(lit[4], "four", "numeric key");
var spreadObj = { ...lit, extra: true };
eq(spreadObj.dynKey, 1, "object spread");

// let/const, TDZ
let lv = 1;
{ let lv = 2; eq(lv, 2, "block let"); }
eq(lv, 1, "outer let");
var fns = [];
for (let i = 0; i < 3; i++) fns.push(() => i);
eq(fns.map(f => f()).join(), "0,1,2", "let per iteration");
var tdz = false;
try { useBefore; let useBefore = 1; } catch (e) { tdz = e instanceof ReferenceError; }
eq(tdz, true, "TDZ");
const cst = 5;
var constErr = false;
try { cst = 6; } catch (e) { constErr = e instanceof TypeError; }
eq(constErr, true, "const assign");

// getter this, optional chaining
var oc = { a: { b: { c: 1 } }, f() { return 2; } };
eq(oc?.a?.b?.c, 1, "optional chain");
eq(oc.x?.y.z, undefined, "optional short circuit");
eq(oc.f?.(), 2, "optional call");
eq(oc.g?.(), undefined, "optional call missing");
eq(oc?.["a"]?.b.c, 1, "optional index");

// tagged template
function tag(s, ...v) { return s.raw.join("|") + v.join(","); }
eq(tag`a${1}b${2}c`, "a|b|c1,2", "tagged template");
var nm = "Ala";
eq(`Hej ${nm}, 2+2=${2 + 2}`, "Hej Ala, 2+2=4", "template");
eq(`linia1
linia2`, "linia1\nlinia2", "multiline template");

// getter na klasie + Symbol.iterator
class Range {
  constructor(n) { this.n = n; }
  [Symbol.iterator]() { var i = 0, n = this.n; return { next: () => ({ done: i >= n, value: i++ }) }; }
}
eq([...new Range(4)].join(), "0,1,2,3", "custom iterator");

// new.target, instanceof z bind
function NT() { return new.target === NT; }
eq(new NT() instanceof NT, true, "new.target instance");
eq(NT(), false, "new.target call");

// rekurencja gleboka i przepelnienie stosu
function depth(n) { return n === 0 ? 0 : 1 + depth(n - 1); }
eq(depth(2000), 2000, "deep recursion");
var so = false;
try { (function inf() { inf(); })(); } catch (e) { so = e instanceof RangeError; }
eq(so, true, "stack overflow caught");

// eval, Function
eq(eval("1 + 2"), 3, "eval");
eq(new Function("a", "b", "return a * b")(6, 7), 42, "Function ctor");

// label, break/continue
var out = "";
outer: for (var i = 0; i < 3; i++) {
  for (var j = 0; j < 3; j++) {
    if (j === 1) continue outer;
    if (i === 2) break outer;
    out += i + "" + j + " ";
  }
}
eq(out, "00 10 ", "labels");
done();
function done() { console.log((fails ? "FAILED " + fails : "OK") + " functions (" + count + " tests)"); }
