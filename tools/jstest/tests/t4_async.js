// Promise, mikrozadania, timery - kolejnosc jak w przegladarce
var log = [];
setTimeout(() => log.push("timeout"), 0);
Promise.resolve(1).then(v => log.push("then" + v));
queueMicrotask(() => log.push("micro"));
log.push("sync");
new Promise((res, rej) => rej(new Error("x"))).catch(e => log.push("caught " + e.message));
Promise.all([1, Promise.resolve(2), new Promise(r => setTimeout(() => r(3), 10))])
  .then(v => log.push("all " + v.join()));
Promise.allSettled([Promise.reject("no"), 5]).then(r => log.push("settled " + r.map(x => x.status).join()));
Promise.race([new Promise(r => setTimeout(() => r("slow"), 50)), Promise.resolve("fast")]).then(v => log.push("race " + v));
Promise.resolve(1).then(v => { throw v + 1; }).then(() => log.push("bad")).catch(v => log.push("chain " + v)).finally(() => log.push("fin"));
var thenable = { then(res) { res(42); } };
Promise.resolve(thenable).then(v => log.push("thenable " + v));
var iv = 0, id = setInterval(() => { if (++iv === 3) clearInterval(id); }, 5);
setTimeout((a, b) => log.push("args " + a + b), 1, "x", "y");
requestAnimationFrame(t => log.push("raf " + (typeof t)));

setTimeout(() => {
  var expected = "sync,then1,micro,caught x,settled rejected,fulfilled,race fast,thenable 42,chain 2,fin,timeout,args xy,all 1,2,3,raf number";
  var got = log.join(",");
  if (iv !== 3) console.log("FAIL interval count " + iv);
  if (got === expected) console.log("OK async (" + log.length + " steps)");
  else console.log("FAILED async\n got:      " + got + "\n expected: " + expected);
}, 200);
