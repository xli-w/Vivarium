function renderShell() {
  const page = document.body.dataset.page || "overview";
  const header = document.querySelector("[data-dashboard-header]");
  const nav = document.querySelector("[data-dashboard-nav]");
  const footer = document.querySelector("[data-dashboard-footer]");
  if (header) {
    const eyebrow = header.dataset.eyebrow || "LIBBYS VIVARIUM";
    const title = header.dataset.title || "Dashboard";
    header.innerHTML = `<div><p class="eyebrow">${eyebrow}</p><h1>${title}</h1></div><div class="connection" id="connectionStatus">Connecting</div>`;
  }
  if (nav) {
    const sections = [["overview", "index.html", "Overview"], ["notifications", "notifications.html", "Notifications"], ["history", "history.html", "History"], ["settings", "settings.html", "Settings"]];
    nav.innerHTML = sections.map(([name, href, label]) => `<a href="${href}"${name === page ? ' class="active"' : ""}>${label}</a>`).join("");
  }
  if (footer) {
    footer.innerHTML = '<span><strong>Libbys Vivarium |</strong> <a href="/documentation/operations"> Documentation</a></span><span><a href="mobile.html">Mobile Version </a>| Dashboard / polling <b id="pollRate">--</b></span>';
  }
  if (!document.querySelector('link[rel="manifest"]')) {
    const manifest = document.createElement("link");
    manifest.rel = "manifest";
    manifest.href = "/manifest.json";
    document.head.append(manifest);
  }
  if ("serviceWorker" in navigator) navigator.serviceWorker.register("/sw.js").catch(() => {});
}

renderShell();

let storedPreferences = {};
try { storedPreferences = JSON.parse(localStorage.getItem("terraPreferences") || "{}"); } catch (_) { storedPreferences = {}; }
const state = { token: sessionStorage.getItem("terraApiKey") || "", preferences: storedPreferences };

const $ = (id) => document.getElementById(id);
const text = (id, value) => { const el = $(id); if (el) el.textContent = value; };
const fmt = (value, suffix = "") => value === null || value === undefined || Number.isNaN(Number(value)) ? "--" : `${Number(value).toFixed(1)}${suffix}`;
const age = (seconds) => seconds === null || seconds === undefined ? "--" : seconds < 60 ? `${Math.round(seconds)}s ago` : `${Math.round(seconds / 60)}m ago`;
let selectedHours = Number(state.preferences.historyHours || 6);
let refreshInFlight = false;
let lastHistoryRefresh = 0;
let lastAlarmHistoryRefresh = 0;
let commandsReady = false;
let lastCameraRefresh = 0;
let pollTimer;

function saveDashboardCache(data) {
  try { localStorage.setItem("terraDashboardCache", JSON.stringify({ savedAt: Date.now(), data })); } catch (_) { /* cache is optional */ }
}

function loadDashboardCache() {
  try { return JSON.parse(localStorage.getItem("terraDashboardCache") || "null"); } catch (_) { return null; }
}

function formatTemperature(value) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) return "--";
  const number = Number(value);
  return state.preferences.units === "F" ? `${(number * 9 / 5 + 32).toFixed(1)} °F` : `${number.toFixed(1)} °C`;
}

function formatTime(timestamp) {
  return new Date(timestamp).toLocaleTimeString([], { hour: "numeric", minute: "2-digit", hour12: state.preferences.timeFormat === "12" });
}

function schedulePolling() {
  clearInterval(pollTimer);
  pollTimer = setInterval(refresh, Number(state.preferences.pollSeconds || 5) * 1000);
}

async function api(path, options = {}) {
  const headers = new Headers(options.headers || {});
  if (state.token) headers.set("X-API-Key", state.token);
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 8000);
  let response;
  try {
    response = await fetch(path, { ...options, headers, signal: controller.signal });
  } finally {
    clearTimeout(timeout);
  }
  if (response.status === 401) {
    const token = window.prompt("Enter the TERRA API key to view the dashboard:");
    if (token && token !== state.token) {
      state.token = token;
      sessionStorage.setItem("terraApiKey", token);
      return api(path, options);
    }
  }
  if (!response.ok) throw new Error(await response.text() || `Request failed (${response.status})`);
  return response;
}

