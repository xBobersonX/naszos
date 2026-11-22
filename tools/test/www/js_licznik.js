// Skrypt dolaczany przez <script src="js_licznik.js"> w <head> - wiec strona
// jeszcze nie istnieje. Czekamy na DOMContentLoaded.
console.log("js_licznik.js: skrypt wczytany, czekam na strone");

document.addEventListener("DOMContentLoaded", function () {
  var info = document.getElementById("info");
  info.innerHTML = "Skrypt dziala! <b>" + navigator.userAgent + "</b>";

  // licznik
  var n = 0;
  var wynik = document.getElementById("wynik");
  function pokaz() {
    wynik.textContent = n;
    wynik.style.color = n < 0 ? "crimson" : n > 0 ? "green" : "";
  }
  document.getElementById("plus").onclick = function () { n++; pokaz(); };
  document.getElementById("minus").onclick = function () { n--; pokaz(); };
  document.getElementById("zero").addEventListener("click", function () { n = 0; pokaz(); });

  // kolory
  var tlo = document.getElementById("tlo");
  document.querySelectorAll(".kolor").forEach(function (b) {
    b.addEventListener("click", function () {
      tlo.style.background = b.dataset.kolor;
      tlo.textContent = "Kolor: " + b.textContent;
    });
  });
  document.getElementById("losuj").addEventListener("click", function () {
    var c = "#" + Math.floor(Math.random() * 0xFFFFFF).toString(16).padStart(6, "0");
    tlo.style.background = c;
    tlo.textContent = "Wylosowany kolor: " + c;
  });

  // lista zakupow
  var lista = document.getElementById("lista");
  var pole = document.getElementById("nowy");
  function dodaj() {
    var tekst = pole.value.trim();
    if (!tekst) {
      alert("Najpierw wpisz, co kupic!");
      return;
    }
    var li = document.createElement("li");
    li.textContent = tekst;
    li.addEventListener("click", function () { li.classList.toggle("zrobione"); });
    lista.appendChild(li);
    pole.value = "";
    console.log("Dodano do listy: " + tekst + " (razem " + lista.children.length + ")");
  }
  document.getElementById("dodaj").addEventListener("click", dodaj);
  pole.addEventListener("keydown", function (e) { if (e.key === "Enter") dodaj(); });

  // stoper
  var start = 0, timer = null, stoper = document.getElementById("stoper");
  document.getElementById("start").addEventListener("click", function () {
    if (timer) {
      clearInterval(timer);
      timer = null;
      return;
    }
    start = Date.now();
    timer = setInterval(function () {
      stoper.textContent = ((Date.now() - start) / 1000).toFixed(1) + " s";
    }, 100);
  });
});
