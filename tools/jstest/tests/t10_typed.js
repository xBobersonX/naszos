// Tablice typowane: ArrayBuffer, Int8Array..Float64Array, DataView, TextEncoder/TextDecoder
var fails = 0, passed = 0;
function check(name, got, exp) {
  if (got === exp) passed++;
  else { fails++; console.log("FAIL " + name + "\n  got: " + got + "\n  exp: " + exp); }
}
var u8 = new Uint8Array(4);
check("dlugosc", u8.length, 4);
check("zera", u8.join(), "0,0,0,0");
u8[0] = 300; u8[1] = -1; u8[2] = 1.7; u8[9] = 5;
check("Uint8 modulo", u8.join(), "44,255,1,0");
check("poza zakresem", u8[9], undefined);
var i8 = new Int8Array([127, 128, -129]);
check("Int8", i8.join(), "127,-128,127");
var c = new Uint8ClampedArray([300, -5, 1.5, 2.5, 0.5]);
check("Clamped", c.join(), "255,0,2,2,0");
var f32 = new Float32Array([0.1]);
check("Float32", f32[0] !== 0.1 && Math.abs(f32[0] - 0.1) < 1e-7, true);
check("Float64", new Float64Array([0.1])[0], 0.1);
check("Uint32", new Uint32Array([-1])[0], 4294967295);
check("Int16", new Int16Array([40000])[0], -25536);

// wspolny bufor
var buf = new ArrayBuffer(8);
check("byteLength", buf.byteLength, 8);
var a = new Uint8Array(buf), b = new Uint32Array(buf, 4, 1);
b[0] = 0x01020304;
check("widok na bufor (little endian)", a.join(), "0,0,0,0,4,3,2,1");
check("byteOffset", b.byteOffset + "," + b.byteLength + "," + (b.buffer === buf), "4,4,true");
var sub = a.subarray(4, 6); sub[0] = 99;
check("subarray dzieli pamiec", a[4], 99);
check("slice kopiuje", (function () { var s = a.slice(4, 6); s[0] = 1; return a[4] + "," + s.length; })(), "99,2");
check("ArrayBuffer.slice", new Uint8Array(buf.slice(4)).join(), "99,3,2,1");
check("isView", ArrayBuffer.isView(a) + "," + ArrayBuffer.isView(buf), "true,false");
var bad = ""; try { new Uint32Array(buf, 1); } catch (e) { bad = e.name; }
check("zly offset", bad, "RangeError");

// metody
var t = new Int32Array([5, 1, 4, 2, 3]);
check("sort liczbowo", t.sort().join(), "1,2,3,4,5");
check("sort z funkcja", new Int32Array([1, 3, 2]).sort(function (x, y) { return y - x; }).join(), "3,2,1");
check("map daje typowana", t.map(function (x) { return x * 100; }) instanceof Int32Array, true);
check("map", t.map(function (x) { return x * 2; }).join(), "2,4,6,8,10");
check("filter", t.filter(function (x) { return x & 1; }).join(), "1,3,5");
check("reduce", t.reduce(function (s, x) { return s + x; }, 0), 15);
check("indexOf/includes", t.indexOf(4) + "," + t.includes(9), "3,false");
check("fill", new Uint8Array(3).fill(7).join(), "7,7,7");
check("reverse", new Uint8Array([1, 2, 3]).reverse().join(), "3,2,1");
var s = new Uint8Array(5); s.set([1, 2], 1); s.set(new Uint8Array([9]), 4);
check("set", s.join(), "0,1,2,0,9");
check("from/of", Uint16Array.from([1, 2], function (x) { return x * 3; }).join() + "|" + Int8Array.of(1, -1).join(), "3,6|1,-1");
check("spread", [...new Uint8Array([4, 5])].join(), "4,5");
var it = ""; for (var v of new Uint8Array([7, 8])) it += v;
check("for-of", it, "78");
check("entries", Array.from(new Uint8Array([6]).entries())[0].join(), "0,6");
check("Array.from", Array.from(new Uint8Array([1, 2])).length, 2);
check("Object.keys", Object.keys(new Uint8Array(2)).join(), "0,1");
check("in", (1 in new Uint8Array(2)) + "," + (5 in new Uint8Array(2)), "true,false");
check("BYTES_PER_ELEMENT", Float64Array.BYTES_PER_ELEMENT + "," + new Int16Array(1).BYTES_PER_ELEMENT, "8,2");
check("Array.isArray", Array.isArray(new Uint8Array(1)), false);
var nn = ""; try { Uint8Array(2); } catch (e) { nn = e.name; }
check("bez new", nn, "TypeError");

// DataView
var dv = new DataView(new ArrayBuffer(8));
dv.setUint16(0, 0x1234);
dv.setUint16(2, 0x1234, true);
dv.setFloat32(4, 1.5);
var bytes = new Uint8Array(dv.buffer);
check("DataView endian", bytes[0] + "," + bytes[1] + "," + bytes[2] + "," + bytes[3], "18,52,52,18");
check("DataView get", dv.getUint16(0) + "," + dv.getUint16(2, true) + "," + dv.getFloat32(4), "4660,4660,1.5");
check("DataView Int8", (dv.setInt8(0, -2), dv.getInt8(0)), -2);
var oob = ""; try { dv.getUint32(6); } catch (e) { oob = e.name; }
check("DataView zakres", oob, "RangeError");

// TextEncoder / TextDecoder
var enc = new TextEncoder().encode("zażółć");
check("TextEncoder", enc instanceof Uint8Array && enc.length, 10);
check("TextDecoder", new TextDecoder().decode(enc), "zażółć");
check("decode bufora", new TextDecoder("utf-8").decode(new Uint8Array([104, 105]).buffer), "hi");

if (fails) console.log("FAILED typed: " + fails + " bledow, " + passed + " ok");
else console.log("OK typed (" + passed + " checks)");