function renderAlarm(data) {
  const strip = $("alarmStrip");
  if (!strip) return;
  const health = data.health || {};
  const mainAlarm = data.heartbeat?.alarm;
  const alarm = data.supervisorAlarm || (mainAlarm && mainAlarm !== "NONE" ? { code: mainAlarm, detail: "Main controller alarm" } : null);
  const fresh = health.mainHeartbeatFresh && health.mainTelemetryFresh;
  if (document.body.classList.contains("stale-data")) {
    strip.classList.remove("alarm");
    strip.classList.add("loading");
    text("alarmTitle", "Cached data / stale");
    text("alarmDetail", "The Pi is unavailable; controls remain disabled until live telemetry returns.");
    return;
  }
  strip.classList.toggle("loading", !fresh && !alarm);
  strip.classList.toggle("alarm", Boolean(alarm));
  text("alarmTitle", alarm ? alarm.code || "Alarm" : fresh ? "System Nominal" : "Waiting for telemetry");
  text("alarmDetail", alarm ? alarm.detail || "Attention required" : fresh ? "All monitored systems are reporting normally." : "The dashboard cannot confirm the current system state.");
}

function renderCamera(camera) {
  const frame = $("cameraFrame");
  if (!frame) return;
  const panelHeading = frame.closest(".camera-panel")?.querySelector(".panel-heading");
  if (panelHeading && !$("cameraActions")) {
    const actions = document.createElement("div");
    actions.id = "cameraActions";
    actions.className = "camera-actions";
    actions.innerHTML = '<button type="button" class="quiet-button" id="cameraRefresh">Refresh</button><button type="button" class="quiet-button" id="cameraFullscreen">Fullscreen</button><button type="button" class="quiet-button" id="cameraDownload">Download</button>';
    panelHeading.append(actions);
    $("cameraRefresh").addEventListener("click", () => { lastCameraRefresh = 0; refresh(); });
    $("cameraFullscreen").addEventListener("click", () => frame.requestFullscreen?.());
    $("cameraDownload").addEventListener("click", () => { const image = frame.querySelector("img"); if (image?.src) window.open(image.src, "_blank", "noopener"); });
  }
  let ageOverlay = frame.querySelector(".camera-age");
  if (!ageOverlay) {
    ageOverlay = document.createElement("span");
    ageOverlay.className = "camera-age";
    ageOverlay.id = "cameraAge";
    frame.append(ageOverlay);
  }
  const available = camera?.snapshotAvailable || camera?.streamAvailable;
  const imageAge = lastCameraRefresh ? age((Date.now() - lastCameraRefresh) / 1000) : "loading";
  text("cameraState", available ? (camera.streamAvailable ? "Live stream" : `Snapshot / ${imageAge}`) : "Unavailable");
  text("cameraAge", available && !camera.streamAvailable ? `Image ${imageAge}` : available ? "Live" : "No image");
  if (!available) {
    frame.innerHTML = '<div class="camera-placeholder"><span>CAM</span><p>Camera not configured</p></div>';
    return;
  }
  let img = frame.querySelector("img");
  if (!img) {
    img = document.createElement("img");
    img.alt = "Vivarium camera feed";
    frame.replaceChildren(img);
  }
  if (camera.streamAvailable) {
    let streamUrl = camera.streamUrl;
    if (state.token && streamUrl && streamUrl.startsWith("/api/")) {
      streamUrl += (streamUrl.includes("?") ? "&" : "?") + `token=${encodeURIComponent(state.token)}`;
    }
    if (img.src !== streamUrl) {
      img.src = streamUrl;
      img.onerror = () => {
        img.onerror = () => { frame.innerHTML = '<div class="camera-placeholder"><span>CAM</span><p>Camera unavailable</p></div>'; };
        if (camera.snapshotAvailable) {
          lastCameraRefresh = 0;
          renderCamera({ ...camera, streamAvailable: false });
        } else {
          frame.innerHTML = '<div class="camera-placeholder"><span>CAM</span><p>Stream unavailable</p></div>';
        }
      };
    }
  } else {
    if (!lastCameraRefresh || Date.now() - lastCameraRefresh >= 10000) {
      lastCameraRefresh = Date.now();
      api(`${camera.snapshotUrl}?t=${lastCameraRefresh}`).then((response) => response.blob()).then((blob) => {
        if (img.dataset.objectUrl) URL.revokeObjectURL(img.dataset.objectUrl);
        img.dataset.objectUrl = URL.createObjectURL(blob);
        img.src = img.dataset.objectUrl;
      }).catch(() => { frame.innerHTML = '<div class="camera-placeholder"><span>CAM</span><p>Snapshot unavailable</p></div>'; });
    }
  }
}

