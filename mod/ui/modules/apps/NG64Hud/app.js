// NG64: Super Mario 64's HUD. The mod (ng64Hud.lua) sends { visible, health, wedges, lives, coins, stars, images }
// as "NG64Hud"; this draws it the way SM64 does, on SM64's 320x240 screen scaled to the window: lives from the left
// edge, stars from the right, coins and the power meter from the middle, the camera icon bottom right. The graphics
// are SM64's own, taken from the player's ROM by the helper into /ng64_cache/hud/ (plain text if they're missing).
angular.module("beamng.apps").directive("ng64Hud", [
  function () {
    return {
      template: '<div class="ng64hud"></div>',
      replace: true,
      link: function (scope, element) {
        "use strict"
        var root = element[0]
        // BeamNG doesn't load an app's app.css on its own: add the styles once
        if (!document.getElementById("ng64hud-style")) {
          var st = document.createElement("style")
          st.id = "ng64hud-style"
          st.textContent =
            ".ng64hud{position:absolute;left:0;top:0;right:0;bottom:0;pointer-events:none;overflow:hidden}" +
            ".ng64hud img{position:absolute;top:0;image-rendering:pixelated}" +
            ".ng64hud-counter,.ng64hud-meter,.ng64hud-camera{position:absolute}" +
            ".ng64hud-meter{transition:top .25s ease-in-out}" +
            ".ng64hud-text{position:absolute;top:0;left:0;white-space:nowrap;color:#fff;font-weight:bold;text-shadow:2px 2px 0 #000}"
          document.head.appendChild(st)
        }
        var IMG = "/ng64_cache/hud/"
        var GLYPH_ADVANCE = 12            // SM64's HUD font: 16 px glyphs, 12 px apart
        var TOP = 15                      // counters' top edge (SM64 HUD_TOP_Y)
        var METER_Y = 74                  // power meter centre, from the top (SM64: 166 from the bottom)
        var METER_HIDDEN_Y = -60          // where it slides to when Mario's at full health
        var METER_HIDE_AFTER_MS = 1500    // full health this long, and the meter goes away (SM64: 45 frames)

        var state = null
        var meterVisible = false
        var fullSince = 0
        var raf = 0

        function el(tag, cls, parent) {
          var e = document.createElement(tag)
          if (cls) e.className = cls
          ;(parent || root).appendChild(e)
          return e
        }

        // the three counters: an icon, "x", then the number, each a row of 16x16 HUD glyphs
        function counter() {
          var c = { box: el("div", "ng64hud-counter") }
          c.icon = el("img", "ng64hud-glyph", c.box)
          c.times = el("img", "ng64hud-glyph", c.box)
          c.digits = []
          c.text = el("span", "ng64hud-text", c.box)
          return c
        }
        var lives = counter(), coins = counter(), stars = counter()
        var meter = { box: el("div", "ng64hud-meter") }
        meter.left = el("img", "ng64hud-meter-half", meter.box)
        meter.right = el("img", "ng64hud-meter-half", meter.box)
        meter.pie = el("img", "ng64hud-pie", meter.box)
        meter.text = el("span", "ng64hud-text ng64hud-meter-text", meter.box)
        var camera = { box: el("div", "ng64hud-camera") }
        camera.cam = el("img", "ng64hud-glyph", camera.box)
        camera.lakitu = el("img", "ng64hud-glyph", camera.box)

        function place(e, x, y, s) {
          e.style.left = x * s + "px"
          e.style.top = y * s + "px"
        }
        function size(e, w, h, s) {
          e.style.width = w * s + "px"
          e.style.height = h * s + "px"
        }

        function drawCounter(c, x, s, iconName, value, showTimes, images) {
          place(c.box, x, TOP, s)
          c.icon.style.display = c.times.style.display = images ? "" : "none"
          c.text.style.display = images ? "none" : ""
          if (!images) {
            c.text.textContent = iconName.toUpperCase() + " x " + value
            c.text.style.fontSize = 10 * s + "px"
            return
          }
          c.icon.src = IMG + iconName + ".png"
          size(c.icon, 16, 16, s)
          c.icon.style.left = "0px"
          c.times.style.display = showTimes ? "" : "none"
          c.times.src = IMG + "times.png"
          size(c.times, 16, 16, s)
          c.times.style.left = 16 * s + "px"
          var str = String(value)
          var start = showTimes ? 32 : 16
          while (c.digits.length < str.length) c.digits.push(el("img", "ng64hud-glyph", c.box))
          for (var i = 0; i < c.digits.length; i++) {
            var d = c.digits[i]
            if (i >= str.length) { d.style.display = "none"; continue }
            d.style.display = ""
            d.src = IMG + "digit_" + str[i] + ".png"
            size(d, 16, 16, s)
            d.style.left = (start + i * GLYPH_ADVANCE) * s + "px"
          }
        }

        function layout() {
          raf = 0
          // measured on the app's slot: this element itself is 0x0 while it's hidden
          var box = root.parentElement || root
          var w = box.clientWidth, h = box.clientHeight
          if (!state || !state.visible || !w || !h) { root.style.display = "none"; return }
          root.style.display = ""
          var s = h / 240                   // SM64 screen units -> pixels
          var cx = w / s / 2                // screen centre, in SM64 units
          var right = w / s
          var images = !!state.images

          drawCounter(lives, 22, s, "mario", state.lives, true, images)
          drawCounter(coins, cx + 8, s, "coin", state.coins, true, images)
          drawCounter(stars, right - 78, s, "star", state.stars, state.stars < 100, images)

          // power meter: SM64's two halves with the pie for however many wedges are left
          var wedges = Math.max(0, Math.min(8, state.wedges | 0))
          var now = Date.now()
          if (wedges < 8) { meterVisible = true; fullSince = 0 }
          else if (meterVisible) {
            if (!fullSince) fullSince = now
            else if (now - fullSince > METER_HIDE_AFTER_MS) meterVisible = false
          }
          place(meter.box, cx - 20 - 32, (meterVisible ? METER_Y : METER_HIDDEN_Y) - 32, s)
          size(meter.box, 64, 64, s)
          meter.left.style.display = meter.right.style.display = images ? "" : "none"
          meter.text.style.display = images ? "none" : ""
          if (images) {
            meter.left.src = IMG + "meter_left.png"
            meter.right.src = IMG + "meter_right.png"
            size(meter.left, 32, 64, s)
            size(meter.right, 32, 64, s)
            meter.left.style.left = "0px"
            meter.right.style.left = 32 * s + "px"
            meter.pie.style.display = wedges > 0 ? "" : "none"
            if (wedges > 0) meter.pie.src = IMG + "pie_" + wedges + ".png"
            size(meter.pie, 32, 32, s)
            meter.pie.style.left = meter.pie.style.top = 16 * s + "px"
          } else {
            meter.pie.style.display = "none"
            meter.text.textContent = "POWER " + wedges
            meter.text.style.fontSize = 10 * s + "px"
          }

          // camera status, bottom right: SM64's camera icon with Lakitu (the only camera NG64 has)
          camera.box.style.display = images ? "" : "none"
          if (images) {
            place(camera.box, right - 54, 205, s)
            camera.cam.src = IMG + "camera.png"
            camera.lakitu.src = IMG + "lakitu.png"
            size(camera.cam, 16, 16, s)
            size(camera.lakitu, 16, 16, s)
            camera.cam.style.left = "0px"
            camera.lakitu.style.left = 16 * s + "px"
          }
          // keep checking while the meter waits to hide
          if (meterVisible && wedges === 8) schedule()
        }

        function schedule() {
          if (!raf) raf = window.requestAnimationFrame(layout)
        }

        scope.$on("NG64Hud", function (event, data) {
          state = data
          schedule()
        })
        window.addEventListener("resize", schedule)
        scope.$on("$destroy", function () {
          window.removeEventListener("resize", schedule)
          if (raf) window.cancelAnimationFrame(raf)
        })
        // ask for the current state straight away rather than waiting for the next periodic update
        if (window.bngApi) bngApi.engineLua("if ng64 and ng64.hud then ng64.hud.resend() end")
        schedule()
      },
    }
  },
])
