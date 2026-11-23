(() => {
  const tabs = [...document.querySelectorAll('[role="tab"]')];
  const panels = [...document.querySelectorAll("[data-panel]")];
  tabs.forEach((tab, index) => {
    tab.addEventListener("click", () => activateTab(tab));
    tab.addEventListener("keydown", (event) => {
      if (!["ArrowRight", "ArrowLeft", "Home", "End"].includes(event.key))
        return;
      event.preventDefault();
      let next = index;
      if (event.key === "ArrowRight") next = (index + 1) % tabs.length;
      if (event.key === "ArrowLeft")
        next = (index - 1 + tabs.length) % tabs.length;
      if (event.key === "Home") next = 0;
      if (event.key === "End") next = tabs.length - 1;
      tabs[next].focus();
      activateTab(tabs[next]);
    });
  });
  function activateTab(selected) {
    tabs.forEach((tab) => {
      const active = tab === selected;
      tab.classList.toggle("is-active", active);
      tab.setAttribute("aria-selected", String(active));
      tab.tabIndex = active ? 0 : -1;
    });
    panels.forEach((panel) => {
      panel.hidden = panel.dataset.panel !== selected.dataset.tab;
    });
  }
  tabs.forEach((tab) => {
    tab.tabIndex = tab.dataset.tab === "auto" ? 0 : -1;
  });

  const throttle = document.getElementById("throttle");
  const output = document.getElementById("throttle-output");
  const rpm = document.getElementById("rpm-value");
  const speed = document.getElementById("speed-value");
  const temp = document.getElementById("temp-value");
  const voltage = document.getElementById("voltage-value");
  const sampleCount = document.getElementById("sample-count");
  let samples = 2;
  let paused = false;
  function updateReadings() {
    const value = Number(throttle.value);
    const rpmValue = Math.round(760 + value * 51.5);
    const speedValue = Math.round(value * 1.28);
    const tempValue = (85.4 + value * 0.16).toFixed(1);
    const voltageValue = (14.15 - value * 0.002).toFixed(1);
    output.value = value;
    output.textContent = value;
    rpm.textContent = rpmValue.toLocaleString("es-MX");
    speed.textContent = speedValue;
    temp.textContent = tempValue;
    voltage.textContent = voltageValue;
    document.getElementById("rpm-meter").style.width =
      `${Math.min(100, 8 + value * 0.88)}%`;
    document.getElementById("speed-meter").style.width =
      `${Math.min(100, value * 0.84)}%`;
    document.getElementById("gauge-rpm").textContent =
      `${(rpmValue / 1000).toFixed(1)}k`;
    document.getElementById("gauge-speed").textContent = speedValue;
    document.getElementById("chart-current").textContent =
      `${rpmValue.toLocaleString("es-MX")} rpm`;
    document
      .getElementById("chart-bar-live")
      .style.setProperty(
        "--h",
        `${Math.max(10, Math.min(96, value * 0.78 + 12))}%`,
      );
  }
  throttle.addEventListener("input", () => {
    if (paused) return;
    updateReadings();
    samples += 1;
    sampleCount.textContent = String(samples).padStart(2, "0");
  });
  const pauseButton = document.getElementById("pause-sim");
  pauseButton.addEventListener("click", () => {
    paused = !paused;
    pauseButton.setAttribute("aria-pressed", String(paused));
    pauseButton.innerHTML = paused
      ? "▶ <span>Reanudar demo</span>"
      : "Ⅱ <span>Pausar demo</span>";
    throttle.disabled = paused;
    throttle.setAttribute("aria-disabled", String(paused));
  });
  const clock = document.getElementById("demo-clock");
  const updateClock = () => {
    clock.textContent = new Intl.DateTimeFormat("es-MX", {
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
      hour12: false,
    }).format(new Date());
  };
  updateClock();
  window.setInterval(updateClock, 1000);
})();