function renderTelemetry(telemetry) {
  if (!telemetry) return;
  text("mainState", telemetry.state || "Unknown");
  text("manualMode", telemetry.manual ? "Manual" : "Automatic");
  text("bootId", telemetry.bootId || "--");
  const zones = [["external", "externalTemperatureC", "externalHumidityPct", "externalSensorOk"], ["upper", "upperTemperatureC", "upperHumidityPct", "upperSensorOk"], ["lower", "lowerTemperatureC", "lowerHumidityPct", "lowerSensorOk"]];
  for (const [prefix, temp, hum, ok] of zones) {
    text(`${prefix}Temp`, formatTemperature(telemetry[temp]));
    text(`${prefix}Hum`, `${fmt(telemetry[hum], "%")} RH`);
    text(`${prefix}Health`, telemetry[ok] === true ? "Sensor valid" : telemetry[ok] === false ? "Sensor fault" : "Sensor status unavailable");
  }
  text("soilMoisture", fmt(telemetry.soilMoisturePct, "%"));
  text("soilHealth", telemetry.soilRaw === undefined ? "Sensor status unavailable" : `Raw ${telemetry.soilRaw}`);

  const reservoirCard = $("reservoirCard");
  if (reservoirCard) {
    const isLow = Boolean(telemetry.reservoirLow);
    reservoirCard.classList.toggle("warn", isLow);
    text("reservoirLevel", isLow ? "LOW / REFILL" : "NORMAL / OK");
    text("reservoirStatus", isLow ? "Refill needed (pumps paused)" : "Sufficient water level");
  }

  const drainageLevelCard = $("drainageLevelCard");
  if (drainageLevelCard) {
    const isHigh = Boolean(telemetry.drainageHigh);
    drainageLevelCard.classList.toggle("warn", isHigh);
    text("drainageLevel", isHigh ? "HIGH / DETECTED" : "CLEAR / NORMAL");
    text("drainageLevelStatus", isHigh ? "Water detected in substrate" : "Drainage layer clear");
  }

  const drainagePumpCard = $("drainagePumpCard");
  if (drainagePumpCard) {
    const isPumping = Boolean(telemetry.drainagePump);
    drainagePumpCard.classList.toggle("active", isPumping);
    text("drainagePumpState", isPumping ? "PUMPING" : "STANDBY");
    text("drainagePumpStatus", isPumping ? "Actively draining layer" : "Pump idle");
  }

  const updateWidget = (widgetId, isCurrentActive, badgeId, activeLabel = "ON", idleLabel = "OFF", activeValue = "ON") => {
    const card = $(widgetId);
    if (!card) return;
    card.classList.toggle("active", Boolean(isCurrentActive));
    const badge = $(badgeId);
    if (badge) {
      badge.textContent = isCurrentActive ? activeLabel : idleLabel;
      badge.classList.toggle("on", Boolean(isCurrentActive));
    }
    card.querySelectorAll("[data-value]").forEach((btn) => {
      const isSelected = isCurrentActive ? (btn.dataset.value === activeValue) : (btn.dataset.value === "OFF" || btn.dataset.value === "0");
      btn.classList.toggle("active", isSelected);
      btn.setAttribute("aria-pressed", String(isSelected));
    });
  };

  updateWidget("misterWidget", telemetry.misterPump, "misterStatusBadge");
  updateWidget("foggerWidget", telemetry.fogger, "foggerStatusBadge");
  updateWidget("heaterWidget", telemetry.heater, "heaterStatusBadge");

  const fanPwm = telemetry.fanPwm || 0;
  const fanPct = Math.round(fanPwm / 255 * 100);
  const fanCard = $("fanWidget");
  if (fanCard) {
    fanCard.classList.toggle("active", fanPwm > 0);
    text("fanStatusBadge", `${fanPct}%`);
    const badge = $("fanStatusBadge");
    if (badge) badge.classList.toggle("on", fanPwm > 0);
    fanCard.querySelectorAll("[data-value]").forEach((btn) => {
      const val = Number(btn.dataset.value);
      const isSelected = (val === 0 && fanPwm === 0) || (val > 0 && Math.abs(val - fanPwm) < 60);
      btn.classList.toggle("active", isSelected);
      btn.setAttribute("aria-pressed", String(isSelected));
    });
  }

  updateWidget("feederWidget", telemetry.foodServoActive, "feederStatusBadge", "FEEDING", "REST");

  const manualBtn = $("manualModeBtn");
  if (manualBtn) {
    const manual = Boolean(telemetry.manual);
    manualBtn.classList.toggle("active", manual);
    manualBtn.dataset.value = manual ? "OFF" : "ON";
    manualBtn.setAttribute("aria-pressed", String(manual));
    manualBtn.textContent = manual ? "Disable Manual Mode" : "Enable Manual Mode";
  }
  const manualRemaining = Number(telemetry.manualRemainingSeconds || 0);
  text("manualTimeout", telemetry.manual
    ? `Manual active / ${Math.ceil(manualRemaining / 60)} min remaining`
    : telemetry.manualTimedOut ? "Timed out / automatic mode" : "Automatic mode");

  const interlocksEl = $("interlocks");
  if (interlocksEl) {
    const interlocks = [["Door", telemetry.doorOpen], ["Reservoir low", telemetry.reservoirLow], ["Drainage high", telemetry.drainageHigh]];
    interlocksEl.replaceChildren(...interlocks.map(([label, active]) => { const el = document.createElement("span"); el.className = `interlock${active ? " warn" : ""}`; el.textContent = `${label}: ${active ? "ACTIVE" : "clear"}`; return el; }));
  }

  const actuatorListEl = $("actuatorList");
  if (actuatorListEl) {
    const actuators = [["Mister", telemetry.misterPump], ["Fogger", telemetry.fogger], ["Heater", telemetry.heater], ["Drainage", telemetry.drainagePump], ["Fan", `${Math.round((telemetry.fanPwm || 0) / 255 * 100)}%`], ["Feeder", telemetry.foodServoActive]];
    actuatorListEl.replaceChildren(...actuators.map(([label, value]) => { const el = document.createElement("div"); const active = value === true || (typeof value === "string" && value !== "0%"); el.className = `actuator${active ? " active" : ""}`; const name = document.createElement("span"); name.textContent = label; const state = document.createElement("strong"); state.className = active ? "on" : "off"; state.textContent = typeof value === "boolean" ? (value ? "ON" : "OFF") : value; el.append(name, state); return el; }));
  }
}

