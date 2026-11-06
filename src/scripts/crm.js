(function (global) {
  var USER_KEY = "crm.user";
  var RIDES_KEY = "crm.rides";
  var JOINED_KEY = "crm.joined";

  var KICKERS = {
    success: "成功",
    unverified: "未验证",
    direction: "方向或日期不符",
    tolerance: "超出 ±15 分钟",
    full: "人数已满",
    conflict: "时间段冲突"
  };

  var DETAILS = {
    success: "操作已完成。",
    unverified: "未登录或尚未通过学生身份验证，不能发布或加入行程。",
    direction: "只匹配同方向、同一日期的行程。相反方向或不同日期不能成为候选。",
    tolerance: "出发时间差大于 15 分钟，不能匹配。",
    full: "拼车人数含组织者已达到该行程容量，不能再加入。",
    conflict: "该出发时间与已有行程重叠，不能同时加入。"
  };

  function read(key, fallback) {
    try {
      var raw = localStorage.getItem(key);
      return raw ? JSON.parse(raw) : fallback;
    } catch (err) {
      return fallback;
    }
  }

  function write(key, value) {
    localStorage.setItem(key, JSON.stringify(value));
  }

  function appRoot() {
    var scripts = document.getElementsByTagName("script");
    for (var i = 0; i < scripts.length; i += 1) {
      var src = scripts[i].src || "";
      var mark = "/scripts/crm.js";
      var at = src.indexOf(mark);
      if (at !== -1) return src.slice(0, at + 1);
    }
    return "";
  }

  function isVerified(user) {
    return !!(user && user.status === "verified");
  }

  function toMinutes(hhmm) {
    var parts = String(hhmm || "").split(":");
    var hour = Number(parts[0]);
    var minute = Number(parts[1]);
    if (!isFinite(hour) || !isFinite(minute)) return null;
    return hour * 60 + minute;
  }

  function windowsOverlap(left, right) {
    if (!left || !right || left.date !== right.date) return false;
    var leftMid = toMinutes(left.time);
    var rightMid = toMinutes(right.time);
    if (leftMid === null || rightMid === null) return false;
    var leftTol = Number(left.tolerance);
    var rightTol = Number(right.tolerance);
    if (!isFinite(leftTol)) leftTol = 15;
    if (!isFinite(rightTol)) rightTol = 15;
    return leftMid - leftTol <= rightMid + rightTol && rightMid - rightTol <= leftMid + leftTol;
  }

  function sameDirection(left, right) {
    return left.date === right.date &&
      left.origin === right.origin &&
      left.destination === right.destination;
  }

  function showResult(node, code, detail) {
    if (!node) return;
    node.hidden = false;
    node.className = "result result--" + code;
    node.replaceChildren();
    var kicker = document.createElement("span");
    kicker.className = "result-kicker";
    kicker.textContent = KICKERS[code] || code;
    var text = document.createElement("span");
    text.className = "result-text";
    text.textContent = detail || DETAILS[code] || "";
    node.append(kicker, text);
  }

  function mountNav() {
    var root = appRoot();
    var user = read(USER_KEY, null);
    var verified = isVerified(user);
    var links = document.querySelectorAll("[data-href]");
    for (var i = 0; i < links.length; i += 1) {
      var link = links[i];
      link.setAttribute("href", root + link.dataset.href);
      if (link.classList.contains("nav-link") && document.body.dataset.page === link.dataset.nav) {
        link.setAttribute("aria-current", "page");
      } else {
        link.removeAttribute("aria-current");
      }
      if (link.dataset.nav === "publish" && !verified) {
        link.setAttribute("aria-disabled", "true");
        link.setAttribute("tabindex", "-1");
        link.classList.add("is-disabled");
        link.title = "未通过学生身份验证，发布入口不可用";
      } else if (link.dataset.nav === "publish") {
        link.removeAttribute("aria-disabled");
        link.removeAttribute("tabindex");
        link.classList.remove("is-disabled");
        link.removeAttribute("title");
      }
    }
    var badge = document.getElementById("verify-badge");
    if (!badge) return;
    badge.className = "badge";
    if (!user) {
      badge.textContent = "未登录";
      badge.classList.add("badge--muted");
    } else if (verified) {
      badge.textContent = "已验证";
      badge.classList.add("badge--ok");
    } else {
      badge.textContent = "未审核";
      badge.classList.add("badge--warn");
    }
  }

  document.addEventListener("click", function (event) {
    var link = event.target.closest ? event.target.closest("[data-nav='publish']") : null;
    if (link && link.getAttribute("aria-disabled") === "true") event.preventDefault();
  });

  document.addEventListener("DOMContentLoaded", mountNav);

  global.CRM = {
    getUser: function () { return read(USER_KEY, null); },
    saveUser: function (user) { write(USER_KEY, user); mountNav(); },
    getRides: function () { return read(RIDES_KEY, []); },
    saveRides: function (rides) { write(RIDES_KEY, rides); },
    getJoined: function () { return read(JOINED_KEY, []); },
    saveJoined: function (rides) { write(JOINED_KEY, rides); },
    isVerified: isVerified,
    toMinutes: toMinutes,
    windowsOverlap: windowsOverlap,
    sameDirection: sameDirection,
    showResult: showResult,
    mountNav: mountNav
  };
})(window);
