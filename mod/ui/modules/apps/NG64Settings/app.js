// NG64: the settings panel that appears with the Mario HUD. The mod (ng64Ents.lua) sends { pickups, enemies, song, volume }
// as "NG64Settings" (song -1 = music off); changes go back through ng64.setOption. Collapsible to its title bar.
angular.module("beamng.apps").directive("ng64Settings", [
  function () {
    return {
      template: '<div class="ng64set"></div>',
      replace: true,
      link: function (scope, element) {
        "use strict"
        var root = element[0]
        if (!document.getElementById("ng64set-style")) {
          var st = document.createElement("style")
          st.id = "ng64set-style"
          st.textContent =
            ".ng64set{position:absolute;left:0;top:0;right:0;font:13px sans-serif;color:#fff;background:rgba(20,20,28,.82);border-radius:6px;border:1px solid rgba(255,255,255,.2);overflow:hidden;pointer-events:auto;user-select:none}" +
            ".ng64set-head{display:flex;justify-content:space-between;align-items:center;padding:5px 8px;background:rgba(200,40,40,.85);font-weight:bold;cursor:pointer}" +
            ".ng64set-body{padding:8px;display:flex;flex-direction:column;gap:8px}" +
            ".ng64set-row{display:flex;justify-content:space-between;align-items:center;gap:8px}" +
            ".ng64set-body select{width:100%;background:#222;color:#fff;border:1px solid #666;padding:2px}" +
            ".ng64set-body input[type=range]{width:100%}" +
            ".ng64set-hint{opacity:.6;font-size:11px}"
          document.head.appendChild(st)
        }
        var SONGS = ["Bob-omb Battlefield", "Title Theme", "Inside the Castle Walls", "Dire, Dire Docks", "Lethal Lava Land",
          "Koopa's Theme", "Snow Mountain", "Slider", "Haunted House", "Piranha Plant's Lullaby", "Cave Dungeon", "Star Select",
          "Powerful Mario", "Metallic Mario", "Koopa's Message", "Koopa's Road", "High Score", "Merry-Go-Round", "Race Fanfare",
          "Star Appears", "Stage Boss", "Key Get", "Endless Stairs", "Ultimate Koopa", "Staff Roll", "Puzzle Solved",
          "Toad's Message", "Peach's Message", "Opening", "Ultimate Victory", "Ending", "File Select", "Lakitu", "Star Get"]
        var collapsed = false
        try { collapsed = localStorage.getItem("ng64set.collapsed") === "1" } catch (e) {}

        function mk(tag, cls, parent, html) {
          var e = document.createElement(tag)
          if (cls) e.className = cls
          if (html) e.innerHTML = html
          ;(parent || root).appendChild(e)
          return e
        }
        function lua(call) { bngApi.engineLua("extensions.ng64." + call) }

        var head = mk("div", "ng64set-head", root, "<span>NG64 Settings</span>")
        var arrow = mk("span", "", head)
        var body = mk("div", "ng64set-body", root)

        function toggleRow(label, key, hint) {
          var row = mk("label", "ng64set-row", body)
          mk("span", "", row, label + (hint ? ' <span class="ng64set-hint">(' + hint + ")</span>" : ""))
          var cb = mk("input", "", row)
          cb.type = "checkbox"
          cb.onchange = function () { lua("setOption('" + key + "', " + (cb.checked ? "true" : "false") + ")") }
          return cb
        }
        var pick = toggleRow("Pickups", "pickups", "P")
        var enem = toggleRow("Enemies", "enemies", "O")

        mk("div", "", body, "Music")
        var sel = mk("select", "", body)
        var off = mk("option", "", sel, "Off")
        off.value = "-1"
        SONGS.forEach(function (n, i) { var o = mk("option", "", sel, n); o.value = String(i) })
        sel.onchange = function () { lua("setOption('song', " + sel.value + ")") }

        var volLabel = mk("div", "", body, "Volume")
        var vol = mk("input", "", body)
        vol.type = "range"; vol.min = 0; vol.max = 100
        vol.oninput = function () { volLabel.textContent = "Volume " + vol.value + "%" }
        vol.onchange = function () { lua("setOption('volume', " + vol.value + ")") }

        function applyCollapse() {
          body.style.display = collapsed ? "none" : "flex"
          arrow.textContent = collapsed ? "▸" : "▾"
        }
        head.onclick = function () {
          collapsed = !collapsed
          try { localStorage.setItem("ng64set.collapsed", collapsed ? "1" : "0") } catch (e) {}
          applyCollapse()
        }
        applyCollapse()

        function show(o) {
          pick.checked = !!o.pickups
          enem.checked = !!o.enemies
          sel.value = String(o.song)
          vol.value = o.volume
          volLabel.textContent = "Volume " + o.volume + "%"
        }
        scope.$on("NG64Settings", function (ev, o) { scope.$evalAsync(function () { show(o) }) })
        lua("resendSettings()")
        scope.$on("$destroy", function () {})
      },
    }
  },
])