function renderDashboard(data, stale = false, cachedAt = null) {
  const health = data.health || {};
  const fresh = health.mainHeartbeatFresh && health.mainTelemetryFresh;
  commandsReady = !stale && fresh && health.mqttConnected;
  document.body.classList.toggle("stale-data", stale);
  const statusEl = $("connectionStatus");
  if (statusEl) {
    statusEl.classList.toggle("offline", !health.mqttConnected);
    text("connectionStatus", stale ? "Cached / stale" : health.mqttConnected ? (fresh ? "MQTT / live" : "MQTT / stale") : "MQTT / offline");
  }
  const mainStatusEl = $("mainStatus");
  if (mainStatusEl) {
    mainStatusEl.className = `tag ${fresh ? "" : "bad"}`;
    text("mainStatus", fresh ? "Online" : "Stale");
  }
  text("heartbeatAge", age(health.mainHeartbeatAgeSeconds));
  text("telemetryAge", age(health.mainTelemetryAgeSeconds));
  text("updatedAt", stale && cachedAt ? `Cached ${new Date(cachedAt).toLocaleString()}` : `Updated ${formatTime(Date.now())}`);
  text("pollRate", `${data.pollSeconds || 5}s`);
  document.querySelectorAll("[data-command], #allOffButton").forEach((button) => {
    button.disabled = stale || !fresh || !health.mqttConnected;
    button.title = button.disabled ? "Controls are unavailable while controller telemetry is stale or MQTT is offline." : "";
  });
  renderAlarm(data);
  renderCamera(data.camera);
  renderTelemetry(data.telemetry);
  renderDiagnostics(data);
}

function renderDiagnostics(data) {
  const diagGrid = $("diagnosticGrid");
  if (!diagGrid) return;
  const health = data.health || {};
  const heartbeat = data.heartbeat || {};
  const telemetry = data.telemetry || {};
  const values = [["MQTT", health.mqttConnected ? "Connected" : "Offline"], ["Heartbeat", health.mainHeartbeatFresh ? "Fresh" : "Stale"], ["Telemetry", health.mainTelemetryFresh ? "Fresh" : "Stale"], ["Database", health.database?.ok ? "Healthy" : "Error"], ["Device", heartbeat.deviceId || "--"], ["Boot ID", heartbeat.bootId || "--"], ["Sequence", `${heartbeat.sequence ?? "--"} / ${telemetry.sequence ?? "--"}`], ["Camera", data.camera?.snapshotAvailable || data.camera?.streamAvailable ? "Configured" : "Not configured"]];
  diagGrid.replaceChildren(...values.map(([label, value]) => { const el = document.createElement("div"); el.className = "diagnostic"; const name = document.createElement("span"); name.textContent = label; const content = document.createElement("strong"); content.textContent = value; el.append(name, content); return el; }));
}

