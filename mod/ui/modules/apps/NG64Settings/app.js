// NG64: the settings panel that appears with the Mario HUD. The mod (ng64Ents.lua) sends { pickups, enemies, song, volume }
// as "NG64Settings" (song -1 = music off) whenever they change - from here, or from the keys; changes go back through
// ng64.setOption. Two tabs (Settings, Debug), a UI scale, and it collapses to its title bar.
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
            ".ng64set{position:absolute;left:0;top:0;width:230px;box-sizing:border-box;font:13px sans-serif;color:#fff;background:rgba(20,20,28,.82);border-radius:6px;border:1px solid rgba(255,255,255,.2);overflow:hidden;pointer-events:auto;user-select:none;transform-origin:0 0}" +
            ".ng64set-head{display:flex;justify-content:space-between;align-items:center;padding:5px 8px;background:rgba(200,40,40,.85);font-weight:bold;cursor:pointer}" +
            ".ng64set-tabs{display:flex;border-bottom:1px solid rgba(255,255,255,.2)}" +
            ".ng64set-tab{flex:1;text-align:center;padding:4px 0;cursor:pointer;opacity:.6}" +
            ".ng64set-tab.on{opacity:1;background:rgba(255,255,255,.12)}" +
            ".ng64set-body{padding:8px;display:flex;flex-direction:column;gap:8px}" +
            ".ng64set-row{display:flex;justify-content:space-between;align-items:center;gap:8px}" +
            ".ng64set-body input[type=range]{width:100%}" +
            ".ng64set-hint{opacity:.6;font-size:11px}" +
            ".ng64set-pick{background:#222;border:1px solid #666;padding:3px 6px;cursor:pointer;display:flex;justify-content:space-between}" +
            ".ng64set-list{max-height:150px;overflow-y:auto;background:#1a1a1a;border:1px solid #666;border-top:none}" +
            ".ng64set-item{padding:3px 6px;cursor:pointer}" +
            ".ng64set-item:hover{background:rgba(200,40,40,.6)}" +
            ".ng64set-item.on{background:rgba(255,255,255,.18)}" +
            ".ng64set-steps{display:flex;gap:6px}" +
            ".ng64set-btn{flex:1;text-align:center;background:#333;border:1px solid #666;border-radius:3px;padding:3px 4px;cursor:pointer}" +
            ".ng64set-btn:hover{background:rgba(200,40,40,.6)}" +
            ".ng64set-grid{display:grid;grid-template-columns:1fr 1fr;gap:5px}" +
            ".ng64set-sub{opacity:.7;font-size:11px;margin-top:2px}"
          document.head.appendChild(st)
        }
        // the helper's songs, in its order (main.c s_songs)
        var SONGS = ["Bob-omb Battlefield", "Title Theme", "Inside the Castle Walls", "Dire, Dire Docks", "Lethal Lava Land",
          "Koopa's Theme", "Snow Mountain", "Slider", "Haunted House", "Piranha Plant's Lullaby", "Cave Dungeon", "Star Select",
          "Powerful Mario", "Metallic Mario", "Koopa's Message", "Koopa's Road", "High Score", "Merry-Go-Round", "Race Fanfare",
          "Star Appears", "Stage Boss", "Key Get", "Endless Stairs", "Ultimate Koopa", "Staff Roll", "Puzzle Solved",
          "Toad's Message", "Peach's Message", "Opening", "Ultimate Victory", "Ending", "File Select", "Lakitu", "Star Get"]
        function load(k, d) { try { var v = localStorage.getItem("ng64set." + k); return v === null ? d : v } catch (e) { return d } }
        function save(k, v) { try { localStorage.setItem("ng64set." + k, String(v)) } catch (e) {} }
        var collapsed = load("collapsed", "0") === "1"
        var tab = load("tab", "settings")
        var scale = parseFloat(load("scale", "1")) || 1

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
        var tabs = mk("div", "ng64set-tabs", root)
        var tabSet = mk("div", "ng64set-tab", tabs, "Settings")
        var tabDbg = mk("div", "ng64set-tab", tabs, "Debug")
        var body = mk("div", "ng64set-body", root)
        var dbg = mk("div", "ng64set-body", root)

        // ---- settings -------------------------------------------------------------------------------------------
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

        // the music: BeamNG's UI doesn't open a native <select> in an app, so it is a list of its own
        mk("div", "", body, "Music")
        var picker = mk("div", "ng64set-pick", body)
        var pickName = mk("span", "", picker, "Off")
        mk("span", "", picker, "▾")
        var list = mk("div", "ng64set-list", body)
        list.style.display = "none"
        var song = -1
        var items = []
        function addItem(i, name) {
          var it = mk("div", "ng64set-item", list, name)
          it.onclick = function (ev) {
            ev.stopPropagation()
            list.style.display = "none"
            setTimeout(fitBox, 0)
            setSong(i)
          }
          items.push({ i: i, el: it })
        }
        addItem(-1, "Off")
        SONGS.forEach(function (n, i) { addItem(i, n) })
        picker.onclick = function () {
          var open = list.style.display === "none"
          list.style.display = open ? "block" : "none"
          setTimeout(fitBox, 0)
          if (open) items.forEach(function (x) { if (x.i === song) x.el.scrollIntoView({ block: "nearest" }) })
        }
        var steps = mk("div", "ng64set-steps", body)
        var prevB = mk("div", "ng64set-btn", steps, "◀ Prev")
        var nextB = mk("div", "ng64set-btn", steps, "Next ▶")
        prevB.onclick = function () { setSong(song <= 0 ? SONGS.length - 1 : song - 1) }
        nextB.onclick = function () { setSong(song < 0 || song >= SONGS.length - 1 ? 0 : song + 1) }
        function showSong(i) {
          song = i
          pickName.textContent = i < 0 ? "Off" : SONGS[i] || ("Song " + (i + 1))
          items.forEach(function (x) { x.el.className = "ng64set-item" + (x.i === i ? " on" : "") })
        }
        function setSong(i) { showSong(i); lua("setOption('song', " + i + ")") }

        var volLabel = mk("div", "", body, "Volume")
        var vol = mk("input", "", body)
        vol.type = "range"; vol.min = 0; vol.max = 100
        vol.oninput = function () { volLabel.textContent = "Volume " + vol.value + "%" }
        vol.onchange = function () { lua("setOption('volume', " + vol.value + ")") }

        // the whole panel's size
        var scaleLabel = mk("div", "", body, "")
        var scl = mk("input", "", body)
        scl.type = "range"; scl.min = 75; scl.max = 250; scl.step = 5
        scl.value = Math.round(scale * 100)
        // the app's own box in the layout is fitted to the panel (at its scale), so nothing is cut off
        // (the layout sizes a container a few levels up, which clips: every box up to the one the layout sized is fitted)
        function fitBox() {
          var w = Math.ceil(230 * scale) + "px", h = Math.ceil(root.offsetHeight * scale) + "px"
          for (var el = root.parentElement, k = 0; el && k < 5; el = el.parentElement, k++) {
            el.style.overflow = "visible"
            el.style.minWidth = w
            el.style.minHeight = h
            if (el.style.width) { el.style.width = w; el.style.height = h; break }
          }
        }
        function applyScale() {
          root.style.transform = "scale(" + scale + ")"
          scaleLabel.textContent = "UI scale " + Math.round(scale * 100) + "%"
          fitBox()
        }
        scl.oninput = function () { scale = scl.value / 100; applyScale() }
        scl.onchange = function () { save("scale", scale) }

        // ---- debug: spawners ---------------------------------------------------------------------------------------
        function spawners(title, list) {
          mk("div", "ng64set-sub", dbg, title)
          var grid = mk("div", "ng64set-grid", dbg)
          list.forEach(function (s) {
            var b = mk("div", "ng64set-btn", grid, s[0])
            b.onclick = function () { lua("debugSpawn(" + s[1] + ")") }
          })
        }
        spawners("Pickups (in front of Mario)", [["Coin", 1], ["Red Coin", 2], ["Blue Coin", 3], ["Power Star", 4],
          ["Metal Cap", 5], ["Wing Cap", 6], ["Invincibility", 7], ["Red Coin Course", 199]])
        spawners("Enemies", [["Goomba", 20], ["Bob-omb", 21], ["Koopa", 22]])
        mk("div", "ng64set-hint", dbg, "Pickups / enemies switched off clear what was spawned.")

        // ---- layout ----------------------------------------------------------------------------------------------------
        function apply() {
          tabs.style.display = collapsed ? "none" : "flex"
          body.style.display = !collapsed && tab === "settings" ? "flex" : "none"
          dbg.style.display = !collapsed && tab === "debug" ? "flex" : "none"
          tabSet.className = "ng64set-tab" + (tab === "settings" ? " on" : "")
          tabDbg.className = "ng64set-tab" + (tab === "debug" ? " on" : "")
          arrow.textContent = collapsed ? "▸" : "▾"
          setTimeout(fitBox, 0)
        }
        head.onclick = function () { collapsed = !collapsed; save("collapsed", collapsed ? "1" : "0"); apply() }
        tabSet.onclick = function () { tab = "settings"; save("tab", tab); apply() }
        tabDbg.onclick = function () { tab = "debug"; save("tab", tab); apply() }
        apply()
        applyScale()

        function show(o) {
          pick.checked = !!o.pickups
          enem.checked = !!o.enemies
          showSong(typeof o.song === "number" ? o.song : -1)
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