function drawChart(id, series, stateId, suffix) {
  const chart = $(id);
  if (!chart) return;
  const width = 720;
  const height = 220;
  const values = series.flatMap((item) => item.values.map((point) => point.value)).filter((value) => Number.isFinite(value));
  chart.replaceChildren();
  if (!values.length) { text(stateId, "No data"); return; }
  const low = Math.min(...values);
  const high = Math.max(...values);
  const pad = Math.max((high - low) * .12, 1);
  const min = low - pad;
  const max = high + pad;
  [0.25, 0.5, 0.75].forEach((ratio) => { const line = document.createElementNS("http://www.w3.org/2000/svg", "line"); line.setAttribute("x1", "0"); line.setAttribute("x2", String(width)); line.setAttribute("y1", String(height * ratio)); line.setAttribute("y2", String(height * ratio)); line.classList.add("grid-line"); chart.appendChild(line); });
  const firstPoint = series.flatMap((item) => item.values).find((point) => Number.isFinite(point.value));
  const lastPoint = series.flatMap((item) => item.values).reverse().find((point) => Number.isFinite(point.value));
  const addLabel = (x, y, value, anchor = "start") => { const label = document.createElementNS("http://www.w3.org/2000/svg", "text"); label.setAttribute("x", String(x)); label.setAttribute("y", String(y)); label.setAttribute("text-anchor", anchor); label.textContent = value; label.classList.add("chart-axis-label"); chart.appendChild(label); };
  addLabel(4, 14, fmt(max, suffix));
  addLabel(4, height - 4, fmt(min, suffix));
  if (firstPoint?.ts) addLabel(4, height + 18, new Date(firstPoint.ts * 1000).toLocaleString([], { month: "short", day: "numeric", hour: "numeric" }));
  if (lastPoint?.ts) addLabel(width - 4, height + 18, new Date(lastPoint.ts * 1000).toLocaleString([], { month: "short", day: "numeric", hour: "numeric" }), "end");
  series.forEach((item) => {
    const totalSamples = item.values.length;
    const pointsWithX = item.values
      .map((point, i) => ({ x: (i / Math.max(totalSamples - 1, 1)) * width, y: Number.isFinite(point.value) ? height - ((point.value - min) / (max - min)) * height : null }))
      .filter((p) => p.y !== null);
    if (!pointsWithX.length) return;
    if (pointsWithX.length === 1) {
      const circle = document.createElementNS("http://www.w3.org/2000/svg", "circle");
      circle.setAttribute("cx", pointsWithX[0].x.toFixed(1));
      circle.setAttribute("cy", pointsWithX[0].y.toFixed(1));
      circle.setAttribute("r", "4");
      circle.classList.add(item.className);
      chart.appendChild(circle);
    } else {
      const pointsStr = pointsWithX.map((p) => `${p.x.toFixed(1)},${p.y.toFixed(1)}`).join(" ");
      const line = document.createElementNS("http://www.w3.org/2000/svg", "polyline");
      line.setAttribute("points", pointsStr);
      line.classList.add(item.className);
      line.dataset.series = item.className;
      line.setAttribute("aria-label", `${item.label} ${suffix}`);
      const title = document.createElementNS("http://www.w3.org/2000/svg", "title");
      title.textContent = `${item.label}: ${fmt(Math.min(...item.values.map((point) => point.value).filter(Number.isFinite)), suffix)} to ${fmt(Math.max(...item.values.map((point) => point.value).filter(Number.isFinite)), suffix)}`;
      line.appendChild(title);
      if (item.step) {
        const stepped = [];
        pointsWithX.forEach((point, index) => { if (index) stepped.push(`${point.x.toFixed(1)},${stepped[stepped.length - 1].split(",")[1]}`); stepped.push(`${point.x.toFixed(1)},${point.y.toFixed(1)}`); });
        line.setAttribute("points", stepped.join(" "));
      }
      chart.appendChild(line);
    }
  });
  const average = values.reduce((sum, value) => sum + value, 0) / values.length;
  text(stateId, `${values.length} samples / min ${fmt(low, suffix)} / avg ${fmt(average, suffix)} / max ${fmt(high, suffix)}`);
}

async function refreshHistory() {
  if (!$("temperatureChart") && !$("humidityChart")) return;
  const until = Date.now() / 1000;
  try {
    const response = await api(`/api/history?since=${until - selectedHours * 3600}&until=${until}&limit=10000`);
    const rows = await response.json();
    const mainRows = rows.filter((row) => row.source === "main");
    const telemetry = mainRows.map((row) => row.data);
    const points = (key, transform = Number) => telemetry.map((data, index) => ({ ts: mainRows[index]?.ts, value: transform(data[key]) }));
    drawChart("temperatureChart", [{ label: "External", className: "external", values: points("externalTemperatureC") }, { label: "Upper", className: "upper", values: points("upperTemperatureC") }, { label: "Lower", className: "lower", values: points("lowerTemperatureC") }], "temperatureChartState", " °C");
    drawChart("humidityChart", [{ label: "Upper", className: "upper", values: points("upperHumidityPct") }, { label: "Lower", className: "lower", values: points("lowerHumidityPct") }, { label: "Soil", className: "soil", values: points("soilMoisturePct") }], "humidityChartState", " %");
    drawChart("waterChart", [{ label: "Reservoir available", className: "reservoir", step: true, values: points("reservoirLow", (value) => value ? 0 : 1) }, { label: "Drainage high", className: "drainage", step: true, values: points("drainageHigh", (value) => value ? 1 : 0) }, { label: "Drainage pump", className: "pump", step: true, values: points("drainagePump", (value) => value ? 1 : 0) }], "waterChartState", "");
    drawChart("activityChart", [{ label: "Door open", className: "door", step: true, values: points("doorOpen", (value) => value ? 1 : 0) }, { label: "Mister", className: "mister", step: true, values: points("misterPump", (value) => value ? 1 : 0) }, { label: "Fogger", className: "fogger", step: true, values: points("fogger", (value) => value ? 1 : 0) }, { label: "Heater", className: "heater", step: true, values: points("heater", (value) => value ? 1 : 0) }, { label: "Fan", className: "fan", values: points("fanPwm", (value) => value ? Number(value) / 255 : 0) }], "activityChartState", "");
    $("historyRetry")?.setAttribute("hidden", "");
    updateExportLinks(until - selectedHours * 3600, until);
    lastHistoryRefresh = Date.now();
  } catch (error) { text("temperatureChartState", "Unavailable"); text("humidityChartState", error.message); $("historyRetry")?.removeAttribute("hidden"); }
}

function updateExportLinks(since, until) {
  document.querySelectorAll("[data-export]").forEach((link) => {
    const url = new URL(link.dataset.exportUrl || link.getAttribute("href"), window.location.href);
    url.searchParams.set("since", String(since));
    url.searchParams.set("until", String(until));
    link.href = url.pathname + url.search;
  });
}

async function refreshAlarmHistory() {
  const list = $("alarmHistoryList");
  if (!list) return;
  try {
    const response = await api("/api/alarms?limit=100");
    const alarms = await response.json();
    text("alarmHistoryState", `${alarms.length} event${alarms.length === 1 ? "" : "s"}`);
    list.replaceChildren(...(alarms.length ? alarms.map((alarm) => {
      const row = document.createElement("div");
      row.className = `history-alarm-row ${String(alarm.severity || "").toLowerCase()}${alarm.acknowledged ? " acknowledged" : ""}`;
      row.setAttribute("role", "listitem");
      const heading = document.createElement("div");
      heading.className = "history-alarm-heading";
      const title = document.createElement("strong");
      title.textContent = alarm.code || "UNKNOWN";
      const time = document.createElement("time");
      time.dateTime = new Date(alarm.timestamp * 1000).toISOString();
      time.textContent = new Date(alarm.timestamp * 1000).toLocaleString();
      heading.append(title, time);
      const description = document.createElement("span");
      description.textContent = `${alarm.severity || "CRITICAL"} / ${alarm.source || "unknown"} - ${alarm.detail || "No detail recorded"}`;
      row.append(heading, description);
      if (!alarm.acknowledged) {
        const ack = document.createElement("button");
        ack.className = "quiet-button";
        ack.textContent = "Acknowledge";
        ack.addEventListener("click", async () => { try { await api(`/api/notifications/${alarm.id}/ack`, { method: "POST" }); refreshAlarmHistory(); } catch (error) { ack.textContent = `Failed: ${error.message}`; } });
        row.append(ack);
      }
      return row;
    }) : [Object.assign(document.createElement("span"), { className: "muted", textContent: "No alarms recorded for this installation." })]));
  } catch (error) {
    text("alarmHistoryState", "Unavailable");
    list.replaceChildren(Object.assign(document.createElement("span"), { className: "muted", textContent: error.message }));
  }
}

async function refreshEvents() {
  const wantsNotifications = Boolean($('notificationList') || $('emailDeliveryGrid'));
  const wantsAudit = Boolean($('auditList'));
  const wantsTargets = Boolean($('upperTarget'));
  if (!wantsNotifications && !wantsAudit && !wantsTargets) return;
  try {
    const requests = [];
    if (wantsNotifications) requests.push(api('/api/notifications?limit=100').then((response) => response.json()));
    if (wantsAudit) requests.push(api('/api/audit?limit=100').then((response) => response.json()));
    if (wantsTargets) requests.push(api('/api/targets').then((response) => response.json()));
    const results = await Promise.all(requests);
    let resultIndex = 0;
    const notifications = wantsNotifications ? results[resultIndex++] : null;
    const auditRows = wantsAudit ? results[resultIndex++] : null;
    const targetBands = wantsTargets ? results[resultIndex++] : null;
    if (notifications) {
      text('notificationState', `${notifications.active.length} active / email ${notifications.email.configured ? 'ready' : 'not configured'}`);
      const summary = $('notificationSummary');
      if (summary) summary.textContent = `Queue ${notifications.email.queueDepth} / cooldown ${notifications.email.cooldownSeconds}s`;
      if ($('notificationList')) $('notificationList').replaceChildren(...(notifications.events.length ? notifications.events.map((event) => { const row = document.createElement('div'); row.className = `history-alarm-row ${String(event.severity).toLowerCase()}${event.acknowledged ? ' acknowledged' : ''}`; row.setAttribute('role', 'listitem'); row.textContent = `${event.code} - ${event.detail || 'No detail'}${event.acknowledged ? ' (acknowledged)' : ''}`; if (!event.acknowledged) { const button = document.createElement('button'); button.className = 'quiet-button'; button.textContent = 'Acknowledge'; button.onclick = async () => { try { await api(`/api/notifications/${event.id}/ack`, { method: 'POST' }); refreshEvents(); } catch (error) { text('notificationState', `Acknowledge failed: ${error.message}`); } }; row.append(button); } return row; }) : [Object.assign(document.createElement('span'), { className: 'muted', textContent: 'No events recorded.' })]));
    }
    if (notifications && $('emailDeliveryGrid')) {
      const delivery = [["Email", notifications.email.configured ? "Configured" : "Not configured"], ["Queue depth", notifications.email.queueDepth], ["Cooldown", `${notifications.email.cooldownSeconds}s`]];
      $('emailDeliveryGrid').replaceChildren(...delivery.map(([label, value]) => { const row = document.createElement('div'); row.className = 'diagnostic'; row.innerHTML = `<span>${label}</span><strong>${value}</strong>`; return row; }));
      text('emailDeliveryState', notifications.email.configured ? 'Ready' : 'Not configured');
    }
    if (auditRows) {
      text('auditState', `${auditRows.length} records`);
      if ($('auditList')) $('auditList').replaceChildren(...(auditRows.length ? auditRows.map((entry) => { const row = document.createElement('div'); row.className = 'audit-row'; row.textContent = `${new Date(entry.timestamp * 1000).toLocaleString()} / ${entry.command} ${entry.value} / ${entry.published ? 'published' : 'failed'}`; return row; }) : [Object.assign(document.createElement('span'), { className: 'muted', textContent: 'No commands recorded.' })]));
    }
    const severity = { unknown: 0, within: 1, approaching: 2, out: 3 };
    const inlineTargets = targetBands ? [
      ["upperTarget", "upper", targetBands.temperature.values.upper, targetBands.temperature, targetBands.humidity.values.upper, targetBands.humidity],
      ["lowerTarget", "lower", targetBands.temperature.values.lower, targetBands.temperature, targetBands.humidity.values.lower, targetBands.humidity],
      ["externalTarget", "external", targetBands.temperature.values.external, targetBands.temperature, null, null],
      ["soilTarget", "soil", targetBands.soil.values.soil, targetBands.soil, null, null],
    ] : [];
    inlineTargets.forEach(([id, zone, reading, band, humidityReading, humidityBand]) => {
      const indicator = $(id);
      if (!indicator) return;
      const status = humidityReading && severity[humidityReading.status] > severity[reading.status] ? humidityReading.status : reading.status;
      const humidityText = humidityReading ? ` / RH ${humidityBand.min}-${humidityBand.max}${humidityBand.unit} ${humidityReading.status}` : "";
      indicator.textContent = `Target ${band.min}-${band.max}${band.unit} ${reading.status}${humidityText}`;
      indicator.className = `target-indicator ${status}`;
    });
  } catch (error) { text('notificationState', error.message); }
}

async function refresh() {
  if (refreshInFlight) return;
  refreshInFlight = true;
  try {
    const response = await api("/api/dashboard");
    const data = await response.json();
    saveDashboardCache(data);
    renderDashboard(data);
  } catch (error) {
    commandsReady = false;
    const cached = loadDashboardCache();
    if (cached?.data) renderDashboard(cached.data, true, cached.savedAt);
    document.querySelectorAll("[data-command], #allOffButton").forEach((button) => { button.disabled = true; });
    text("connectionStatus", "Dashboard Unavailable");
    const connEl = $("connectionStatus");
    if (connEl) connEl.classList.add("offline");
    text("controlFeedback", error.message);
  }
  const alarmListEl = $("alarmList");
  if (alarmListEl) {
    try {
      const response = await api("/api/alarms?limit=8");
      const alarms = await response.json();
      alarmListEl.replaceChildren(...(alarms.length ? alarms.map((alarm) => { const el = document.createElement("div"); el.className = "alarm-row"; const code = document.createElement("strong"); code.textContent = alarm.code || "UNKNOWN"; const detail = document.createElement("span"); detail.textContent = alarm.detail || "No detail"; el.append(code, detail); return el; }) : [Object.assign(document.createElement("span"), { className: "muted", textContent: "No recent alarms." })]));
    } catch (_) { /* dashboard status already shows the primary failure */ }
  }
  const now = Date.now();
  if (now - lastHistoryRefresh >= 60000) await refreshHistory();
  if (now - lastAlarmHistoryRefresh >= 60000) {
    await refreshAlarmHistory();
    lastAlarmHistoryRefresh = Date.now();
  }
  await refreshEvents();
  refreshInFlight = false;
}

async function sendCommand(command, value, buttonEl) {
  if (!commandsReady || buttonEl?.disabled) {
    text("controlFeedback", "Controls are unavailable until fresh controller telemetry is confirmed.");
    return;
  }
  if (buttonEl) buttonEl.classList.add("publishing");
  text("controlFeedback", `Publishing ${command} request...`);
  try {
    await api("/api/command", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ command, value }) });
    text("controlFeedback", `${command} request published. Waiting for telemetry confirmation.`);
  } catch (error) { text("controlFeedback", error.message); }
  finally { if (buttonEl) buttonEl.classList.remove("publishing"); }
}

document.querySelectorAll("[data-command]").forEach((button) => button.addEventListener("click", () => sendCommand(button.dataset.command, button.dataset.value, button)));
$("allOffButton")?.addEventListener("click", (e) => sendCommand("alloff", "OFF", e.currentTarget));
$("refreshButton")?.addEventListener("click", refresh);
$("diagnosticsRefresh")?.addEventListener("click", refresh);
$("historyRetry")?.addEventListener("click", refreshHistory);
$("preferencesForm")?.addEventListener("submit", (event) => {
  event.preventDefault();
  state.preferences = Object.fromEntries(new FormData(event.currentTarget).entries());
  localStorage.setItem("terraPreferences", JSON.stringify(state.preferences));
  selectedHours = Number(state.preferences.historyHours || 6);
  text("preferenceState", "Saved locally");
  schedulePolling();
  refresh();
});
if ($("preferencesForm")) Object.entries(state.preferences).forEach(([key, value]) => { const input = $("preferencesForm").elements.namedItem(key); if (input) input.value = value; });
document.querySelectorAll("[data-export]").forEach((link) => link.addEventListener("click", async (event) => {
  event.preventDefault();
  const response = await api(link.href);
  const blob = await response.blob();
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a"); anchor.href = url; anchor.download = link.dataset.export; anchor.click(); URL.revokeObjectURL(url);
}));
document.querySelectorAll("[data-hours]").forEach((button) => { button.classList.toggle("active", Number(button.dataset.hours) === selectedHours); button.addEventListener("click", () => { selectedHours = Number(button.dataset.hours); document.querySelectorAll("[data-hours]").forEach((item) => item.classList.toggle("active", item === button)); refreshHistory(); }); });
document.querySelectorAll("[data-chart][data-series]").forEach((button) => button.addEventListener("click", () => {
  const pressed = button.getAttribute("aria-pressed") !== "true";
  button.setAttribute("aria-pressed", String(pressed));
  document.querySelectorAll(`#${button.dataset.chart} [data-series="${button.dataset.series}"]`).forEach((series) => series.classList.toggle("hidden", !pressed));
}));
refresh();
schedulePolling();
